#include "repo.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

// ---------------------------------------------------------------------------
// Locating and creating a repository
// ---------------------------------------------------------------------------

Repo Repo::find() {
    fs::path dir = fs::current_path();
    while (true) {
        if (fs::is_directory(dir / ".git")) {
            Repo r;
            r.root = dir;
            r.git = dir / ".git";
            return r;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            throw Error("not a repository (or any parent directory): .git");
        dir = dir.parent_path();
    }
}

Repo Repo::init(const fs::path& dir) {
    Repo r;
    r.root = dir;
    r.git = dir / ".git";
    fs::create_directories(r.git / "objects");
    fs::create_directories(r.git / "refs" / "heads");
    if (!fs::exists(r.git / "HEAD")) write_file_atomic(r.git / "HEAD", "ref: refs/heads/main\n");
    if (!fs::exists(r.git / "config"))
        write_file_atomic(r.git / "config",
                          "[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n\tbare = false\n");
    return r;
}

// ---------------------------------------------------------------------------
// Objects
//
// On disk an object is zlib(  "<type> <size>\0<data>"  ), stored at
// .git/objects/<first 2 hex chars>/<remaining 38>. Its id is the SHA-1 of the
// uncompressed bytes, header included.
// ---------------------------------------------------------------------------

static std::string with_header(const std::string& type, const std::string& data) {
    std::string store = type + " " + std::to_string(data.size());
    store.push_back('\0');
    store += data;
    return store;
}

std::string Repo::hash_object(const std::string& type, const std::string& data) {
    return to_hex(sha1(with_header(type, data)));
}

std::string Repo::write_object(const std::string& type, const std::string& data) {
    std::string store = with_header(type, data);
    std::string oid = to_hex(sha1(store));
    fs::path p = git / "objects" / oid.substr(0, 2) / oid.substr(2);
    // Same content always has the same name, so an existing file is already correct.
    if (!fs::exists(p)) write_file_atomic(p, zlib_compress(store));
    return oid;
}

bool Repo::has_object(const std::string& oid) const {
    return oid.size() == 40 && fs::exists(git / "objects" / oid.substr(0, 2) / oid.substr(2));
}

Object Repo::read_object(const std::string& oid) const {
    if (oid.size() != 40) throw Error("bad object id " + oid);
    std::string raw;
    if (!try_read_file(git / "objects" / oid.substr(0, 2) / oid.substr(2), raw))
        throw Error("object " + oid + " not found (packed objects are not supported)");
    std::string store = zlib_decompress(raw);

    size_t sp = store.find(' ');
    size_t nul = store.find('\0');
    if (sp == std::string::npos || nul == std::string::npos || sp > nul)
        throw Error("corrupt object " + oid);
    Object obj;
    obj.type = store.substr(0, sp);
    obj.data = store.substr(nul + 1);
    if (std::to_string(obj.data.size()) != store.substr(sp + 1, nul - sp - 1))
        throw Error("object " + oid + " has wrong length");
    return obj;
}

// A tree is a sequence of:  "<mode> <name>\0<20 raw bytes of id>"
std::vector<TreeEntry> Repo::parse_tree(const std::string& data) {
    std::vector<TreeEntry> out;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t sp = data.find(' ', pos);
        size_t nul = (sp == std::string::npos) ? sp : data.find('\0', sp);
        if (nul == std::string::npos || nul + 21 > data.size()) throw Error("corrupt tree object");
        TreeEntry e;
        e.mode = data.substr(pos, sp - pos);
        e.name = data.substr(sp + 1, nul - sp - 1);
        e.oid = to_hex(data.substr(nul + 1, 20));
        out.push_back(std::move(e));
        pos = nul + 21;
    }
    return out;
}

std::string Repo::serialize_tree(std::vector<TreeEntry> entries) {
    // Git sorts tree entries by name, comparing a directory as if its name
    // ended in '/'. Getting this wrong produces a different hash than Git.
    auto key = [](const TreeEntry& e) { return e.is_dir() ? e.name + "/" : e.name; };
    std::sort(entries.begin(), entries.end(),
              [&](const TreeEntry& a, const TreeEntry& b) { return key(a) < key(b); });
    std::string out;
    for (const auto& e : entries) {
        out += e.mode + " " + e.name;
        out.push_back('\0');
        out += from_hex(e.oid);
    }
    return out;
}

