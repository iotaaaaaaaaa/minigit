// Line-based diff (Myers) and three-way merge (diff3).
#pragma once

#include <ostream>
#include <string>
#include <vector>

// One step of an edit script that turns sequence A into sequence B.
struct Edit {
    char type;  // ' ' keep, '-' delete from A, '+' insert from B
    int a;      // index into A (for ' ' and '-'), else -1
    int b;      // index into B (for ' ' and '+'), else -1
};

// Shortest edit script from a to b, using Myers' O(ND) algorithm.
std::vector<Edit> myers_diff(const std::vector<std::string>& a, const std::vector<std::string>& b);

// Prints the edit script as unified-diff hunks ("@@ -1,4 +1,5 @@ ...") with
// three lines of context. Returns false if there were no changes.
bool print_hunks(const std::vector<std::string>& a, const std::vector<std::string>& b,
                 const std::vector<Edit>& edits, std::ostream& out);

struct MergeResult {
    std::string text;
    bool conflict = false;
};

// Merges two descendants of `base`. Regions changed on only one side take
// that side; regions changed differently on both sides become conflict blocks.
MergeResult merge3(const std::string& base, const std::string& ours, const std::string& theirs,
                   const std::string& ours_label, const std::string& theirs_label);
