export module IRPatch;
import std;

// Applies context diffs (diff -c) of LLVM IR files, as written by the TUI "diff" command.
// Unlike patch(1), context lines are compared with metadata IDs, attribute group IDs and numeric suffixes
// of global / struct type names masked out, since those differ between machines for the same code.
// IDs in inserted lines are translated using the matched context. An unknown !dbg is taken from a neighbouring instruction of the same function.
namespace IRPatch {

struct NormLine {
    std::string norm;
    std::vector<std::string> tokens; // Masked out parts, in order of appearance
    std::vector<std::size_t> positions;
};

struct PatchLine {
    char kind; // ' ' context, '+' added, '-' removed, '!' changed
    std::string text;
};

struct Hunk {
    long old_start = 0;
    std::vector<PatchLine> old_lines;
    std::vector<PatchLine> new_lines;
};

inline bool isIdentChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$' || c == '-';
}

inline bool isNumber(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c); });
}

// ".str.20.7" -> ".str", "struct.foo.27" -> "struct.foo", "0" -> ""
inline std::string stripNumSuffix(std::string_view name) {
    if (isNumber(name)) return "";
    for (std::size_t dot = name.find_last_of('.'); dot != std::string_view::npos && dot != 0 && isNumber(name.substr(dot + 1)); dot = name.find_last_of('.')) {
        name = name.substr(0, dot);
    }
    return std::string(name);
}

inline NormLine normalize(std::string_view line) {
    NormLine res;
    std::size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        std::size_t end = i + 1;
        if ((c == '!' || c == '#') && end < line.size() && std::isdigit((unsigned char)line[end])) {
            while (end < line.size() && std::isdigit((unsigned char)line[end])) end++;
            res.norm += c;
        } else if (c == '@' || (c == '%' && (line.substr(end).starts_with("struct.") || line.substr(end).starts_with("class.") || line.substr(end).starts_with("union.")))) {
            while (end < line.size() && isIdentChar(line[end])) end++;
            res.norm += c + stripNumSuffix(line.substr(i + 1, end - i - 1));
        } else {
            res.norm += c;
            i = end;
            continue;
        }
        res.tokens.emplace_back(line.substr(i, end - i));
        res.positions.push_back(i);
        i = end;
    }
    return res;
}

// Returns the kind of the metadata attachment whose value starts at pos (e.g. "dbg" for ", !dbg !12"), or an empty string
inline std::string attachmentKind(std::string_view line, std::size_t pos) {
    if (pos < 2 || line[pos - 1] != ' ') return "";
    std::size_t k = pos - 2;
    while (k > 0 && isIdentChar(line[k])) k--;
    if (line[k] != '!' || k + 1 == pos - 1 || k < 2 || line.substr(k - 2, 2) != ", ") return "";
    return std::string(line.substr(k + 1, pos - 1 - (k + 1)));
}

inline std::string findDbg(std::string_view line) {
    std::size_t pos = line.find("!dbg !");
    if (pos == std::string_view::npos) return "";
    pos += 5;
    std::size_t end = pos + 1;
    while (end < line.size() && std::isdigit((unsigned char)line[end])) end++;
    return std::string(line.substr(pos, end - pos));
}

// An instruction of a function body, as opposed to a define, a closing brace, a label, a comment or a debug record
inline bool isInstructionLine(std::string_view line) {
    return line.size() > 2 && line.starts_with("  ") && line[2] != ' ' && line[2] != '#';
}

inline bool isFunctionBoundary(std::string_view line) {
    return line.starts_with("define ") || line.starts_with("}");
}

