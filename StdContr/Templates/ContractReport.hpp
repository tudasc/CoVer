// Reports which parts of a boolean contract predicate were unsatisfied, while
// leaving the predicate's short-circuit evaluation exactly as written.
//
// The standard gives a violation handler only the predicate's source text
// (std::contracts::contract_violation::comment()), with no structure and no
// per-operand values. To get more, each leaf of the predicate is wrapped in
// TERM(expr, msg), where expr is the expression to check and msg the error
// to report on failure.
// Also need to wrap entire expression in cr::check{}(expr).
//
// TERM() yields cr::truth and used by connectives (||,&&,etc) as normal.
// A leaf that evaluates to false notes its message as it
// goes; on failure the handler is left with the leaves that were actually
// reached and found false, which is a (not-necessarily-minimal) unsat core

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(__GNUC__)
// Keeps report assembly out of the function being checked.
#define CR_COLD_PATH [[gnu::noinline, gnu::cold]]
#else
#define CR_COLD_PATH
#endif

// The most leaves one predicate can contribute to a core, and the per-thread
// cost: this many pointer/length pairs.
#ifndef CR_MAX_LEAVES
#define CR_MAX_LEAVES 32
#endif

namespace cr {

struct text {
    const char* data;
    std::size_t size;
};

inline constexpr std::size_t max_leaves = CR_MAX_LEAVES;

namespace detail {

constexpr bool is_identifier_char(char c) {
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Length of `name` where it occurs at `i`, otherwise 0.
inline std::size_t matches(const char* src, std::size_t size, std::size_t i, const char* name) {
    std::size_t n = 0;
    while (name[n] != '\0' && i + n < size && src[i + n] == name[n]) ++n;
    return name[n] == '\0' ? n : 0;
}

// Matches TERM( or CR_TERM( at `i`, on a token boundary; returns its length, or
// 0 for no match.
inline std::size_t term_prefix(const char* src, std::size_t size, std::size_t i) {
    if (i > 0 && is_identifier_char(src[i - 1])) return 0;
    if (const std::size_t n = matches(src, size, i, "CR_TERM(")) return n;
    return matches(src, size, i, "TERM(");
}

} // namespace detail

// This undoes the cr::/macro machinery to get the clean contract input
inline std::string clean_predicate(const char* comment) {
    std::string predicate;
    std::size_t size = 0;
    while (comment[size] != '\0') ++size;

    // Drop the enclosing check{}( ... ), leaving just the predicate inside it.
    std::size_t i = 0;
    std::size_t end = size;
    for (std::size_t p = 0; p + 2 < size; ++p) {
        if (comment[p] != '{' || comment[p + 1] != '}' || comment[p + 2] != '(') continue;
        for (std::size_t q = p + 2, depth = 0; q < size; ++q) {
            if (comment[q] == '(') ++depth;
            else if (comment[q] == ')' && --depth == 0) { i = p + 3; end = q; break; }
        }
        break;
    }

    while (i < end) {
        const std::size_t prefix = detail::term_prefix(comment, end, i);
        if (prefix == 0) {
            predicate += comment[i++];
            continue;
        }
        i += prefix;                                 // past "TERM("
        while (i < end && comment[i] == ' ') ++i;     // ... and any space after it
        bool copying = true;                         // cleared once the message starts
        for (std::size_t depth = 1; i < end;) {
            const char c = comment[i];
            if (c == '"' || c == '\'') {  // a literal passes through whole, so that
                if (copying) predicate += c;  // parens and commas in it do not confuse
                ++i;                      // the scan
                while (i < end && comment[i] != c) {
                    const bool escaped = comment[i] == '\\' && i + 1 < end;
                    if (copying) predicate += comment[i];
                    ++i;
                    if (escaped) {
                        if (copying) predicate += comment[i];
                        ++i;
                    }
                }
                if (i < end) {
                    if (copying) predicate += comment[i];
                    ++i;
                }
                continue;
            }
            if (c == ',' && depth == 1) {  // the message argument begins here
                copying = false;
                ++i;
                continue;
            }
            if (c == '(' || c == '[') ++depth;
            if ((c == ')' || c == ']') && --depth == 0) {  // TERM's own ')'
                ++i;
                break;
            }
            if (copying) predicate += c;
            ++i;
        }
    }
    return predicate;
}

namespace detail {

// Messages of the leaves reached and found false, in evaluation order.
struct trace {
    text entries[max_leaves];
    // Counts past max_leaves rather than saturating, so a report can tell
    // whether it holds every failing leaf or only the first max_leaves of them.
    std::size_t count;
};

struct report {
    const text* entries;
    std::size_t count;
    bool complete;
    bool valid;
};

// constinit, at namespace scope: a function-local thread_local would be
// initialized lazily, putting a guard variable check on the checked function's
// hot path. These are zero-initialized before the thread runs instead.
inline constinit thread_local trace trace_storage{};
inline constinit thread_local report report_storage{};

// Out of line, but not cold: a predicate that holds may still have reached a
// false leaf on the way, so this is not the failure path. Keeping it out of line
// costs a call per false leaf and keeps the checked function small.
[[gnu::noinline]] inline void note_false(text message) {
    trace& t = trace_storage;
    if (t.count < max_leaves) t.entries[t.count] = message;
    ++t.count;
}

// Out of line and cold: the checked function keeps none of this.
CR_COLD_PATH inline void record(std::size_t base, std::size_t reached) {
    const std::size_t room = base < max_leaves ? max_leaves - base : 0;
    const std::size_t stored = reached < room ? reached : room;

    report& r = report_storage;
    r.entries = trace_storage.entries + base;
    r.count = stored;
    r.complete = stored == reached;
    r.valid = true;
}

} // namespace detail

// ------------------------------------------------------------------ the leaves

// What TERM() evaluates to. operator! is withheld on purpose.
struct truth {
    bool value;