Commit Repo::read_commit(const std::string& oid) const {
    Object obj = read_object(oid);
    if (obj.type != "commit") throw Error(oid + " is a " + obj.type + ", not a commit");

    Commit c;
    size_t pos = 0;
    while (pos < obj.data.size()) {
        size_t nl = obj.data.find('\n', pos);
        if (nl == std::string::npos) nl = obj.data.size();
        std::string line = obj.data.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) break;  // blank line separates headers from the message
        if (line.rfind("tree ", 0) == 0) c.tree = line.substr(5);
        else if (line.rfind("parent ", 0) == 0) c.parents.push_back(line.substr(7));
        else if (line.rfind("author ", 0) == 0) c.author = line.substr(7);
        else if (line.rfind("committer ", 0) == 0) c.committer = line.substr(10);
    }
    if (pos < obj.data.size()) c.message = obj.data.substr(pos);

    // "Name <email> 1700000000 +0530": the timestamp is the second-last word.
    std::istringstream words(c.committer);
    std::string w, prev, prev2;
    while (words >> w) {
        prev2 = prev;
        prev = w;
    }
    try {
        c.time = std::stoll(prev2);
    } catch (...) {
        c.time = 0;
    }
    return c;
}

static void flatten(const Repo& repo, const std::string& tree_oid, const std::string& prefix, FileMap& out) {
    Object obj = repo.read_object(tree_oid);
    if (obj.type != "tree") throw Error(tree_oid + " is not a tree");
    for (const auto& e : Repo::parse_tree(obj.data)) {
        if (e.is_dir()) flatten(repo, e.oid, prefix + e.name + "/", out);
        else out[prefix + e.name] = FileEntry{e.mode, e.oid};
    }
}

FileMap Repo::flatten_tree(const std::string& tree_oid) const {
    FileMap out;
    flatten(*this, tree_oid, "", out);
    return out;
}

FileMap Repo::commit_files(const std::optional<std::string>& commit_oid) const {
    if (!commit_oid) return {};
    return flatten_tree(read_commit(*commit_oid).tree);
}

namespace {
// In-memory directory tree used to turn flat paths back into nested trees.
struct DirNode {
    std::map<std::string, DirNode> dirs;
    std::map<std::string, FileEntry> files;
};

std::string write_dir(Repo& repo, const DirNode& node) {
    std::vector<TreeEntry> entries;
    for (const auto& [name, f] : node.files) entries.push_back({f.mode, name, f.oid});
    // Children are written first: a tree's id depends on its children's ids.
    for (const auto& [name, child] : node.dirs) entries.push_back({"40000", name, write_dir(repo, child)});
    return repo.write_object("tree", Repo::serialize_tree(std::move(entries)));
}
}  // namespace

std::string Repo::write_tree(const FileMap& files) {
    DirNode top;
    for (const auto& [path, entry] : files) {
        DirNode* node = &top;
        size_t start = 0, slash;
        while ((slash = path.find('/', start)) != std::string::npos) {
            node = &node->dirs[path.substr(start, slash - start)];
            start = slash + 1;
        }
        node->files[path.substr(start)] = entry;
    }
    return write_dir(*this, top);
}

// ---------------------------------------------------------------------------
// Refs
// ---------------------------------------------------------------------------

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string Repo::head_ref() const {
    std::string head = trim(read_file(git / "HEAD"));
    if (head.rfind("ref: ", 0) == 0) return head.substr(5);
    return "";
}

std::optional<std::string> Repo::read_ref(const std::string& ref) const {
    std::string s;
    if (!try_read_file(git / ref, s)) return std::nullopt;
    s = trim(s);
    if (s.size() != 40 || !is_hex(s)) return std::nullopt;
    return s;
}

void Repo::write_ref(const std::string& ref, const std::string& oid) {
    write_file_atomic(git / ref, oid + "\n");
}

std::optional<std::string> Repo::head_commit() const {
    std::string ref = head_ref();
    if (!ref.empty()) return read_ref(ref);  // nullopt on a branch with no commits yet
    return read_ref("HEAD");
}

void Repo::update_head(const std::string& oid) {
    std::string ref = head_ref();
    write_ref(ref.empty() ? "HEAD" : ref, oid);
}

std::optional<std::string> Repo::resolve(const std::string& name) const {
    if (name == "HEAD") return head_commit();
    if (auto oid = read_ref("refs/heads/" + name)) return oid;
    if (name.size() == 40 && is_hex(name) && has_object(name)) return name;

    // Abbreviated id: look for exactly one object whose name starts with it.
    if (name.size() >= 4 && name.size() < 40 && is_hex(name)) {
        fs::path dir = git / "objects" / name.substr(0, 2);
        std::string rest = name.substr(2), found;
        if (fs::is_directory(dir)) {
            for (const auto& e : fs::directory_iterator(dir)) {
                std::string file = e.path().filename().string();
                if (file.size() == 38 && file.rfind(rest, 0) == 0) {
                    if (!found.empty()) throw Error("short id " + name + " is ambiguous");
                    found = name.substr(0, 2) + file;
                }
            }
        }
        if (!found.empty()) return found;
    }
    return std::nullopt;
}

