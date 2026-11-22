// minigit: a small Git-compatible version control system.
//
// This file holds the commands. The storage layer is in repo.cpp and the
// diff/merge algorithms are in diff.cpp.

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <functional>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>

#include "diff.h"
#include "repo.h"

using Index = std::map<std::string, IndexEntry>;
using Args = std::vector<std::string>;

// ---------------------------------------------------------------------------
// Working directory helpers
// ---------------------------------------------------------------------------

// Reads a tracked path from disk as Git would store it. Returns false if the
// path does not exist or is not a file or symlink.
static bool read_work_file(const Repo& repo, const std::string& path, std::string& mode, std::string& content) {
    fs::path full = repo.root / path;
    struct stat st;
    if (lstat(full.c_str(), &st) != 0) return false;
    if (S_ISLNK(st.st_mode)) {
        mode = "120000";
        content = fs::read_symlink(full).string();  // a symlink's "content" is its target
        return true;
    }
    if (!S_ISREG(st.st_mode)) return false;
    mode = (st.st_mode & S_IXUSR) ? "100755" : "100644";
    content = read_file(full);
    return true;
}

static IndexEntry make_index_entry(const Repo& repo, const std::string& path, const FileEntry& f) {
    IndexEntry e;
    e.path = path;
    e.oid = f.oid;
    e.mode = mode_from_string(f.mode);
    struct stat st;
    if (lstat((repo.root / path).c_str(), &st) == 0) {
        e.ctime_s = static_cast<uint32_t>(st.st_ctim.tv_sec);
        e.ctime_ns = static_cast<uint32_t>(st.st_ctim.tv_nsec);
        e.mtime_s = static_cast<uint32_t>(st.st_mtim.tv_sec);
        e.mtime_ns = static_cast<uint32_t>(st.st_mtim.tv_nsec);
        e.dev = static_cast<uint32_t>(st.st_dev);
        e.ino = static_cast<uint32_t>(st.st_ino);
        e.uid = st.st_uid;
        e.gid = st.st_gid;
        e.size = static_cast<uint32_t>(st.st_size);
    }
    return e;
}

static void write_work_file(const Repo& repo, const std::string& path, const FileEntry& f,
                            const std::string* content_override = nullptr) {
    fs::path full = repo.root / path;
    fs::create_directories(full.parent_path());
    std::error_code ec;
    fs::remove(full, ec);
    std::string content = content_override ? *content_override : repo.read_object(f.oid).data;
    if (f.mode == "120000") {
        fs::create_symlink(content, full);
        return;
    }
    write_file_atomic(full, content);
    chmod(full.c_str(), f.mode == "100755" ? 0755 : 0644);
}

static void remove_work_file(const Repo& repo, const std::string& path) {
    std::error_code ec;
    fs::path full = repo.root / path;
    fs::remove(full, ec);
    // Remove directories left empty, stopping at the repository root.
    for (fs::path dir = full.parent_path(); dir != repo.root; dir = dir.parent_path()) {
        if (!fs::is_directory(dir) || !fs::is_empty(dir, ec) || ec) break;
        fs::remove(dir, ec);
    }
}

// Turns a path typed by the user (relative to the current directory) into a
// path relative to the repository root, using '/' separators.
static std::string repo_path(const Repo& repo, const std::string& arg) {
    fs::path abs = fs::weakly_canonical(fs::current_path() / arg);
    fs::path rel = abs.lexically_relative(fs::weakly_canonical(repo.root));
    std::string s = rel.generic_string();
    if (s == ".") return "";
    if (s.rfind("..", 0) == 0) throw Error("path '" + arg + "' is outside the repository");
    return s;
}

// Every file under dir (repo-relative, "" for the root), skipping .git.
static void list_files(const Repo& repo, const std::string& dir, std::vector<std::string>& out) {
    for (const auto& e : fs::directory_iterator(repo.root / dir)) {
        std::string name = e.path().filename().string();
        std::string rel = dir.empty() ? name : dir + "/" + name;
        if (rel == ".git") continue;
        if (e.is_directory() && !e.is_symlink()) list_files(repo, rel, out);
        else out.push_back(rel);
    }
}

