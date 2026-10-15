module;

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

export module IRPatch;

// Applies context diffs (diff -c) of LLVM IR files, as written by the TUI "diff" command.
// Unlike patch(1), context lines are compared with metadata IDs, attribute group IDs and numeric suffixes
// of global / struct type names masked out, since those differ between machines for the same code.
// IDs in inserted lines are translated using the matched context. An unknown !dbg is taken from a neighbouring line.
namespace IRPatch {

struct NormLine {
    std::string norm;
    std::vector<std::string> tokens; // Masked out parts, in order of appearance
    std::vector<size_t> positions;
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
    for (size_t dot = name.find_last_of('.'); dot != std::string_view::npos && dot != 0 && isNumber(name.substr(dot + 1)); dot = name.find_last_of('.')) {
        name = name.substr(0, dot);
    }
    return std::string(name);
}

inline NormLine normalize(std::string_view line) {
    NormLine res;
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        size_t end = i + 1;
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
inline std::string attachmentKind(std::string_view line, size_t pos) {
    if (pos < 2 || line[pos - 1] != ' ') return "";
    size_t k = pos - 2;
    while (k > 0 && isIdentChar(line[k])) k--;
    if (line[k] != '!' || k + 1 == pos - 1 || k < 2 || line.substr(k - 2, 2) != ", ") return "";
    return std::string(line.substr(k + 1, pos - 1 - (k + 1)));
}

inline std::string findDbg(std::string_view line) {
    size_t pos = line.find("!dbg !");
    if (pos == std::string_view::npos) return "";
    pos += 5;
    size_t end = pos + 1;
    while (end < line.size() && std::isdigit((unsigned char)line[end])) end++;
    return std::string(line.substr(pos, end - pos));
}

inline std::string translateLine(std::string const& line, std::map<std::string, std::string> const& tokmap, std::string const& fallback_dbg, std::string& err) {
    NormLine nl = normalize(line);
    std::string res;
    size_t copied = 0;
    for (size_t t = 0; t < nl.tokens.size(); t++) {
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
    for (size_t h_idx = 0; h_idx < hunks.size(); h_idx++) {
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

        std::map<std::string, std::string> tokmap;
        for (long k = 0; k < n; k++) {
            NormLine const& from = old_norm[k];
            NormLine const& to = tnorm[pos + k];
            for (size_t t = 0; t < std::min(from.tokens.size(), to.tokens.size()); t++) tokmap.emplace(from.tokens[t], to.tokens[t]);
        }

        // Merge: context lines are taken from the target, inserted lines from the patch
        std::vector<std::pair<std::string, bool>> merged; // (line, is from target)
        size_t i = 0, j = 0;
        while (i < h.old_lines.size() || j < h.new_lines.size()) {
            if (i < h.old_lines.size() && h.old_lines[i].kind != ' ') { i++; continue; }
            if (j < h.new_lines.size() && h.new_lines[j].kind != ' ') { merged.push_back({h.new_lines[j++].text, false}); continue; }
            if (i >= h.old_lines.size() || j >= h.new_lines.size()) return std::format("{}: Hunk {} has mismatched context", patch_path, h_idx + 1);
            merged.push_back({target[pos + i], true});
            i++; j++;
        }
        for (size_t k = 0; k < merged.size(); k++) {
            if (merged[k].second) continue;
            std::string fallback_dbg;
            for (size_t b = k; b-- > 0 && fallback_dbg.empty();) if (merged[b].second) fallback_dbg = findDbg(merged[b].first);
            for (size_t f = k + 1; f < merged.size() && fallback_dbg.empty(); f++) if (merged[f].second) fallback_dbg = findDbg(merged[f].first);
            merged[k].first = translateLine(merged[k].first, tokmap, fallback_dbg, err);
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

}