std::vector<std::string> Repo::branches() const {
    std::vector<std::string> out;
    fs::path heads = git / "refs" / "heads";
    if (fs::is_directory(heads))
        for (const auto& e : fs::recursive_directory_iterator(heads))
            if (e.is_regular_file()) out.push_back(fs::relative(e.path(), heads).generic_string());
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Index (the staging area), in Git's on-disk format version 2:
//
//   header : "DIRC" | version (4 bytes) | entry count (4 bytes)
//   entry  : ten 32-bit stat fields | 20-byte id | 16-bit flags | path
//            | 1-8 NUL bytes so the entry length is a multiple of 8
//   trailer: SHA-1 of everything before it
//
// All integers are big-endian. Using the real format means `git status` and
// `git commit` from official Git work on a repository created by this tool.
// ---------------------------------------------------------------------------

static void put32(std::string& out, uint32_t v) {
    for (int i = 3; i >= 0; i--) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

static uint32_t get32(const std::string& s, size_t pos) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data()) + pos;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

std::string mode_to_string(uint32_t mode) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%o", mode);
    return buf;
}

uint32_t mode_from_string(const std::string& mode) {
    return static_cast<uint32_t>(std::stoul(mode, nullptr, 8));
}

std::map<std::string, IndexEntry> Repo::read_index() const {
    std::map<std::string, IndexEntry> out;
    std::string data;
    if (!try_read_file(git / "index", data)) return out;  // no index yet: nothing staged

    if (data.size() < 32 || data.compare(0, 4, "DIRC") != 0) throw Error("corrupt index file");
    if (sha1(data.substr(0, data.size() - 20)) != data.substr(data.size() - 20))
        throw Error("index file checksum mismatch");
    uint32_t version = get32(data, 4);
    if (version != 2) throw Error("index version " + std::to_string(version) + " is not supported");
    uint32_t count = get32(data, 8);

    size_t pos = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 62 > data.size() - 20) throw Error("corrupt index file");
        IndexEntry e;
        e.ctime_s = get32(data, pos);
        e.ctime_ns = get32(data, pos + 4);
        e.mtime_s = get32(data, pos + 8);
        e.mtime_ns = get32(data, pos + 12);
        e.dev = get32(data, pos + 16);
        e.ino = get32(data, pos + 20);
        e.mode = get32(data, pos + 24);
        e.uid = get32(data, pos + 28);
        e.gid = get32(data, pos + 32);
        e.size = get32(data, pos + 36);
        e.oid = to_hex(data.substr(pos + 40, 20));
        unsigned flags = (static_cast<unsigned char>(data[pos + 60]) << 8) | static_cast<unsigned char>(data[pos + 61]);
        if ((flags >> 12) & 3) throw Error("index has unresolved merge entries, which are not supported");

        size_t name_start = pos + 62;
        size_t name_end = data.find('\0', name_start);
        if (name_end == std::string::npos) throw Error("corrupt index file");
        e.path = data.substr(name_start, name_end - name_start);

        size_t entry_len = 62 + e.path.size();
        pos += (entry_len + 8) & ~size_t(7);  // round up, always at least one NUL
        out[e.path] = std::move(e);
    }
    return out;  // optional extensions after the entries are ignored
}

void Repo::write_index(const std::map<std::string, IndexEntry>& entries) {
    std::string out = "DIRC";
    put32(out, 2);
    put32(out, static_cast<uint32_t>(entries.size()));
    for (const auto& [path, e] : entries) {
        size_t start = out.size();
        put32(out, e.ctime_s);
        put32(out, e.ctime_ns);
        put32(out, e.mtime_s);
        put32(out, e.mtime_ns);
        put32(out, e.dev);
        put32(out, e.ino);
        put32(out, e.mode);
        put32(out, e.uid);
        put32(out, e.gid);
        put32(out, e.size);
        out += from_hex(e.oid);
        unsigned name_len = static_cast<unsigned>(std::min<size_t>(path.size(), 0xfff));
        out.push_back(static_cast<char>(name_len >> 8));
        out.push_back(static_cast<char>(name_len & 0xff));
        out += path;
        size_t entry_len = out.size() - start;
        out.append(((entry_len + 8) & ~size_t(7)) - entry_len, '\0');
    }
    out += sha1(out);
    write_file_atomic(git / "index", out);
}

FileMap index_files(const std::map<std::string, IndexEntry>& index) {
    FileMap out;
    for (const auto& [path, e] : index) out[path] = FileEntry{mode_to_string(e.mode), e.oid};
    return out;
}