// The working directory's view of every tracked file. A tracked file that is
// missing on disk is simply absent from the result.
static FileMap worktree_files(const Repo& repo, const Index& index) {
    FileMap out;
    for (const auto& [path, e] : index) {
        std::string mode, content;
        if (read_work_file(repo, path, mode, content)) out[path] = FileEntry{mode, Repo::hash_object("blob", content)};
    }
    return out;
}

// ---------------------------------------------------------------------------
// Comparing two snapshots
// ---------------------------------------------------------------------------

struct Change {
    char kind;  // 'A' added, 'D' deleted, 'M' modified
    std::string path;
};

static std::vector<Change> compare(const FileMap& from, const FileMap& to) {
    std::vector<Change> out;
    for (const auto& [path, f] : from) {
        auto it = to.find(path);
        if (it == to.end()) out.push_back({'D', path});
        else if (it->second != f) out.push_back({'M', path});
    }
    for (const auto& [path, f] : to)
        if (!from.count(path)) out.push_back({'A', path});
    std::sort(out.begin(), out.end(), [](const Change& a, const Change& b) { return a.path < b.path; });
    return out;
}

static void require_clean(const Repo& repo, const Index& index, const std::string& action) {
    FileMap staged = index_files(index);
    if (!compare(repo.commit_files(repo.head_commit()), staged).empty() ||
        !compare(staged, worktree_files(repo, index)).empty())
        throw Error("you have uncommitted changes; commit them before you " + action);
}

// Makes the working directory and index match `target`, given that they
// currently match `current`. Refuses to overwrite untracked files.
static void switch_files(Repo& repo, const FileMap& current, const FileMap& target) {
    for (const auto& [path, f] : target)
        if (!current.count(path) && fs::exists(fs::symlink_status(repo.root / path)))
            throw Error("untracked file '" + path + "' would be overwritten");

    for (const auto& [path, f] : current)
        if (!target.count(path)) remove_work_file(repo, path);
    for (const auto& [path, f] : target) {
        auto it = current.find(path);
        if (it == current.end() || it->second != f) write_work_file(repo, path, f);
    }
    Index index;
    for (const auto& [path, f] : target) index[path] = make_index_entry(repo, path, f);
    repo.write_index(index);
}

// ---------------------------------------------------------------------------
// History helpers
// ---------------------------------------------------------------------------

static std::string identity() {
    const char* name = getenv("GIT_AUTHOR_NAME");
    const char* email = getenv("GIT_AUTHOR_EMAIL");
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    long off = local.tm_gmtoff / 60;  // minutes east of UTC
    char tz[32];
    std::snprintf(tz, sizeof(tz), "%c%02ld%02ld", off < 0 ? '-' : '+', std::labs(off) / 60, std::labs(off) % 60);
    return std::string(name ? name : "minigit") + " <" + (email ? email : "minigit@localhost") + "> " +
           std::to_string(static_cast<long long>(now)) + " " + tz;
}

static std::string create_commit(Repo& repo, const std::string& tree, const std::vector<std::string>& parents,
                                 const std::string& message) {
    std::string who = identity();
    std::string body = "tree " + tree + "\n";
    for (const auto& p : parents) body += "parent " + p + "\n";
    body += "author " + who + "\ncommitter " + who + "\n\n" + message;
    if (body.back() != '\n') body += "\n";
    return repo.write_object("commit", body);
}

// Visits commits reachable from `start`, newest first. History is a graph
// (merges have two parents), so this is a best-first search keyed on commit
// time with a visited set, not a simple walk down a list.
static void walk_history(const Repo& repo, const std::string& start,
                         const std::function<bool(const std::string&, const Commit&)>& visit) {
    std::priority_queue<std::pair<long long, std::string>> queue;
    std::set<std::string> seen{start};
    queue.push({repo.read_commit(start).time, start});
    while (!queue.empty()) {
        std::string oid = queue.top().second;
        queue.pop();
        Commit c = repo.read_commit(oid);
        if (!visit(oid, c)) return;
        for (const auto& p : c.parents)
            if (seen.insert(p).second) queue.push({repo.read_commit(p).time, p});
    }
}