// The location for an inserted line at index k of the merged hunk whose own !dbg is unknown: That of the nearest instruction
// of the same function in the hunk, otherwise in the target outside of the hunk (e.g. for an insertion before the allocas at the start of a function).
// Never that of a define, which is the function's DISubprogram, and never one of another function. Empty if the function has none
inline std::string fallbackDbg(std::vector<std::pair<std::string, bool>> const& merged, std::size_t k, std::vector<std::string> const& target, long pos, long n) {
    bool hunk_has_start = false, hunk_has_end = false;
    for (std::size_t b = k; b-- > 0;) {
        if (isFunctionBoundary(merged[b].first)) { hunk_has_start = true; break; }
        if (!merged[b].second || !isInstructionLine(merged[b].first)) continue;
        if (std::string dbg = findDbg(merged[b].first); !dbg.empty()) return dbg;
    }
    for (std::size_t f = k + 1; f < merged.size(); f++) {
        if (isFunctionBoundary(merged[f].first)) { hunk_has_end = true; break; }
        if (!merged[f].second || !isInstructionLine(merged[f].first)) continue;
        if (std::string dbg = findDbg(merged[f].first); !dbg.empty()) return dbg;
    }
    for (long b = pos; !hunk_has_start && b-- > 0 && !isFunctionBoundary(target[b]);) {
        if (!isInstructionLine(target[b])) continue;
        if (std::string dbg = findDbg(target[b]); !dbg.empty()) return dbg;
    }
    for (long f = pos + n; !hunk_has_end && f < static_cast<long>(target.size()) && !isFunctionBoundary(target[f]); f++) {
        if (!isInstructionLine(target[f])) continue;
        if (std::string dbg = findDbg(target[f]); !dbg.empty()) return dbg;
    }
    return "";
}

inline std::string translateLine(std::string const& line, std::map<std::string, std::string> const& tokmap, std::string const& fallback_dbg, std::string& err) {
    NormLine nl = normalize(line);
    std::string res;
    std::size_t copied = 0;
    for (std::size_t t = 0; t < nl.tokens.size(); t++) {
        std::string const& tok = nl.tokens[t];
        res += line.substr(copied, nl.positions[t] - copied);
        copied = nl.positions[t] + tok.size();
        if (tokmap.contains(tok)) {
            res += tokmap.at(tok);
        } else if (tok[0] == '@' || tok[0] == '%') {
            res += tok; // Name not in context, assume it is unchanged
        } else if (std::string kind = attachmentKind(line, nl.positions[t]); !kind.empty()) {
            res.erase(res.size() - (kind.size() + 4)); // Drop ", !<kind> "
            if (kind == "dbg" && !fallback_dbg.empty()) res += ", !dbg " + fallback_dbg;
        } else {
            err = std::format("Cannot translate {} in inserted line \"{}\", it does not occur in the context", tok, line);
            return "";
        }
    }
    return res + line.substr(copied);
}

inline bool parseHunks(std::string const& path, std::vector<Hunk>& hunks, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "Could not open patch file " + path; return false; }
    std::string line;
    std::vector<PatchLine>* section = nullptr;
    bool expect_header = false;
    while (std::getline(in, line)) {
        if (line.starts_with("***************")) {
            hunks.emplace_back();
            expect_header = true;
            section = nullptr;
        } else if (hunks.empty() || line.starts_with("\\")) {
            continue; // Diff history, file headers, "\ No newline at end of file"
        } else if (expect_header) {
            if (!line.starts_with("*** ")) { err = std::format("{}: Malformed hunk header \"{}\"", path, line); return false; }
            hunks.back().old_start = std::atol(line.c_str() + 4);
            expect_header = false;
            section = &hunks.back().old_lines;
        } else if (section == &hunks.back().old_lines && line.starts_with("--- ")) {
            section = &hunks.back().new_lines;
        } else if (section) {
            char kind = line.empty() ? ' ' : line[0];
            if (kind != ' ' && kind != '+' && kind != '-' && kind != '!') { err = std::format("{}: Malformed hunk line \"{}\"", path, line); return false; }
            section->push_back({kind, line.size() > 2 ? line.substr(2) : ""});
        }
    }
    // diff omits a section if it contains no changes
    for (Hunk& h : hunks) {
        if (h.old_lines.empty()) std::copy_if(h.new_lines.begin(), h.new_lines.end(), std::back_inserter(h.old_lines), [](PatchLine const& l) { return l.kind == ' '; });
        if (h.new_lines.empty()) std::copy_if(h.old_lines.begin(), h.old_lines.end(), std::back_inserter(h.new_lines), [](PatchLine const& l) { return l.kind == ' '; });
    }
    if (hunks.empty()) { err = path + ": No hunks found"; return false; }
    return true;
}