    constexpr operator std::int8_t() const noexcept { return value; }
};

void operator!(truth) = delete("negate inside the leaf instead: TERM(!x, \"...\")");

template <class T, std::size_t N>
constexpr truth note(T&& value, const char (&message)[N]) {
    static_assert(N > 1, "a leaf needs a message: TERM(x, \"x is not set!\")");

    const bool holds = static_cast<bool>(value);
    if !consteval {
        if (!holds) detail::note_false(text{message, N - 1});
    }
    return truth{holds};
}

// Wraps a predicate: cr::check{}(TERM(...) || TERM(...)).
class check {
public:
    constexpr check() noexcept {
        if !consteval {
            base_ = detail::trace_storage.count;
        }
    }

    // Records the report if the predicate failed, then hands back its value so
    // the contract fires as usual.
    template <class T>
    constexpr bool operator()(T&& value) const noexcept {
        const bool holds = static_cast<bool>(value);

        // During constant evaluation there is no handler to report to, and the
        // trace has static storage duration; just yield the value so that
        // predicates using check stay usable in constexpr functions.
        if consteval {
            return holds;
        }

        detail::trace& t = detail::trace_storage;
        if (!holds) detail::record(base_, t.count - base_);
        t.count = base_;  // leave the enclosing predicate's trace as we found it
        return holds;
    }

private:
    std::size_t base_ = 0;
};

// Check if violation came from cr::report triggerd by failing TERM
inline bool has_report() noexcept { return detail::report_storage.valid; }
// Unsat core info
inline std::size_t core_size() noexcept { return detail::report_storage.count; }
inline text core(std::size_t i) noexcept { return detail::report_storage.entries[i]; }
inline bool core_complete() noexcept { return detail::report_storage.complete; }
// Clear current reports
inline void clear_report() noexcept { detail::report_storage.valid = false; }

}

#define CR_TERM(expression, message) (::cr::note((expression), message))

#ifndef CR_NO_SHORT_MACROS
#define TERM(expression, message) CR_TERM(expression, message)
#endif
