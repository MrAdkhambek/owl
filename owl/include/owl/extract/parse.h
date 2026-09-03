#pragma once

// Raw text to typed value: the conversion layer under the extractors. A
// type becomes a legal Header/Path/Query payload by specializing
// FromView<T> -- no base class, no trait to opt into. Specializations
// answer std::optional so "did not parse" has a spelling that carries no
// exception machinery, and FromContext is free to decide what that means
// in status codes.

#include <charconv>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "owl/util/util.h"

namespace owl {
    // Deliberately undefined: the specializations below are the whole
    // extension surface, so an unspecialized use is a compile error at the
    // call site rather than a silent fallback.
    template <class T>
    struct FromView;

    // The one spelling generic code calls. A variable rather than a type
    // keeps call sites bracket-free: fromString<int>("42").
    template <class T>
    inline constexpr FromView<T> fromString{};

    // The cheapest conversion -- the result borrows the raw text, so it
    // lives exactly as long as the request that produced it. Total: it
    // never fails, because text is already text.
    template <>
    struct FromView<std::string_view> {
        [[nodiscard]] std::optional<std::string_view>
        operator()(const std::string_view raw) const noexcept { return raw; }
    };

    // Materializes an owned copy. That allocation is what forfeits
    // nothrow_parse below, where the other specializations keep it.
    template <>
    struct FromView<std::string> {
        [[nodiscard]] std::optional<std::string>
        operator()(const std::string_view raw) const { return std::make_optional<std::string>(raw); }
    };

    // std::from_chars only knows 0 and 1 for bool, but callers send words
    // just as often. Every conventional spelling is accepted, matched
    // case-insensitively, and anything else is nullopt rather than an
    // error -- a flag sent as "TRUE" is still that flag.
    template <>
    struct FromView<bool> {
        [[nodiscard]] std::optional<bool>
        operator()(const std::string_view raw) const noexcept {
            using util::eq_ci;
            if (eq_ci(raw, "true") || eq_ci(raw, "1") || eq_ci(raw, "yes") || eq_ci(raw, "on")) return true;
            if (eq_ci(raw, "false") || eq_ci(raw, "0") || eq_ci(raw, "no") || eq_ci(raw, "off")) return false;
            return std::nullopt;
        }
    };

    // std::from_chars parses a leading '-' but deliberately not '+', while
    // signed values arrive with an explicit plus often enough -- so the
    // sign is stripped by hand, for signed types only: on an unsigned
    // target a '+' is noise rather than a sign, and stays rejected.
    //
    // The full-consumption check matters because from_chars stops at the
    // first non-digit: without it, "42abc" would parse as 42 and a
    // malformed parameter would pass as a prefix. Empty input is rejected
    // up front for the same reason -- it parses as nothing at all.
    template <std::integral T> requires (!std::same_as<T, bool>)
    struct FromView<T> {
        [[nodiscard]] std::optional<T>
        operator()(std::string_view raw) const noexcept {
            if (raw.empty()) return std::nullopt;
            if constexpr (std::is_signed_v<T>) {
                if (raw.front() == '+') {
                    raw.remove_prefix(1);
                    if (raw.empty()) return std::nullopt;
                }
            }
            T out{};
            const auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), out);
            if (ec != std::errc{} || ptr != raw.data() + raw.size()) return std::nullopt;
            return out;
        }
    };

    // The contract Header, Path and Query put on their value type: a
    // FromView that answers optional<T>. Satisfying it -- one
    // specialization -- is all it takes for a type to become a parameter
    // payload; there is no other registration step.
    template <class T>
    concept Parseable = requires(std::string_view raw) {
        { FromView<T>{}(raw) } -> std::same_as<std::optional<T>>;
    };

    // Whether fromString<T> is invocable on a string_view without
    // throwing. An allocation is what costs it, so FromView<std::string>
    // is false while the view and arithmetic specializations hold.
    template <class T>
    inline constexpr bool nothrow_parse = std::is_nothrow_invocable_v<FromView<T>, std::string_view>;
}