// Applies the patch to the IR file. Returns an error message, or an empty string on success.
// The target file is only written if all hunks apply.
export std::string applyPatch(std::string const& target_path, std::string const& patch_path) {
    std::vector<Hunk> hunks;
    std::string err;
    if (!parseHunks(patch_path, hunks, err)) return err;

    std::ifstream in(target_path);
    if (!in) return "Could not open IR file " + target_path;
    std::vector<std::string> target;
    for (std::string line; std::getline(in, line);) target.push_back(line);
    in.close();

    std::vector<NormLine> tnorm;
    tnorm.reserve(target.size());
    for (std::string const& l : target) tnorm.push_back(normalize(l));

    std::vector<std::string> result;
    long const tsize = target.size();
    long copied = 0;
    long offset = 0;
    for (std::size_t h_idx = 0; h_idx < hunks.size(); h_idx++) {
        Hunk const& h = hunks[h_idx];
        std::vector<NormLine> old_norm;
        for (PatchLine const& l : h.old_lines) old_norm.push_back(normalize(l.text));
        long const n = old_norm.size();
        if (n == 0) return std::format("{}: Hunk {} has no context", patch_path, h_idx + 1);

        // Find the match closest to the expected position, after the previous hunk
        auto matches = [&](long p) {
            for (long k = 0; k < n; k++) if (tnorm[p + k].norm != old_norm[k].norm) return false;
            return true;
        };
        long const expected = h.old_start - 1 + offset;
        long pos = -1;
        for (long d = 0; pos < 0 && (expected - d >= copied || expected + d + n <= tsize); d++) {
            if (expected - d >= copied && expected - d + n <= tsize && matches(expected - d)) pos = expected - d;
            else if (d != 0 && expected + d >= copied && expected + d + n <= tsize && matches(expected + d)) pos = expected + d;
        }
        if (pos < 0) return std::format("{}: Hunk {} at line {} does not match", patch_path, h_idx + 1, h.old_start);
        offset = pos - (h.old_start - 1);

        // Inserted lines are numbered as the new file: Their IDs are translated with the context lines of the new side
        std::map<std::string, std::string> tokmap;

        // Merge: context lines are taken from the target, inserted lines from the patch
        std::vector<std::pair<std::string, bool>> merged; // (line, is from target)
        std::size_t i = 0, j = 0;
        while (i < h.old_lines.size() || j < h.new_lines.size()) {
            if (i < h.old_lines.size() && h.old_lines[i].kind != ' ') { i++; continue; }
            if (j < h.new_lines.size() && h.new_lines[j].kind != ' ') { merged.push_back({h.new_lines[j++].text, false}); continue; }
            if (i >= h.old_lines.size() || j >= h.new_lines.size()) return std::format("{}: Hunk {} has mismatched context", patch_path, h_idx + 1);
            merged.push_back({target[pos + i], true});
            NormLine from = normalize(h.new_lines[j].text);
            NormLine const& to = tnorm[pos + i];
            for (std::size_t t = 0; t < std::min(from.tokens.size(), to.tokens.size()); t++) tokmap.emplace(from.tokens[t], to.tokens[t]);
            i++; j++;
        }
        for (std::size_t k = 0; k < merged.size(); k++) {
            if (merged[k].second) continue;
            merged[k].first = translateLine(merged[k].first, tokmap, fallbackDbg(merged, k, target, pos, n), err);
            if (!err.empty()) return std::format("{}: Hunk {}: {}", patch_path, h_idx + 1, err);
        }

        result.insert(result.end(), target.begin() + copied, target.begin() + pos);
        for (auto& [l, _] : merged) result.push_back(std::move(l));
        copied = pos + n;
    }
    result.insert(result.end(), target.begin() + copied, target.end());

    std::ofstream out(target_path, std::ios::trunc);
    for (std::string const& line : result) out << line << '\n';
    if (!out) return "Could not write IR file " + target_path;
    return "";
}

// Writing patches: The IR files are compared with the IDs masked out as applyPatch does, so that renumbered metadata
// is not a change (e.g. after an annotation created a location, which renumbers the metadata of all following lines).
// The hunks are written with the lines of the files, whose IDs applyPatch translates.

struct Edit {
    char kind; // ' ' line of both files, '-' only of the old file, '+' only of the new file
    long old_idx; // Index of the line in the old file, for ' ' and '-'
    long new_idx; // Index of the line in the new file, for ' ' and '+'
};

// Most changed lines diffLines handles. Its state grows with the square of the number of changed lines
constexpr long MAX_CHANGED_LINES = 5000;

