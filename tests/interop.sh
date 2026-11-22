#!/usr/bin/env bash
# Interoperability tests: every scenario is checked against the real `git`
# binary, which is the only authority on whether a repository is valid.
set -euo pipefail

MG="$(cd "$(dirname "$0")/.." && pwd)/bin/minigit"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

export GIT_AUTHOR_NAME="Test User" GIT_AUTHOR_EMAIL="test@example.com"
export GIT_COMMITTER_NAME="Test User" GIT_COMMITTER_EMAIL="test@example.com"
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null

pass=0
ok()   { pass=$((pass + 1)); echo "  ok   $1"; }
fail() { echo "  FAIL $1"; exit 1; }
check_eq() { [ "$2" = "$3" ] && ok "$1" || { echo "    got:  $2"; echo "    want: $3"; fail "$1"; }; }

# ---------------------------------------------------------------------------
echo "1. real git accepts a repository made by minigit"
mkdir "$WORK/a" && cd "$WORK/a"
"$MG" init >/dev/null
mkdir -p src/deep docs
printf 'hello\n'              > README.md
printf 'int main() {}\n'      > src/main.cpp
printf 'nested\n'             > src/deep/file.txt
printf '#!/bin/sh\necho hi\n' > run.sh && chmod +x run.sh
printf 'a\0b\xff binary'      > docs/blob.bin
: > empty.txt
ln -s README.md link
"$MG" add .
"$MG" commit -m "first commit" >/dev/null

git fsck --strict --no-dangling >/dev/null 2>&1 && ok "git fsck --strict passes" || fail "git fsck"
check_eq "git sees a clean working tree" "$(git status --porcelain)" ""
check_eq "git log shows the commit" "$(git log --format=%s)" "first commit"
check_eq "git reads the author" "$(git log --format='%an <%ae>')" "Test User <test@example.com>"
check_eq "tree hash equals git's own" "$(git rev-parse 'HEAD^{tree}')" "$(git write-tree)"
check_eq "git reads file content" "$(git show HEAD:src/deep/file.txt)" "nested"
check_eq "executable bit recorded" "$(git ls-files -s run.sh | cut -d' ' -f1)" "100755"
check_eq "symlink recorded" "$(git ls-files -s link | cut -d' ' -f1)" "120000"
check_eq "hash-object matches git" "$("$MG" hash-object README.md)" "$(git hash-object README.md)"

# ---------------------------------------------------------------------------
echo "2. minigit reads a repository made by real git"
mkdir "$WORK/b" && cd "$WORK/b"
git init -q -b main
git config gc.auto 0
printf 'one\n' > f.txt && mkdir d && printf 'x\n' > d/g.txt
git add . && git commit -qm "git commit one"
printf 'two\n' >> f.txt
git add . && git commit -qm "git commit two"

check_eq "log matches git" "$("$MG" log --oneline | cut -d' ' -f2-)" "$(git log --format=%s)"
check_eq "cat-file commit matches git" "$("$MG" cat-file -p HEAD)" "$(git cat-file -p HEAD)"
check_eq "cat-file tree matches git" "$("$MG" cat-file -p "$(git rev-parse 'HEAD^{tree}')")" "$(git cat-file -p 'HEAD^{tree}')"
check_eq "status is clean" "$("$MG" status | tail -1)" "nothing to commit, working tree clean"

first="$(git rev-parse HEAD~1)"
"$MG" checkout "$first" >/dev/null
check_eq "checkout of an old commit restores the file" "$(cat f.txt)" "one"
"$MG" checkout main >/dev/null
check_eq "checkout back to main" "$(cat f.txt)" "$(printf 'one\ntwo')"

# ---------------------------------------------------------------------------
echo "3. diff output matches git"
cd "$WORK/a"
for i in $(seq 1 40); do echo "line $i"; done > big.txt
"$MG" add big.txt && "$MG" commit -m "add big" >/dev/null
sed -i -e '3s/.*/changed three/' -e '20d' -e '35a inserted' big.txt
# git adds an "index" line and a function-context hint after "@@"; ignore both.
strip() { grep -v -e '^index ' -e '^diff --git' | sed -E 's/^(@@ [^@]* @@).*/\1/'; }
check_eq "unstaged diff equals git diff" "$("$MG" diff | strip)" "$(git diff --no-color | strip)"
"$MG" add big.txt
check_eq "staged diff equals git diff --staged" "$("$MG" diff --staged | strip)" "$(git diff --staged --no-color | strip)"
"$MG" commit -m "edit big" >/dev/null
a="$(git rev-parse HEAD~1)"; b="$(git rev-parse HEAD)"
check_eq "diff of two commits equals git" "$("$MG" diff "$a" "$b" | strip)" "$(git diff --no-color "$a" "$b" | strip)"

