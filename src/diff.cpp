#include "diff.h"

#include <algorithm>

#include "util.h"

// ---------------------------------------------------------------------------
// Myers diff
//
// Picture a grid with A along the x axis and B along the y axis. Moving right
// deletes a line of A, moving down inserts a line of B, and moving diagonally
// (free) keeps a line that is equal in both. A diff is a path from (0,0) to
// (N,M); the shortest edit script is the path with the fewest non-diagonal
// moves.
//
// Number the diagonals k = x - y. For D = 0, 1, 2, ... edits, the algorithm
// records, for each diagonal k, the furthest x reachable with exactly D edits
// (v[k]). The best D-path on diagonal k extends the best (D-1)-path on
// diagonal k-1 (one step right) or k+1 (one step down), then slides down the
// diagonal while lines match. The first D that reaches (N,M) is minimal.
//
// Cost is O((N+M) * D): fast when files are similar, which is the usual case.
// ---------------------------------------------------------------------------

std::vector<Edit> myers_diff(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    const int max = n + m;
    const int off = max + 1;  // shifts diagonal k into a non-negative array index
    std::vector<int> v(2 * max + 3, 0);

    // trace[d] is a snapshot of v for diagonals -d..d taken before round d.
    // It is what lets us walk the path backwards afterwards.
    std::vector<std::vector<int>> trace;
    int found_d = -1;

    for (int d = 0; d <= max && found_d < 0; d++) {
        trace.emplace_back(v.begin() + (off - d), v.begin() + (off + d + 1));
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[off + k - 1] < v[off + k + 1]))
                x = v[off + k + 1];  // step down from diagonal k+1 (an insertion)
            else
                x = v[off + k - 1] + 1;  // step right from diagonal k-1 (a deletion)
            int y = x - k;
            while (x < n && y < m && a[x] == b[y]) {  // slide along matching lines
                x++;
                y++;
            }
            v[off + k] = x;
            if (x >= n && y >= m) {
                found_d = d;
                break;
            }
        }
    }

    // Walk back from (N,M) to (0,0), one edit per round.
    std::vector<Edit> edits;
    int x = n, y = m;
    for (int d = found_d; d > 0; d--) {
        const std::vector<int>& pv = trace[d];
        auto at = [&](int k) { return pv[k + d]; };
        int k = x - y;
        int prev_k = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
        int prev_x = at(prev_k);
        int prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) {
            edits.push_back({' ', x - 1, y - 1});
            x--;
            y--;
        }
        if (x == prev_x) edits.push_back({'+', -1, y - 1});
        else edits.push_back({'-', x - 1, -1});
        x = prev_x;
        y = prev_y;
    }
    while (x > 0 && y > 0) {  // the leading run of equal lines (round 0)
        edits.push_back({' ', x - 1, y - 1});
        x--;
        y--;
    }
    std::reverse(edits.begin(), edits.end());
    return edits;
}

// ---------------------------------------------------------------------------
// Unified diff output
// ---------------------------------------------------------------------------

static void print_line(std::ostream& out, char prefix, const std::string& line) {
    out << prefix << line;
    if (line.empty() || line.back() != '\n') out << "\n\\ No newline at end of file\n";
}

bool print_hunks(const std::vector<std::string>& a, const std::vector<std::string>& b,
                 const std::vector<Edit>& edits, std::ostream& out) {
    const size_t context = 3, n = edits.size();
    bool any = false;
    size_t i = 0;
    // Lines of A and B consumed by edits[0..i).
    size_t a_done = 0, b_done = 0;

    while (i < n) {
        if (edits[i].type == ' ') {
            a_done++;
            b_done++;
            i++;
            continue;
        }
        any = true;

        // A hunk starts up to `context` lines before the change...
        size_t start = (i > context) ? i - context : 0;
        size_t back = i - start;
        size_t hunk_a = a_done - back, hunk_b = b_done - back;

        // ...and runs until changes are more than 2*context lines apart.
        size_t j = i;
        while (true) {
            while (j < n && edits[j].type != ' ') j++;
            size_t k = j;
            while (k < n && edits[k].type == ' ') k++;
            if (k < n && k - j <= 2 * context) {
                j = k;
                continue;
            }
            break;
        }
        size_t end = std::min(n, j + context);

        size_t a_count = 0, b_count = 0;
        for (size_t t = start; t < end; t++) {
            if (edits[t].type != '+') a_count++;
            if (edits[t].type != '-') b_count++;
        }
        // Unified format: line numbers are 1-based, except an empty range
        // names the line before it, and a count of exactly 1 is left out.
        auto range = [](size_t start0, size_t count) {
            std::string s = std::to_string(count ? start0 + 1 : start0);
            if (count != 1) s += "," + std::to_string(count);
            return s;
        };
        out << "@@ -" << range(hunk_a, a_count) << " +" << range(hunk_b, b_count) << " @@\n";
        for (size_t t = start; t < end; t++) {
            const Edit& e = edits[t];
            print_line(out, e.type, e.type == '+' ? b[e.b] : a[e.a]);
        }

        // Account for everything from i to end before continuing the scan.
        for (size_t t = i; t < end; t++) {
            if (edits[t].type != '+') a_done++;
            if (edits[t].type != '-') b_done++;
        }
        i = end;
    }
    return any;
}

