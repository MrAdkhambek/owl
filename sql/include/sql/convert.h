#pragma once

// The value vocabulary both drivers share: what may be bound as a
// parameter (bindable) and how text becomes a typed value (from_text).
// Written once here rather than per driver so the rules -- whole-text
// parsing, the bool spellings, the bytea hex form -- have one definition
// and one test. psql binds and reads everything as text; sqlite reaches
// for from_text only when a TEXT cell is asked for a number.

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "sql/error.h"

namespace sql {
    using blob = std::vector<std::byte>;
    using blob_view = std::span<const std::byte>;

    namespace detail {
        template <typename T>
        inline constexpr bool is_optional_v = false;

        template <typename T>
        inline constexpr bool is_optional_v<std::optional<T>> = true;

        template <typename T>
        struct optional_value {
            using type = T;
        };

        template <typename T>
        struct optional_value<std::optional<T>> {
            using type = T;
        };

        template <typename T>
        using optional_value_t = typename optional_value<T>::type;

        // bool and char are integral to the language but not to SQL: a
        // char would bind as a number, which is never what was meant.
        template <typename T>
        concept sql_integral = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char>;

        // Every spelling of "some text", including the literal itself,
        // which a `const Args&` parameter deduces as char[N].
        template <typename T>
        concept text_like = std::same_as<T, std::string> || std::same_as<T, std::string_view>
            || std::same_as<T, const char*> || std::same_as<T, char*>
            || (std::is_array_v<T> && std::same_as<std::remove_cv_t<std::remove_extent_t<T>>, char>);

        template <typename T>
        concept bytes_like = std::same_as<T, blob> || std::same_as<T, blob_view>;

        template <typename T>
        concept scalar_bindable = std::same_as<T, bool> || sql_integral<T> || std::floating_point<T>
            || text_like<T> || bytes_like<T>;

        [[nodiscard]] inline std::string_view text_of(const std::string& s) noexcept {
            return s;
        }

        [[nodiscard]] inline std::string_view text_of(const std::string_view s) noexcept {
            return s;
        }

        [[nodiscard]] inline std::string_view text_of(const char* const s) noexcept {
            return s;
        }

        [[nodiscard]] inline std::string hex_of(const blob_view bytes) {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out;
            out.reserve(2 + bytes.size() * 2);
            out += "\\x";
            for (const auto b : bytes) {
                const auto v = std::to_integer<unsigned>(b);
                out += digits[v >> 4];
                out += digits[v & 0xF];
            }
            return out;
        }

        [[nodiscard]] constexpr int hex_value(const char c) noexcept {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // Postgres' bytea output in hex format: \x followed by pairs. The
        // legacy escape format is refused rather than guessed at.
        [[nodiscard]] inline std::expected<blob, error> blob_from_hex(std::string_view text) {
            if (!text.starts_with("\\x")) {
                return std::unexpected(error{error_kind::conversion, "bytea is not in hex format"});
            }
            text.remove_prefix(2);
            if (text.size() % 2 != 0) {
                return std::unexpected(error{error_kind::conversion, "bytea hex has an odd digit count"});
            }
            blob out;
            out.reserve(text.size() / 2);
            for (std::size_t i = 0; i < text.size(); i += 2) {
                const int hi = hex_value(text[i]);
                const int lo = hex_value(text[i + 1]);
                if (hi < 0 || lo < 0) {
                    return std::unexpected(error{error_kind::conversion, "bytea hex has a non-hex digit"});
                }
                out.push_back(static_cast<std::byte>((hi << 4) | lo));
            }
            return out;
        }

        template <typename T>
        concept readable_scalar = std::same_as<T, bool> || sql_integral<T> || std::floating_point<T>
            || std::same_as<T, std::string> || std::same_as<T, std::string_view> || std::same_as<T, blob>;
    }

    // What a query argument may be. Closed on purpose: a type outside the
    // list is a compile error at the call site, not a runtime surprise.
    template <typename T>
    concept bindable = detail::scalar_bindable<std::remove_cvref_t<T>>
        || std::same_as<std::remove_cvref_t<T>, std::nullopt_t>
        || (detail::is_optional_v<std::remove_cvref_t<T>>
            && detail::scalar_bindable<detail::optional_value_t<std::remove_cvref_t<T>>>);

    // The text form a text-format driver sends. nullopt is SQL NULL, which is
    // why the return is optional rather than a string that could never say
    // "no value". Numbers use to_chars (shortest round-trip for floats).
    template <bindable T>
    [[nodiscard]] std::optional<std::string> to_text(const T& value) {
        using U = std::remove_cvref_t<T>;
        if constexpr (std::same_as<U, std::nullopt_t>) {
            return std::nullopt;
        } else if constexpr (detail::is_optional_v<U>) {
            if (!value) return std::nullopt;
            return to_text(*value);
        } else if constexpr (std::same_as<U, bool>) {
            return std::string{value ? "true" : "false"};
        } else if constexpr (detail::sql_integral<U> || std::floating_point<U>) {
            char buf[64];
            const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, value);
            return std::string{buf, end};
        } else if constexpr (detail::text_like<U>) {
            return std::string{detail::text_of(value)};
        } else {
            return detail::hex_of(blob_view{value});
        }
    }

    // Text to a typed value. Whole-text or nothing: "12abc" is a conversion
    // error, not 12, because a silently truncated number is the worst kind
    // of wrong. Postgres' bool is t/f; the others are accepted so sqlite
    // TEXT and hand-written fixtures read the same way.
    template <detail::readable_scalar T>
    [[nodiscard]] std::expected<T, error> from_text(const std::string_view text) {
        if constexpr (std::same_as<T, std::string>) {
            return std::string{text};
        } else if constexpr (std::same_as<T, std::string_view>) {
            return text;
        } else if constexpr (std::same_as<T, blob>) {
            return detail::blob_from_hex(text);
        } else if constexpr (std::same_as<T, bool>) {
            if (text == "t" || text == "true" || text == "1") return true;
            if (text == "f" || text == "false" || text == "0") return false;
            return std::unexpected(error{error_kind::conversion, "not a boolean: " + std::string{text}});
        } else {
            T out{};
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
            if (ec != std::errc{} || end != text.data() + text.size()) {
                return std::unexpected(error{error_kind::conversion, "not a number: " + std::string{text}});
            }
            return out;
        }
    }
}
