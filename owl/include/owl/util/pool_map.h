#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace owl {
    [[nodiscard]] constexpr char ascii_to_lower(const char c) noexcept {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    struct CaseInsensitiveHash {
        [[nodiscard]] std::size_t operator()(const std::string_view str) const noexcept {
            std::size_t seed = 0;
            for (const char c : str) {
                seed ^= std::hash<char>{}(ascii_to_lower(c)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }
            return seed;
        }
    };

    struct CaseInsensitiveEqual {
        [[nodiscard]] bool operator()(const std::string_view a, const std::string_view b) const noexcept {
            return std::ranges::equal(a, b, {}, ascii_to_lower, ascii_to_lower);
        }
    };

    class pool_map final : public std::unordered_map<std::string_view, std::string_view> {
        using base = std::unordered_map<std::string_view, std::string_view>;

    public:
        using base::at;
        using base::contains;
        using base::end;
        using base::find;

        pool_map() = default;

        // The from_range constructor ranges::to looks for. Filled by hand
        // rather than forwarded to the base: libstdc++ 14 has no from_range
        // constructor on unordered_map yet, and one insert per pair is what
        // it would do anyway.
        template <std::ranges::input_range R> requires std::convertible_to<std::ranges::range_reference_t<R>, value_type>
        pool_map(std::from_range_t, R&& r) {
            for (auto&& kv : r) base::insert(static_cast<value_type>(std::forward<decltype(kv)>(kv)));
        }

        [[nodiscard]] const std::string_view* find_value(const std::string_view key) const noexcept {
            if (const auto it = find(key); it != end()) {
                return &it->second;
            }
            return nullptr;
        }

        template <typename Func>
        std::string_view& compute_if_absent(const std::string_view key, Func&& compute) {
            if (const auto it = find(key); it != end()) {
                return it->second;
            }
            return emplace(key, std::invoke(std::forward<Func>(compute))).first->second;
        }
    };
}