# ---------------------------------------------------------------------------
echo "4. branching and merging"
mkdir "$WORK/c" && cd "$WORK/c"
"$MG" init >/dev/null
printf 'alpha\nbeta\ngamma\ndelta\nepsilon\n' > story.txt
printf 'keep\n' > keep.txt
printf 'will go\n' > gone.txt
"$MG" add . && "$MG" commit -m base >/dev/null

"$MG" checkout -b feature >/dev/null
sed -i '1s/.*/ALPHA/' story.txt
printf 'from feature\n' > new.txt
rm gone.txt
"$MG" add . && "$MG" commit -m "feature work" >/dev/null

"$MG" checkout main >/dev/null
[ -f gone.txt ] && [ ! -f new.txt ] && ok "checkout swaps files between branches" || fail "checkout swap"
sed -i '5s/.*/EPSILON/' story.txt
"$MG" add . && "$MG" commit -m "main work" >/dev/null

"$MG" merge feature >/dev/null
check_eq "both edits to one file are combined" "$(cat story.txt)" "$(printf 'ALPHA\nbeta\ngamma\ndelta\nEPSILON')"
[ -f new.txt ] && [ ! -f gone.txt ] && ok "added and deleted files carried over" || fail "merge add/delete"
check_eq "merge commit has two parents" "$(git log -1 --format=%P | wc -w | tr -d ' ')" "2"
git fsck --strict --no-dangling >/dev/null 2>&1 && ok "git fsck passes after merge" || fail "fsck after merge"
check_eq "git sees a clean tree after merge" "$(git status --porcelain)" ""
check_eq "merge again is a no-op" "$("$MG" merge feature)" "Already up to date."

# Same merge done by real git must give the same tree.
git checkout -q -b verify HEAD~1 && git merge -q --no-edit feature
check_eq "merged tree equals git's merge" "$(git rev-parse 'main^{tree}')" "$(git rev-parse 'verify^{tree}')"
git checkout -q main

echo "5. fast-forward"
"$MG" checkout -b ahead >/dev/null
printf 'more\n' >> keep.txt
"$MG" add . && "$MG" commit -m "ahead" >/dev/null
"$MG" checkout main >/dev/null
check_eq "fast-forward detected" "$("$MG" merge ahead | cut -d' ' -f1)" "Fast-forward"
check_eq "branch moved, no merge commit" "$(git rev-parse main)" "$(git rev-parse ahead)"

echo "6. conflicts"
"$MG" checkout -b left >/dev/null
sed -i '3s/.*/LEFT/' story.txt
"$MG" add . && "$MG" commit -m left >/dev/null
"$MG" checkout main >/dev/null
sed -i '3s/.*/RIGHT/' story.txt
"$MG" add . && "$MG" commit -m right >/dev/null

if "$MG" merge left >/dev/null; then fail "conflicting merge should exit non-zero"; else ok "conflict reported"; fi
grep -q '^<<<<<<< HEAD$' story.txt && grep -q '^RIGHT$' story.txt && grep -q '^LEFT$' story.txt \
    && grep -q '^>>>>>>> left$' story.txt && ok "conflict markers written" || fail "markers"
sed -i -e '/^<<<<<<<\|^=======\|^>>>>>>>/d' -e '/^LEFT$/d' story.txt
"$MG" add story.txt && "$MG" commit -m "resolve" >/dev/null
check_eq "resolved merge has two parents" "$(git log -1 --format=%P | wc -w | tr -d ' ')" "2"
git fsck --strict --no-dangling >/dev/null 2>&1 && ok "git fsck passes after conflict resolution" || fail "fsck"

echo "7. safety checks"
printf 'dirty\n' >> keep.txt
if "$MG" checkout feature >/dev/null 2>&1; then fail "checkout with local changes must be refused"; else ok "checkout refuses to discard local changes"; fi
check_eq "local change still there" "$(tail -1 keep.txt)" "dirty"

echo
echo "interop tests: $pass passed"
