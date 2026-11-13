// Reports which parts of a boolean contract predicate were unsatisfied, while
// leaving the predicate's short-circuit evaluation exactly as written.
//
// The standard gives a violation handler only the predicate's source text
// (std::contracts::contract_violation::comment()), with no structure and no
// per-operand values. To get more, each leaf of the predicate is wrapped in
// TERM(expr, desc[, msg]), where expr is the expression to check, desc the
// subformula to report on failure, and msg an optional user message for it.
// Also need to wrap entire expression in cr::check{}(expr).
//
// TERM() yields cr::truth and used by connectives (||,&&,etc) as normal.
// A leaf that evaluates to false notes its message as it
// goes; on failure the handler is left with the leaves that were actually
// reached and found false, which is a (not-necessarily-minimal) unsat core

#pragma once

#include <cstddef>
#include <cstdint>

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

// A failing leaf: its subformula, and its user message (size 0 if it has none)
struct leaf {
    text formula;
    text message;
};

inline constexpr std::size_t max_leaves = CR_MAX_LEAVES;

namespace detail {

// The leaves reached and found false, in evaluation order.
struct trace {
    leaf entries[max_leaves];
    // Counts past max_leaves rather than saturating, so a report can tell
    // whether it holds every failing leaf or only the first max_leaves of them.
    std::size_t count;
};

struct report {
    const leaf* entries;
    std::size_t count;  // stored in entries
    std::size_t total;  // reached, can exceed count if more than max_leaves failed
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
[[gnu::noinline]] inline void note_false(leaf failed) {
    trace& t = trace_storage;
    if (t.count < max_leaves) t.entries[t.count] = failed;
    ++t.count;
}

// Out of line and cold: the checked function keeps none of this.
CR_COLD_PATH inline void record(std::size_t base, std::size_t reached) {
    const std::size_t room = base < max_leaves ? max_leaves - base : 0;
    const std::size_t stored = reached < room ? reached : room;

    report& r = report_storage;
    r.entries = trace_storage.entries + base;
    r.count = stored;
    r.total = reached;
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

template <class T, std::size_t N, std::size_t M = 1>
constexpr truth note(T&& value, const char (&formula)[N], const char (&message)[M] = "") {
    static_assert(N > 1, "a leaf needs a description: TERM(x, \"x is not set!\")");

    const bool holds = static_cast<bool>(value);
    if !consteval {
        if (!holds) detail::note_false(leaf{text{formula, N - 1}, text{message, M - 1}});
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
inline std::size_t core_total() noexcept { return detail::report_storage.total; }
inline leaf core(std::size_t i) noexcept { return detail::report_storage.entries[i]; }
inline bool core_complete() noexcept { return detail::report_storage.complete; }
// Clear current reports
inline void clear_report() noexcept { detail::report_storage.valid = false; }

}

#define CR_TERM(expression, ...) (::cr::note((expression), __VA_ARGS__))

#ifndef CR_NO_SHORT_MACROS
#define TERM(expression, ...) CR_TERM(expression, __VA_ARGS__)
#endif
