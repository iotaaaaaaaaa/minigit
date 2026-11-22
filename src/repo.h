// The repository on disk: objects, refs and the index.
//
// Git's storage model in one paragraph: everything is an *object* named by
// the SHA-1 of its contents. A blob is a file's bytes. A tree is a directory
// listing (name -> blob or tree). A commit points at one tree plus its parent
// commits. A branch is just a file holding one commit id. Because names are
// hashes of content, identical files are stored once and any corruption
// changes the name.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "util.h"

struct Object {
    std::string type;  // "blob", "tree" or "commit"
    std::string data;
};

// One line of a tree object.
struct TreeEntry {
    std::string mode;  // "100644" file, "100755" executable, "120000" symlink, "40000" directory
    std::string name;
    std::string oid;  // 40 hex chars
    bool is_dir() const { return mode == "40000"; }
};

struct Commit {
    std::string tree;
    std::vector<std::string> parents;
    std::string author;
    std::string committer;
    std::string message;
    long long time = 0;  // committer timestamp, seconds since epoch
};

// A file as recorded in a tree or the index: its mode and blob id.
struct FileEntry {
    std::string mode;
    std::string oid;
    bool operator==(const FileEntry& o) const { return mode == o.mode && oid == o.oid; }
    bool operator!=(const FileEntry& o) const { return !(*this == o); }
};

// Repo-relative path ("src/main.cpp") -> file. std::map keeps paths sorted by
// byte value, which is the order Git requires in the index.
using FileMap = std::map<std::string, FileEntry>;

// One index record. The stat fields let real Git skip re-hashing a file whose
// size and timestamps have not changed.
struct IndexEntry {
    uint32_t ctime_s = 0, ctime_ns = 0, mtime_s = 0, mtime_ns = 0;
    uint32_t dev = 0, ino = 0, mode = 0, uid = 0, gid = 0, size = 0;
    std::string oid;
    std::string path;
};

std::string mode_to_string(uint32_t mode);
uint32_t mode_from_string(const std::string& mode);

class Repo {
   public:
    fs::path root;  // working directory
    fs::path git;   // root/.git

    // Walks up from the current directory until it finds a .git folder.
    static Repo find();
    static Repo init(const fs::path& dir);

    // --- objects ---------------------------------------------------------
    static std::string hash_object(const std::string& type, const std::string& data);
    std::string write_object(const std::string& type, const std::string& data);
    Object read_object(const std::string& oid) const;
    bool has_object(const std::string& oid) const;

    static std::vector<TreeEntry> parse_tree(const std::string& data);
    static std::string serialize_tree(std::vector<TreeEntry> entries);
    Commit read_commit(const std::string& oid) const;

    // Expands a tree into a flat path -> file map, recursing into subtrees.
    FileMap flatten_tree(const std::string& tree_oid) const;
    // The reverse: writes nested tree objects for a flat map, returns the root tree id.
    std::string write_tree(const FileMap& files);
    // Files of a commit, or an empty map when there is no commit yet.
    FileMap commit_files(const std::optional<std::string>& commit_oid) const;

    // --- refs ------------------------------------------------------------
    // "refs/heads/main" when on a branch, empty when HEAD is detached.
    std::string head_ref() const;
    std::optional<std::string> head_commit() const;
    std::optional<std::string> read_ref(const std::string& ref) const;
    void write_ref(const std::string& ref, const std::string& oid);
    // Moves the current branch (or detached HEAD) to oid.
    void update_head(const std::string& oid);
    // Accepts HEAD, a branch name, a full id or a unique id prefix.
    std::optional<std::string> resolve(const std::string& name) const;
    std::vector<std::string> branches() const;

    // --- index -----------------------------------------------------------
    std::map<std::string, IndexEntry> read_index() const;
    void write_index(const std::map<std::string, IndexEntry>& entries);
};

FileMap index_files(const std::map<std::string, IndexEntry>& index);
