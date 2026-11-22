# minigit

A Git-compatible version control system in C++17, written from scratch. The only library used is zlib; SHA-1, the index format, diff and merge are all implemented here.

"Compatible" is meant literally: the official `git` can read, verify and continue working in a repository created by minigit, and minigit can read one created by `git`.

```
$ minigit init
$ echo hello > a.txt && minigit add . && minigit commit -m "first"
$ git log --oneline          # official git reads it
3f2a9c1 first
$ git fsck --strict          # and finds nothing wrong
```

## Build and test

```sh
make          # builds bin/minigit (needs g++ and zlib)
make test     # unit tests, then 34 interoperability checks against real git
```

## Commands

| Command | What it does |
|---|---|
| `init` | Create an empty repository |
| `add <path>...` | Stage files or whole directories, including deletions |
| `status` | Staged, unstaged and untracked changes |
| `commit -m <msg>` | Record the staged snapshot |
| `log [--oneline] [rev]` | History, newest first |
| `branch [name]` | List or create branches |
| `checkout [-b] <name>` | Switch branch or commit |
| `diff [--staged \| A B]` | Unified diff of working tree, index or two commits |
| `merge <branch>` | Fast-forward or three-way merge with conflict markers |
| `hash-object`, `cat-file` | Inspect the object store |

## How it works

| File | Contents |
|---|---|
| `src/util.cpp` | SHA-1, zlib wrappers, atomic file writes |
| `src/repo.cpp` | Object store, trees, commits, refs, index file format |
| `src/diff.cpp` | Myers diff, unified output, three-way merge |
| `src/main.cpp` | The commands |

**Objects.** Everything is stored as an object named by the SHA-1 of its content. A blob is a file. A tree is a directory listing. A commit points to one tree and its parent commits. A branch is a file containing one commit id. Identical content is stored once, and any corruption changes the name.

**Index.** The staging area is a binary file listing every tracked path with its blob id and file stats. minigit writes Git's version 2 format byte for byte, including the trailing checksum, which is why `git status` works on a minigit repository.

**Diff.** Myers' algorithm finds the shortest edit script between two files in O((N+M)·D) time, where D is the number of differing lines. The comment at the top of `diff.cpp` explains the grid-and-diagonals idea.

**Merge.** Find the most recent common ancestor of the two branches, then decide each file from the three versions (base, ours, theirs). If both sides edited the same file, merge it line by line with diff3: regions changed on one side are taken, regions changed differently on both become conflict blocks.

**Atomic writes.** Every file is written to a temporary name and renamed into place, so a crash never leaves a half-written object, ref or index.

## How it is tested

- `tests/unit.cpp` checks SHA-1 against the published vectors, and checks the Myers implementation on 2,000 random inputs against a textbook LCS table: the edit script must rebuild the target and must be the shortest possible.
- `tests/interop.sh` uses real `git` as the judge: `git fsck --strict` on minigit's repositories, tree hashes equal to `git write-tree`, diff output equal to `git diff`, and a merged tree equal to the one `git merge` produces for the same branches.

## Limits

- Reads loose objects only. A repository that has been packed (`git gc`, or anything cloned from a server) is not readable yet.
- No remotes: no `clone`, `push` or `pull`.
- No `.gitignore`, so `add .` stages everything.
- Merges use a single common ancestor. Criss-cross histories that need a recursive merge may conflict where Git would not.
- Conflicts are written as markers in the file; the index does not record the three conflict stages the way Git does.

## Questions to check your understanding

1. Why is a directory compared as `name + "/"` when sorting tree entries, and what breaks if you sort by plain name?
2. Two commits have identical trees but different ids. What else goes into a commit's hash?
3. In `myers_diff`, what does `v[k]` hold, and why can the search stop the first time it reaches the bottom-right corner?
4. Why does `walk_history` need a visited set and a priority queue, when history looks like a list?
5. `merge3` takes "theirs" for a chunk when ours equals base. Why is that always safe?
6. What would go wrong if objects were written directly to their final path without the rename?
7. Why does `checkout` refuse to run when tracked files have uncommitted changes?

## Next steps

- Packfile reading (delta-compressed objects), which unlocks cloned repositories.
- `.gitignore` support.
- Conflict stages in the index.
- Recursive merge for multiple common ancestors.
