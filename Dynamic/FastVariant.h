#pragma once

#include <type_traits>
#include <variant>

#define VARIANT_VISIT(idx) std::forward<decltype(f)>(f)(*std::get_if<idx>(std::forward<std::remove_reference_t<decltype(variant)>*>(&variant)))

template<std::size_t I = 0, typename Visitor, typename Variant>
inline constexpr decltype(auto) fastVisit(Visitor&& f, Variant&& variant) {
    constexpr std::size_t variant_size = std::variant_size_v<std::remove_cvref_t<Variant>>;
    if constexpr (I < variant_size) {
        if (variant.index() == I) return VARIANT_VISIT(I);
        if constexpr (I + 1 < variant_size) return fastVisit<I + 1>(f, variant);
    }
    __builtin_unreachable();
}