// The furthest position on the diagonal k (x - y) of the edit graph reached by an edit script with d changes, from the furthest
// positions with d - 1 changes (prev, for the diagonals -(d - 1)..d - 1). Taken from diagonal k + 1 by adding a line (down)
// or from diagonal k - 1 by removing one (right), whichever reaches further. -1 if neither is possible
inline std::pair<long, bool> diagonalStep(std::vector<std::int32_t> const& prev, long d, long k, long n, long m) {
    long from_down = k + 1 <= d - 1 ? prev[k + 1 + d - 1] : -1;
    long from_right = k - 1 >= -(d - 1) ? prev[k - 1 + d - 1] : -1;
    if (from_down < 0 && from_right < 0) return {-1, false};
    bool down = from_right < 0 || (from_down >= 0 && from_right < from_down);
    long x = down ? from_down : from_right + 1;
    if (x > n || x - k > m || x - k < 0) return {-1, down};
    return {x, down};
}

// The shortest edit script turning the lines a into the lines b (Myers' algorithm), lines given by IDs.
// Empty if more than MAX_CHANGED_LINES lines are removed or added
inline std::optional<std::vector<Edit>> diffLines(std::vector<std::uint32_t> const& a, std::vector<std::uint32_t> const& b) {
    long const a_size = a.size(), b_size = b.size();
    long prefix = 0;
    while (prefix < a_size && prefix < b_size && a[prefix] == b[prefix]) prefix++;
    long suffix = 0;
    while (suffix < a_size - prefix && suffix < b_size - prefix && a[a_size - 1 - suffix] == b[b_size - 1 - suffix]) suffix++;
    long const n = a_size - prefix - suffix, m = b_size - prefix - suffix;
    auto same = [&](long x, long y) { return a[prefix + x] == b[prefix + y]; };

    // trace[d][k + d]: Furthest x on diagonal k with d changes
    std::vector<std::vector<std::int32_t>> trace;
    for (long d = 0;; d++) {
        if (d > MAX_CHANGED_LINES) return std::nullopt;
        std::vector<std::int32_t> v(2 * d + 1, -1);
        bool done = false;
        for (long k = -d; k <= d && !done; k += 2) {
            long x = d == 0 ? 0 : diagonalStep(trace.back(), d, k, n, m).first;
            if (x < 0) continue;
            while (x < n && x - k < m && same(x, x - k)) x++;
            v[k + d] = x;
            done = x == n && x - k == m;
        }
        trace.push_back(std::move(v));
        if (done) break;
    }

    // Walk back from the end through the moves that reached it
    std::vector<Edit> edits;
    for (long i = a_size - 1; i >= a_size - suffix; i--) edits.push_back({' ', i, b_size - a_size + i});
    long x = n, y = m;
    for (long d = static_cast<long>(trace.size()) - 1; d > 0; d--) {
        long k = x - y;
        bool down = diagonalStep(trace[d - 1], d, k, n, m).second;
        long prev_k = down ? k + 1 : k - 1;
        long prev_x = trace[d - 1][prev_k + d - 1];
        long prev_y = prev_x - prev_k;
        long moved_x = down ? prev_x : prev_x + 1;
        for (; x > moved_x; x--, y--) edits.push_back({' ', prefix + x - 1, prefix + y - 1});
        if (down) edits.push_back({'+', -1, prefix + prev_y});
        else edits.push_back({'-', prefix + prev_x, -1});
        x = prev_x;
        y = prev_y;
    }
    for (; x > 0; x--, y--) edits.push_back({' ', prefix + x - 1, prefix + y - 1});
    for (long i = prefix - 1; i >= 0; i--) edits.push_back({' ', i, i});
    std::reverse(edits.begin(), edits.end());
    return edits;
}

// Lines a patch does not change: The module ID, metadata definitions (e.g. a new location of an annotation) and globals
inline bool isIgnoredLine(std::string_view line) {
    return line.starts_with("; ModuleID") || line.starts_with("!") || line.starts_with("@");
}

// Lines not kept as context at the start or end of a hunk: Empty lines and comments, e.g. "; Function Attrs: ...", which differs between machines
inline bool isFillerLine(std::string_view line) {
    return line.empty() || line.starts_with(";");
}

