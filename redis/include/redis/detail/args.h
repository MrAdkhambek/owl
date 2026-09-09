#pragma once

// The argv a command is sent as, assembled from C++ values: strings and
// byte spans are viewed in place, numbers are rendered into scratch
// strings this object owns, and a range contributes one entry per element,
// so DEL over a vector of keys and EVAL with runtime key lists need no
// special case. The flattened list is a vector because ranges make the
// count a runtime value. Views into the caller's arguments are borrowed,
// so a command_args must not outlive the full-expression that built it;
// the command functions never let it.

#include <charconv>
#include <concepts>
#include <cstddef>
#include <deque>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace redis {
    namespace detail {
        template <typename T>
        concept text_arg = std::convertible_to<const T&, std::string_view> && !std::same_as<T, std::nullptr_t>;

        // bool and char are integral to the language and not to Redis.
        template <typename T>
        concept number_arg = (std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char>) || std::floating_point<T>;

        template <typename T>
        concept bytes_arg = std::same_as<T, std::span<const std::byte>> || std::same_as<T, std::vector<std::byte>>;

        template <typename T>
        concept scalar_arg = text_arg<T> || number_arg<T> || bytes_arg<T>;

        // A range of scalars, one level deep. std::string is a range of
        // char, but char is not a scalar_arg, so it stays text.
        template <typename T>
        concept range_arg = !scalar_arg<T> && std::ranges::input_range<T>
            && scalar_arg<std::remove_cvref_t<std::ranges::range_reference_t<T>>>;
    }

    template <typename T>
    concept arg = detail::scalar_arg<std::remove_cvref_t<T>> || detail::range_arg<std::remove_cvref_t<T>>;

    namespace detail {
        class command_args final {
        public:
            template <arg... Args>
            explicit command_args(const Args&... values) {
                argv_.reserve(sizeof...(Args));
                (add(values), ...);
            }

            command_args(const command_args&) = delete;
            command_args& operator=(const command_args&) = delete;

            [[nodiscard]] std::span<const std::string_view> argv() const noexcept {
                return argv_;
            }

        private:
            template <typename T>
            void add(const T& v) {
                using U = std::remove_cvref_t<T>;
                if constexpr (text_arg<U>) {
                    argv_.emplace_back(std::string_view{v});
                } else if constexpr (bytes_arg<U>) {
                    argv_.emplace_back(reinterpret_cast<const char*>(std::data(v)), std::size(v));
                } else if constexpr (number_arg<U>) {
                    argv_.emplace_back(render(v));
                } else {
                    for (const auto& e : v) add(e);
                }
            }

            // A deque keeps every rendered string at a stable address, so
            // the views handed out stay valid as more are added.
            template <number_arg T>
            std::string_view render(const T v) {
                char buf[64];
                const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
                scratch_.emplace_back(buf, end);
                return scratch_.back();
            }

            std::deque<std::string> scratch_;
            std::vector<std::string_view> argv_;
        };
    }
}
