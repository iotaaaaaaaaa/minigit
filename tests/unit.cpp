// Unit tests for the pieces that can be checked in isolation: hashing, the
// diff algorithm and the three-way merge. No test framework, just asserts
// that report a line number.

#include <algorithm>
#include <iostream>
#include <random>
#include <sstream>

#include "diff.h"
#include "repo.h"
#include "util.h"

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        auto va = (a);                                                                    \
        auto vb = (b);                                                                    \
        if (!(va == vb)) {                                                                \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "\n  got:  [" << va     \
                      << "]\n  want: [" << vb << "]\n";                                   \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static void test_sha1() {
    // Published test vectors from the SHA-1 standard.
    CHECK_EQ(to_hex(sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK_EQ(to_hex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK_EQ(to_hex(sha1(std::string(1000000, 'a'))), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    // Lengths around the 64-byte block boundary exercise the padding logic.
    CHECK_EQ(to_hex(sha1(std::string(55, 'x'))), "cef734ba81a024479e09eb5a75b6ddae62e6abf1");
    CHECK_EQ(to_hex(sha1(std::string(56, 'x'))), "901305367c259952f4e7af8323f480d59f81335b");
    CHECK_EQ(to_hex(sha1(std::string(64, 'x'))), "bb2fa3ee7afb9f54c6dfb5d021f14b1ffe40c163");
    // What `echo hello | git hash-object --stdin` prints.
    CHECK_EQ(Repo::hash_object("blob", "hello\n"), "ce013625030ba8dba906f756967f9e9ca394464a");
}

static void test_hex_and_zlib() {
    std::string raw("\x00\xff\x10\xab", 4);
    CHECK_EQ(to_hex(raw), "00ff10ab");
    CHECK(from_hex("00ff10ab") == raw);
    std::string data = "some text " + std::string(5000, 'z') + std::string("\0\1\2", 3);
    CHECK(zlib_decompress(zlib_compress(data)) == data);
}

static void test_split_lines() {
    CHECK_EQ(split_lines("").size(), 0u);
    CHECK_EQ(split_lines("a\nb\n").size(), 2u);
    auto v = split_lines("a\nb");
    CHECK_EQ(v.size(), 2u);
    CHECK_EQ(v[1], "b");  // no newline invented for the last line
}

static void test_tree_round_trip() {
    std::vector<TreeEntry> entries = {
        {"100644", "b.txt", std::string(40, 'a')},
        {"40000", "b", std::string(40, 'b')},
        {"100755", "a", std::string(40, 'c')},
    };
    auto parsed = Repo::parse_tree(Repo::serialize_tree(entries));
    CHECK_EQ(parsed.size(), 3u);
    // Git order: "a", then "b.txt", then directory "b" (compared as "b/").
    CHECK_EQ(parsed[0].name, "a");
    CHECK_EQ(parsed[1].name, "b.txt");
    CHECK_EQ(parsed[2].name, "b");
    CHECK_EQ(parsed[2].oid, std::string(40, 'b'));
}

// Length of the longest common subsequence, by the textbook O(NM) table.
// Used as an independent oracle for the Myers implementation.
static int lcs(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::vector<std::vector<int>> dp(a.size() + 1, std::vector<int>(b.size() + 1, 0));
    for (size_t i = 1; i <= a.size(); i++)
        for (size_t j = 1; j <= b.size(); j++)
            dp[i][j] = a[i - 1] == b[j - 1] ? dp[i - 1][j - 1] + 1 : std::max(dp[i - 1][j], dp[i][j - 1]);
    return dp[a.size()][b.size()];
}

static void test_myers_random() {
    std::mt19937 rng(12345);
    for (int round = 0; round < 2000; round++) {
        std::vector<std::string> a(rng() % 25), b(rng() % 25);
        int alphabet = 1 + rng() % 5;  // few distinct lines, so many matches
        for (auto& s : a) s = std::string(1, char('a' + rng() % alphabet)) + "\n";
        for (auto& s : b) s = std::string(1, char('a' + rng() % alphabet)) + "\n";

        auto edits = myers_diff(a, b);

        // 1. Applying the script to A must produce exactly B, consuming A in order.
        std::vector<std::string> rebuilt;
        int next_a = 0, changes = 0;
        bool ordered = true;
        for (const auto& e : edits) {
            if (e.type != '+') ordered = ordered && e.a == next_a++;
            if (e.type == ' ') {
                ordered = ordered && a[e.a] == b[e.b];
                rebuilt.push_back(a[e.a]);
            } else if (e.type == '+') {
                rebuilt.push_back(b[e.b]);
            }
            if (e.type != ' ') changes++;
        }
        CHECK(ordered);
        CHECK(next_a == static_cast<int>(a.size()));
        CHECK(rebuilt == b);

        // 2. The script must be the shortest possible.
        CHECK_EQ(changes, static_cast<int>(a.size() + b.size()) - 2 * lcs(a, b));
    }
}

static std::string hunks(const std::string& a_text, const std::string& b_text) {
    auto a = split_lines(a_text), b = split_lines(b_text);
    std::ostringstream out;
    print_hunks(a, b, myers_diff(a, b), out);
    return out.str();
}

static void test_unified_output() {
    CHECK_EQ(hunks("a\nb\nc\n", "a\nB\nc\n"), "@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n");
    CHECK_EQ(hunks("", "x\n"), "@@ -0,0 +1 @@\n+x\n");
    CHECK_EQ(hunks("x\n", ""), "@@ -1 +0,0 @@\n-x\n");
    CHECK_EQ(hunks("a\n", "a"), "@@ -1 +1 @@\n-a\n+a\n\\ No newline at end of file\n");
    CHECK_EQ(hunks("same\n", "same\n"), "");

    // Two changes 20 lines apart must become two separate hunks.
    std::string a, b;
    for (int i = 1; i <= 30; i++) {
        a += "line" + std::to_string(i) + "\n";
        b += (i == 2 || i == 25 ? "CHANGED" : "line" + std::to_string(i)) + std::string("\n");
    }
    std::string h = hunks(a, b);
    CHECK_EQ(std::count(h.begin(), h.end(), '@'), 8);
    CHECK(h.find("@@ -1,5 +1,5 @@") != std::string::npos);
    CHECK(h.find("@@ -22,7 +22,7 @@") != std::string::npos);
}

static void test_merge3() {
    const std::string base = "one\ntwo\nthree\nfour\nfive\n";

    // Changes in different places merge cleanly.
    auto r = merge3(base, "ONE\ntwo\nthree\nfour\nfive\n", "one\ntwo\nthree\nfour\nFIVE\n", "ours", "theirs");
    CHECK(!r.conflict);
    CHECK_EQ(r.text, "ONE\ntwo\nthree\nfour\nFIVE\n");

    // Only one side changed.
    r = merge3(base, base, "one\ntwo\nfour\nfive\nsix\n", "ours", "theirs");
    CHECK(!r.conflict);
    CHECK_EQ(r.text, "one\ntwo\nfour\nfive\nsix\n");

    // Both sides made the identical change.
    r = merge3(base, "one\n2\nthree\nfour\nfive\n", "one\n2\nthree\nfour\nfive\n", "ours", "theirs");
    CHECK(!r.conflict);
    CHECK_EQ(r.text, "one\n2\nthree\nfour\nfive\n");

    // Both sides changed the same line differently.
    r = merge3(base, "one\ntwo\nOURS\nfour\nfive\n", "one\ntwo\nTHEIRS\nfour\nfive\n", "ours", "theirs");
    CHECK(r.conflict);
    CHECK_EQ(r.text, "one\ntwo\n<<<<<<< ours\nOURS\n=======\nTHEIRS\n>>>>>>> theirs\nfour\nfive\n");

    // Insertions at both ends.
    r = merge3(base, "zero\n" + base, base + "six\n", "ours", "theirs");
    CHECK(!r.conflict);
    CHECK_EQ(r.text, "zero\n" + base + "six\n");

    // One side deletes a line the other left alone.
    r = merge3(base, "one\nthree\nfour\nfive\n", "one\ntwo\nthree\nfour\nfive!\n", "ours", "theirs");
    CHECK(!r.conflict);
    CHECK_EQ(r.text, "one\nthree\nfour\nfive!\n");

    // New file on both sides with different content: empty base.
    r = merge3("", "a\n", "b\n", "ours", "theirs");
    CHECK(r.conflict);
    r = merge3("", "", "", "ours", "theirs");
    CHECK(!r.conflict);
    CHECK_EQ(r.text, "");
}

int main() {
    test_sha1();
    test_hex_and_zlib();
    test_split_lines();
    test_tree_round_trip();
    test_myers_random();
    test_unified_output();
    test_merge3();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "unit tests: all passed\n";
    return 0;
}