// Writes the patch turning the IR file old_path into new_path, as a context diff (diff -c), the form applyPatch reads.
// Nothing if the files only differ in ignored lines. Returns an error message, or an empty string on success
export std::string writePatch(std::string const& old_path, std::string const& new_path, std::ostream& out) {
    // Lines with the same masked text get the same ID
    std::unordered_map<std::string, std::uint32_t> ids;
    auto readLines = [&ids](std::string const& path, std::vector<std::string>& lines, std::vector<std::uint32_t>& line_ids) {
        std::ifstream in(path);
        for (std::string line; std::getline(in, line);) {
            line_ids.push_back(ids.try_emplace(normalize(line).norm, static_cast<std::uint32_t>(ids.size())).first->second);
            lines.push_back(std::move(line));
        }
        return !in.bad() && in.eof();
    };
    std::vector<std::string> old_lines, new_lines;
    std::vector<std::uint32_t> old_ids, new_ids;
    if (!readLines(old_path, old_lines, old_ids)) return "Could not read " + old_path;
    if (!readLines(new_path, new_lines, new_ids)) return "Could not read " + new_path;
    std::optional<std::vector<Edit>> diff = diffLines(old_ids, new_ids);
    if (!diff) return std::format("More than {} lines of {} and {} differ", MAX_CHANGED_LINES, old_path, new_path);
    std::vector<Edit> const& edits = *diff;
    long const edit_count = edits.size();
    auto text = [&](Edit const& e) -> std::string const& { return e.kind == '+' ? new_lines[e.new_idx] : old_lines[e.old_idx]; };

    // Groups of consecutive changed lines [first, end), skipping those only changing ignored lines
    std::vector<std::pair<long, long>> groups;
    for (long i = 0; i < edit_count;) {
        if (edits[i].kind == ' ') { i++; continue; }
        long first = i;
        bool ignored = true;
        for (; i < edit_count && edits[i].kind != ' '; i++) ignored = ignored && isIgnoredLine(text(edits[i]));
        if (!ignored) groups.push_back({first, i});
    }

    constexpr long CONTEXT = 3;
    bool header = false;
    for (std::size_t g = 0; g < groups.size();) {
        // Groups at most two contexts apart share a hunk
        long change_first = groups[g].first, change_end = groups[g].second;
        for (g++; g < groups.size() && groups[g].first - change_end <= 2 * CONTEXT; g++) change_end = groups[g].second;
        long first = std::max(0L, change_first - CONTEXT), end = std::min(edit_count, change_end + CONTEXT);
        long trimmed_first = first, trimmed_end = end;
        while (trimmed_first < change_first && isFillerLine(text(edits[trimmed_first]))) trimmed_first++;
        while (trimmed_end > change_end && isFillerLine(text(edits[trimmed_end - 1]))) trimmed_end--;
        // applyPatch finds a hunk by its old lines: Keep the fillers if no other old line is left
        if (std::ranges::any_of(edits.begin() + trimmed_first, edits.begin() + trimmed_end, [](Edit const& e) { return e.kind != '+'; })) {
            first = trimmed_first;
            end = trimmed_end;
        }

        // Within a hunk, a group of removed and added lines is a change (!), as diff -c writes it
        std::vector<char> marks(end - first, ' ');
        bool has_removed = false, has_added = false;
        for (long i = first; i < end;) {
            if (edits[i].kind == ' ') { i++; continue; }
            long group_first = i;
            bool removes = false, adds = false;
            for (; i < end && edits[i].kind != ' '; i++) (edits[i].kind == '-' ? removes : adds) = true;
            for (long j = group_first; j < i; j++) marks[j - first] = removes && adds ? '!' : edits[j].kind;
            has_removed = has_removed || removes;
            has_added = has_added || adds;
        }
        auto range = [&](bool old_side) {
            std::vector<long> idx;
            for (long i = first; i < end; i++) {
                if (edits[i].kind == ' ' || edits[i].kind == (old_side ? '-' : '+')) idx.push_back(old_side ? edits[i].old_idx : edits[i].new_idx);
            }
            if (idx.empty()) return std::string("0");
            if (idx.size() == 1) return std::format("{}", idx.front() + 1);
            return std::format("{},{}", idx.front() + 1, idx.back() + 1);
        };

        if (!header) {
            out << "*** " << old_path << "\n--- " << new_path << "\n";
            header = true;
        }
        out << "***************\n*** " << range(true) << " ****\n";
        // As diff -c, a side without changes is left out, and each side has the lines of its file: Inserted lines are numbered
        // as the new file, as the context lines that translate them
        for (long i = first; i < end && has_removed; i++) if (edits[i].kind != '+') out << marks[i - first] << ' ' << old_lines[edits[i].old_idx] << '\n';
        out << "--- " << range(false) << " ----\n";
        for (long i = first; i < end && has_added; i++) if (edits[i].kind != '-') out << marks[i - first] << ' ' << new_lines[edits[i].new_idx] << '\n';
    }
    if (!out) return "Could not write the patch";
    return "";
}

}
