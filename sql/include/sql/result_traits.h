#pragma once

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "sql/cell.h"

namespace sql {
    // The conversion protocol: a T from one cell. Users specialize it; a
    // specialization may ignore c.from when its type decodes identically on
    // both dialects. Builtins below.
    template <typename T>
    struct result_traits;

    namespace detail {
        [[noreturn]] inline void conversion_failure(const std::string_view what) {
            throw std::runtime_error(std::string{what});
        }

        // get()-shaped parse over the sentinel-terminated cell bytes.
        template <typename T>
        [[nodiscard]] T parse_number(const cell c) {
            if (c.bytes.empty()) conversion_failure("sql: cannot parse an empty cell as a number");
            const std::string text{c.bytes};
            char* parsed = nullptr;
            if constexpr (std::is_floating_point_v<T>) {
                const double v = std::strtod(text.c_str(), &parsed);
                if (parsed != text.c_str() + text.size()) conversion_failure("sql: unparsable number");
                return static_cast<T>(v);
            } else {
                const long long v = std::strtoll(text.c_str(), &parsed, 10);
                if (parsed != text.c_str() + text.size()) conversion_failure("sql: unparsable integer");
                return static_cast<T>(v);
            }
        }

        inline void not_null(const cell c) {
            if (c.is_null) conversion_failure("sql: NULL into a non-optional type");
        }
    }

    template <typename T>
        requires std::is_arithmetic_v<T>
    struct result_traits<T> {
        [[nodiscard]] static T from(const cell c) {
            detail::not_null(c);
            return detail::parse_number<T>(c);
        }
    };

    template <>
    struct result_traits<bool> {
        [[nodiscard]] static bool from(const cell c) {
            detail::not_null(c);
            // sqlite stores 0/1; postgres emits t/f (and accepts 0/1).
            if (c.from == dialect::postgres) {
                const std::string_view v{c.bytes};
                if (v == "t" || v == "1") return true;
                if (v == "f" || v == "0") return false;
                detail::conversion_failure("sql: unparsable bool");
            }
            return detail::parse_number<bool>(c);
        }
    };

    template <>
    struct result_traits<std::string> {
        [[nodiscard]] static std::string from(const cell c) {
            detail::not_null(c);
            return std::string{c.bytes};
        }
    };

    template <>
    struct result_traits<std::string_view> {
        // Valid only while the result lives: the view points into the
        // result's cell buffer.
        [[nodiscard]] static std::string_view from(const cell c) {
            detail::not_null(c);
            return c.bytes;
        }
    };

    template <>
    struct result_traits<std::vector<std::byte>> {
        [[nodiscard]] static std::vector<std::byte> from(const cell c) {
            detail::not_null(c);
            const auto* const p = reinterpret_cast<const std::byte*>(c.bytes.data());
            return {p, p + c.bytes.size()};
        }
    };

    template <typename T>
    struct result_traits<std::optional<T>> {
        [[nodiscard]] static std::optional<T> from(const cell c) {
            if (c.is_null) return std::nullopt;
            return result_traits<T>::from(c);
        }
    };
}
