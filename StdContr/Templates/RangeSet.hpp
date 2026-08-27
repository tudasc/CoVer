#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <flat_map>
#include <iterator>
#include <limits>
#include <utility>

// A set of integer ranges given as (base, size), each covering the right-open
// interval [base, base + size). Size is a count, not an extent: base 0 / size 5
// covers [0, 5) -- five values. Size 0 is the one exception to that: instead of
// covering nothing it covers the base alone, i.e. [base, base + 1).
//
// Ranges are keyed by base: erase(base) drops every range with that base.
//
// Caller-guaranteed invariant: ranges with *different* bases never overlap.
// Overlap only ever happens between ranges sharing a base, and those are
// collapsed to the widest -- inserting [0,5) and [0,10) stores [0,10), which is
// sound because a shared base means they are erased together anyway. Debug
// builds assert the invariant on every insert; it is what the lookup rests on.
// Abutting ranges are not overlapping: one may end exactly where the next
// begins.
//
// So the stored entries are sorted by base, unique by base, and pairwise
// disjoint. Ends are therefore sorted too, and the only range that can possibly
// cover x is the last one starting at or before x:
//
//     covered(x)  <=>  end(last entry with base <= x) > x

class range_set {
public:
    constexpr range_set() = default;

    // x inside any stored range?
    [[nodiscard]] constexpr bool contains(std::uintptr_t x) const noexcept {
        const auto it = entries_.upper_bound(x);
        return it != entries_.begin() && x < std::prev(it)->second;
    }

    constexpr void insert(std::uintptr_t base, std::uintptr_t size) {
        const std::uintptr_t end = end_of(base, size);
        const auto [it, inserted] = entries_.try_emplace(base, end);
        if (!inserted) it->second = std::max(it->second, end);  // same base: keep the widest
        assert((it == entries_.begin() || std::prev(it)->second <= it->first) &&
               "range overlaps the one before it, which has a different base");
        assert((std::next(it) == entries_.end() || it->second <= std::next(it)->first) &&
               "range overlaps the one after it, which has a different base");
    }

    // Removes every range with this base
    constexpr void erase(std::uintptr_t base) { entries_.erase(base); }

    constexpr void clear() noexcept { entries_.clear(); }

    // std::flat_map has no reserve(), so go through the underlying containers
    constexpr void reserve(std::size_t n) {
        auto parts = std::move(entries_).extract();
        parts.keys.reserve(n);
        parts.values.reserve(n);
        entries_.replace(std::move(parts.keys), std::move(parts.values));
    }

private:
    static constexpr std::uintptr_t end_of(std::uintptr_t base, std::uintptr_t size) noexcept {
        constexpr std::uintptr_t top = std::numeric_limits<std::uintptr_t>::max();
        const std::uintptr_t room = top - base;
        const std::uintptr_t extent = size == 0 ? 1 : size;
        return extent >= room ? top : base + extent;
    }

    std::flat_map<std::uintptr_t, std::uintptr_t> entries_;
};