// The most recent commit that is an ancestor of both a and b: collect all
// ancestors of a, then walk b's history newest-first until we hit one.
static std::optional<std::string> merge_base(const Repo& repo, const std::string& a, const std::string& b) {
    std::set<std::string> ancestors;
    walk_history(repo, a, [&](const std::string& oid, const Commit&) {
        ancestors.insert(oid);
        return true;
    });
    std::optional<std::string> base;
    walk_history(repo, b, [&](const std::string& oid, const Commit&) {
        if (ancestors.count(oid)) {
            base = oid;
            return false;
        }
        return true;
    });
    return base;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static int cmd_init(const Args&) {
    Repo repo = Repo::init(fs::current_path());
    std::cout << "Initialized empty repository in " << repo.git.string() << "\n";
    return 0;
}

static int cmd_hash_object(const Args& args) {
    bool write = false;
    std::string file;
    for (const auto& a : args) {
        if (a == "-w") write = true;
        else file = a;
    }
    if (file.empty()) throw Error("usage: minigit hash-object [-w] <file>");
    std::string data = read_file(file);
    std::cout << (write ? Repo::find().write_object("blob", data) : Repo::hash_object("blob", data)) << "\n";
    return 0;
}

static int cmd_cat_file(const Args& args) {
    if (args.size() != 2) throw Error("usage: minigit cat-file (-p|-t|-s) <object>");
    Repo repo = Repo::find();
    auto oid = repo.resolve(args[1]);
    if (!oid) throw Error("not a valid object name: " + args[1]);
    Object obj = repo.read_object(*oid);
    if (args[0] == "-t") {
        std::cout << obj.type << "\n";
    } else if (args[0] == "-s") {
        std::cout << obj.data.size() << "\n";
    } else if (args[0] == "-p") {
        if (obj.type == "tree") {
            for (const auto& e : Repo::parse_tree(obj.data))
                std::cout << std::string(6 - e.mode.size(), '0') << e.mode << (e.is_dir() ? " tree " : " blob ")
                          << e.oid << "\t" << e.name << "\n";
        } else {
            std::cout << obj.data;
        }
    } else {
        throw Error("unknown option " + args[0]);
    }
    return 0;
}

static int cmd_add(const Args& args) {
    if (args.empty()) throw Error("nothing specified, nothing added");
    Repo repo = Repo::find();
    Index index = repo.read_index();

    auto stage = [&](const std::string& path) {
        std::string mode, content;
        if (!read_work_file(repo, path, mode, content)) return;
        FileEntry f{mode, repo.write_object("blob", content)};
        index[path] = make_index_entry(repo, path, f);
    };

    for (const auto& arg : args) {
        std::string path = repo_path(repo, arg);
        fs::path full = repo.root / path;
        if (fs::is_directory(full) && !fs::is_symlink(full)) {
            std::vector<std::string> files;
            list_files(repo, path, files);
            for (const auto& f : files) stage(f);
            // Files deleted from disk under this directory are unstaged too.
            std::string prefix = path.empty() ? "" : path + "/";
            for (auto it = index.begin(); it != index.end();) {
                bool under = it->first.compare(0, prefix.size(), prefix) == 0;
                if (under && !fs::exists(fs::symlink_status(repo.root / it->first))) it = index.erase(it);
                else ++it;
            }
        } else if (fs::exists(fs::symlink_status(full))) {
            stage(path);
        } else if (index.erase(path) == 0) {
            throw Error("pathspec '" + arg + "' did not match any files");
        }
    }
    repo.write_index(index);
    return 0;
}

static int cmd_status(const Args&) {
    Repo repo = Repo::find();
    Index index = repo.read_index();
    FileMap staged = index_files(index);

    std::string ref = repo.head_ref();
    if (ref.empty()) std::cout << "HEAD detached at " << repo.head_commit().value_or("?").substr(0, 7) << "\n";
    else std::cout << "On branch " << ref.substr(std::string("refs/heads/").size()) << "\n";
    if (fs::exists(repo.git / "MERGE_HEAD")) std::cout << "A merge is in progress: fix conflicts, add, then commit.\n";

    auto label = [](char k) { return k == 'A' ? "new file:   " : k == 'D' ? "deleted:    " : "modified:   "; };

    auto to_commit = compare(repo.commit_files(repo.head_commit()), staged);
    if (!to_commit.empty()) {
        std::cout << "\nChanges to be committed:\n";
        for (const auto& c : to_commit) std::cout << "\t" << label(c.kind) << c.path << "\n";
    }
    auto unstaged = compare(staged, worktree_files(repo, index));
    if (!unstaged.empty()) {
        std::cout << "\nChanges not staged for commit:\n";
        for (const auto& c : unstaged) std::cout << "\t" << label(c.kind) << c.path << "\n";
    }
    std::vector<std::string> all, untracked;
    list_files(repo, "", all);
    for (const auto& p : all)
        if (!index.count(p)) untracked.push_back(p);
    std::sort(untracked.begin(), untracked.end());
    if (!untracked.empty()) {
        std::cout << "\nUntracked files:\n";
        for (const auto& p : untracked) std::cout << "\t" << p << "\n";
    }
    if (to_commit.empty() && unstaged.empty() && untracked.empty())
        std::cout << "nothing to commit, working tree clean\n";
    return 0;
}

static int cmd_commit(const Args& args) {
    Repo repo = Repo::find();
    std::string message;
    for (size_t i = 0; i + 1 < args.size(); i++)
        if (args[i] == "-m") message = args[i + 1];

    std::vector<std::string> parents;
    auto head = repo.head_commit();
    if (head) parents.push_back(*head);

    // Finishing a conflicted merge: the commit gets the other branch as a second parent.
    std::string merge_head;
    bool merging = try_read_file(repo.git / "MERGE_HEAD", merge_head);
    if (merging) {
        while (!merge_head.empty() && merge_head.back() == '\n') merge_head.pop_back();
        parents.push_back(merge_head);
        if (message.empty()) try_read_file(repo.git / "MERGE_MSG", message);
    }
    if (message.empty()) throw Error("a commit message is required: minigit commit -m <message>");

    Index index = repo.read_index();
    std::string tree = repo.write_tree(index_files(index));
    if (head && !merging && repo.read_commit(*head).tree == tree) {
        std::cout << "nothing to commit\n";
        return 1;
    }

    std::string oid = create_commit(repo, tree, parents, message);
    repo.update_head(oid);
    fs::remove(repo.git / "MERGE_HEAD");
    fs::remove(repo.git / "MERGE_MSG");

    std::string first_line = message.substr(0, message.find('\n'));
    std::cout << "[" << oid.substr(0, 7) << "] " << first_line << "\n";
    return 0;
}

static int cmd_log(const Args& args) {
    Repo repo = Repo::find();
    bool oneline = false;
    std::string rev = "HEAD";
    for (const auto& a : args) {
        if (a == "--oneline") oneline = true;
        else rev = a;
    }
    auto start = repo.resolve(rev);
    if (!start) throw Error(rev == "HEAD" ? "the current branch has no commits yet" : "unknown revision " + rev);

    walk_history(repo, *start, [&](const std::string& oid, const Commit& c) {
        if (oneline) {
            std::cout << oid.substr(0, 7) << " " << c.message.substr(0, c.message.find('\n')) << "\n";
            return true;
        }
        std::cout << "commit " << oid << "\n";
        if (c.parents.size() > 1) {
            std::cout << "Merge:";
            for (const auto& p : c.parents) std::cout << " " << p.substr(0, 7);
            std::cout << "\n";
        }
        std::cout << "Author: " << c.author.substr(0, c.author.find('>') + 1) << "\n";
        std::time_t t = static_cast<std::time_t>(c.time);
        char buf[64];
        std::tm local{};
        localtime_r(&t, &local);
        std::strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Y %z", &local);
        std::cout << "Date:   " << buf << "\n\n";
        std::istringstream lines(c.message);
        for (std::string line; std::getline(lines, line);) std::cout << "    " << line << "\n";
        std::cout << "\n";
        return true;
    });
    return 0;
}

static int cmd_branch(const Args& args) {
    Repo repo = Repo::find();
    if (args.empty()) {
        std::string current = repo.head_ref();
        for (const auto& b : repo.branches())
            std::cout << ("refs/heads/" + b == current ? "* " : "  ") << b << "\n";
        return 0;
    }
    auto head = repo.head_commit();
    if (!head) throw Error("cannot create a branch before the first commit");
    if (repo.read_ref("refs/heads/" + args[0])) throw Error("a branch named '" + args[0] + "' already exists");
    repo.write_ref("refs/heads/" + args[0], *head);
    return 0;
}

static int cmd_checkout(const Args& args) {
    Repo repo = Repo::find();
    bool create = !args.empty() && args[0] == "-b";
    if (args.size() != (create ? 2u : 1u)) throw Error("usage: minigit checkout [-b] <branch-or-commit>");
    std::string name = args.back();

    if (create) {
        cmd_branch({name});
        write_file_atomic(repo.git / "HEAD", "ref: refs/heads/" + name + "\n");
        std::cout << "Switched to a new branch '" << name << "'\n";
        return 0;
    }

    auto target = repo.resolve(name);
    if (!target) throw Error("'" + name + "' did not match any branch or commit");
    Index index = repo.read_index();
    require_clean(repo, index, "switch branches");

    switch_files(repo, index_files(index), repo.commit_files(target));

    if (repo.read_ref("refs/heads/" + name)) {
        write_file_atomic(repo.git / "HEAD", "ref: refs/heads/" + name + "\n");
        std::cout << "Switched to branch '" << name << "'\n";
    } else {
        write_file_atomic(repo.git / "HEAD", *target + "\n");  // detached HEAD
        std::cout << "HEAD is now at " << target->substr(0, 7) << "\n";
    }
    return 0;
}

// Prints a unified diff between two snapshots. `load` fetches file content
// for either side, so the same code diffs commits, the index and the disk.
static void diff_snapshots(const FileMap& from, const FileMap& to,
                           const std::function<std::string(const std::string&, const FileEntry&)>& load_from,
                           const std::function<std::string(const std::string&, const FileEntry&)>& load_to) {
    for (const auto& c : compare(from, to)) {
        std::string old_text, new_text;
        if (c.kind != 'A') old_text = load_from(c.path, from.at(c.path));
        if (c.kind != 'D') new_text = load_to(c.path, to.at(c.path));

        std::cout << "diff --git a/" << c.path << " b/" << c.path << "\n";
        if (c.kind == 'A') std::cout << "new file mode " << to.at(c.path).mode << "\n";
        if (c.kind == 'D') std::cout << "deleted file mode " << from.at(c.path).mode << "\n";
        if (c.kind == 'M' && from.at(c.path).mode != to.at(c.path).mode)
            std::cout << "old mode " << from.at(c.path).mode << "\nnew mode " << to.at(c.path).mode << "\n";

        if (old_text.find('\0') != std::string::npos || new_text.find('\0') != std::string::npos) {
            std::cout << "Binary files differ\n";
            continue;
        }
        if (old_text == new_text) continue;  // mode-only change
        std::cout << "--- " << (c.kind == 'A' ? "/dev/null" : "a/" + c.path) << "\n";
        std::cout << "+++ " << (c.kind == 'D' ? "/dev/null" : "b/" + c.path) << "\n";
        auto a = split_lines(old_text), b = split_lines(new_text);
        print_hunks(a, b, myers_diff(a, b), std::cout);
    }
}

static int cmd_diff(const Args& args) {
    Repo repo = Repo::find();
    auto from_objects = [&](const std::string&, const FileEntry& f) { return repo.read_object(f.oid).data; };
    auto from_disk = [&](const std::string& path, const FileEntry&) {
        std::string mode, content;
        read_work_file(repo, path, mode, content);
        return content;
    };

    Index index = repo.read_index();
    if (args.size() == 2) {  // two commits
        auto a = repo.resolve(args[0]), b = repo.resolve(args[1]);
        if (!a || !b) throw Error("unknown revision");
        diff_snapshots(repo.commit_files(a), repo.commit_files(b), from_objects, from_objects);
    } else if (args.size() == 1 && (args[0] == "--staged" || args[0] == "--cached")) {
        diff_snapshots(repo.commit_files(repo.head_commit()), index_files(index), from_objects, from_objects);
    } else if (args.empty()) {  // what `add` would stage
        diff_snapshots(index_files(index), worktree_files(repo, index), from_objects, from_disk);
    } else {
        throw Error("usage: minigit diff [--staged | <commit> <commit>]");
    }
    return 0;
}

static int cmd_merge(const Args& args) {
    if (args.size() != 1) throw Error("usage: minigit merge <branch>");
    Repo repo = Repo::find();
    auto ours = repo.head_commit();
    auto theirs = repo.resolve(args[0]);
    if (!ours) throw Error("cannot merge before the first commit");
    if (!theirs) throw Error("'" + args[0] + "' does not point to a commit");

    Index index = repo.read_index();
    require_clean(repo, index, "merge");

    auto base = merge_base(repo, *ours, *theirs);
    if (base && *base == *theirs) {
        std::cout << "Already up to date.\n";
        return 0;
    }
    FileMap o = repo.commit_files(ours), t = repo.commit_files(theirs);

    // Our branch has no commits of its own since the split: just move it forward.
    if (base && *base == *ours) {
        switch_files(repo, o, t);
        repo.update_head(*theirs);
        std::cout << "Fast-forward to " << theirs->substr(0, 7) << "\n";
        return 0;
    }

    // True merge: decide every path from (base, ours, theirs).
    FileMap b = repo.commit_files(base);
    std::set<std::string> paths;
    for (const auto& m : {b, o, t})
        for (const auto& [p, f] : m) paths.insert(p);

    FileMap result;                               // what the merged tree will contain
    std::map<std::string, std::string> conflicts; // path -> file content with conflict markers
    auto get = [](const FileMap& m, const std::string& p) -> const FileEntry* {
        auto it = m.find(p);
        return it == m.end() ? nullptr : &it->second;
    };
    auto same = [](const FileEntry* x, const FileEntry* y) { return (!x && !y) || (x && y && *x == *y); };
    auto text = [&](const FileEntry* f) { return f ? repo.read_object(f->oid).data : std::string(); };

    for (const auto& p : paths) {
        const FileEntry *fb = get(b, p), *fo = get(o, p), *ft = get(t, p);
        const FileEntry* pick = nullptr;
        if (same(fo, ft) || same(ft, fb)) pick = fo;       // same on both sides, or only we changed it
        else if (same(fo, fb)) pick = ft;                  // only they changed it
        else if (fo && ft) {                               // both changed it: merge line by line
            MergeResult m = merge3(text(fb), text(fo), text(ft), "HEAD", args[0]);
            if (m.conflict) {
                conflicts[p] = m.text;
                result[p] = *fo;  // keep our version staged until the user resolves it
            } else {
                result[p] = FileEntry{fo->mode, repo.write_object("blob", m.text)};
            }
            continue;
        } else {                                           // one side deleted, the other modified
            const FileEntry* kept = fo ? fo : ft;
            conflicts[p] = text(kept);
            result[p] = *kept;
            std::cout << "CONFLICT (modify/delete): " << p << " kept; delete it or add it to resolve\n";
            continue;
        }
        if (pick) result[p] = *pick;
    }

    switch_files(repo, o, result);
    for (const auto& [p, content] : conflicts) {
        write_work_file(repo, p, result[p], &content);
        std::cout << "CONFLICT: " << p << "\n";
    }

    std::string message = "Merge branch '" + args[0] + "'\n";
    if (!conflicts.empty()) {
        write_file_atomic(repo.git / "MERGE_HEAD", *theirs + "\n");
        write_file_atomic(repo.git / "MERGE_MSG", message);
        std::cout << "Automatic merge failed; fix conflicts, then add and commit.\n";
        return 1;
    }
    std::string oid = create_commit(repo, repo.write_tree(result), {*ours, *theirs}, message);
    repo.update_head(oid);
    std::cout << "Merge made: " << oid.substr(0, 7) << "\n";
    return 0;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    static const std::map<std::string, int (*)(const Args&)> commands = {
        {"init", cmd_init},         {"hash-object", cmd_hash_object}, {"cat-file", cmd_cat_file},
        {"add", cmd_add},           {"status", cmd_status},           {"commit", cmd_commit},
        {"log", cmd_log},           {"branch", cmd_branch},           {"checkout", cmd_checkout},
        {"diff", cmd_diff},         {"merge", cmd_merge},
    };
    if (argc < 2 || !commands.count(argv[1])) {
        std::cerr << "usage: minigit <command> [args]\n\ncommands:";
        for (const auto& [name, fn] : commands) std::cerr << " " << name;
        std::cerr << "\n";
        return 2;
    }
    try {
        return commands.at(argv[1])(Args(argv + 2, argv + argc));
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 128;
    }
}