// ---------------------------------------------------------------------------
// Three-way merge (diff3)
//
// Diff base->ours and base->theirs. A base line that survives unchanged in
// both is "stable" and splits the file into chunks. Between stable lines:
//   only ours changed   -> take ours
//   only theirs changed -> take theirs
//   both changed alike  -> take either
//   both changed differently -> conflict
// ---------------------------------------------------------------------------

// For each base line, the index of the line it was kept as in `other`, or -1.
static std::vector<int> match_table(const std::vector<std::string>& base, const std::vector<std::string>& other) {
    std::vector<int> match(base.size(), -1);
    for (const Edit& e : myers_diff(base, other))
        if (e.type == ' ') match[e.a] = e.b;
    return match;
}

MergeResult merge3(const std::string& base_text, const std::string& ours_text, const std::string& theirs_text,
                   const std::string& ours_label, const std::string& theirs_label) {
    std::vector<std::string> base = split_lines(base_text), ours = split_lines(ours_text),
                             theirs = split_lines(theirs_text);
    std::vector<int> in_ours = match_table(base, ours), in_theirs = match_table(base, theirs);

    MergeResult res;
    auto emit = [&](const std::vector<std::string>& lines, size_t from, size_t to) {
        for (size_t i = from; i < to; i++) res.text += lines[i];
    };
    auto same = [](const std::vector<std::string>& x, size_t x0, size_t x1, const std::vector<std::string>& y,
                   size_t y0, size_t y1) {
        return x1 - x0 == y1 - y0 && std::equal(x.begin() + x0, x.begin() + x1, y.begin() + y0);
    };
    auto end_line = [&] {
        if (!res.text.empty() && res.text.back() != '\n') res.text += '\n';
    };

    size_t ib = 0, io = 0, it = 0;  // cursors into base, ours, theirs
    const size_t nb = base.size();
    while (true) {
        // Stable run: base lines present at exactly the cursor in both sides.
        size_t run = 0;
        while (ib + run < nb && in_ours[ib + run] == static_cast<int>(io + run) &&
               in_theirs[ib + run] == static_cast<int>(it + run))
            run++;
        if (run > 0) {
            emit(base, ib, ib + run);
            ib += run;
            io += run;
            it += run;
            continue;
        }

        // Unstable chunk: everything up to the next base line kept by both sides.
        size_t nxt = ib;
        while (nxt < nb && (in_ours[nxt] < 0 || in_theirs[nxt] < 0)) nxt++;
        size_t eo = (nxt < nb) ? static_cast<size_t>(in_ours[nxt]) : ours.size();
        size_t et = (nxt < nb) ? static_cast<size_t>(in_theirs[nxt]) : theirs.size();

        if (same(ours, io, eo, base, ib, nxt)) {
            emit(theirs, it, et);  // only theirs changed
        } else if (same(theirs, it, et, base, ib, nxt) || same(ours, io, eo, theirs, it, et)) {
            emit(ours, io, eo);  // only ours changed, or both made the same change
        } else {
            res.conflict = true;
            end_line();
            res.text += "<<<<<<< " + ours_label + "\n";
            emit(ours, io, eo);
            end_line();
            res.text += "=======\n";
            emit(theirs, it, et);
            end_line();
            res.text += ">>>>>>> " + theirs_label + "\n";
        }
        ib = nxt;
        io = eo;
        it = et;
        if (nxt == nb) break;
    }
    return res;
}
