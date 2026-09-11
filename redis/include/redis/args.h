#pragma once

// What a command argument may be, and the argv one is assembled into:
// strings and
// byte spans are viewed in place, numbers are rendered into scratch
// strings this object owns, and a range contributes one entry per element,
// so DEL over a vector of keys and EVAL with runtime key lists need no
// special case. The flattened list is a vector because ranges make the
// count a runtime value. Views into the caller's arguments are borrowed,
// so a command_args must not outlive the full-expression that built it;
// the command functions never let it.

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <forward_list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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
        concept range_arg = !scalar_arg<T> && std::ranges::input_range<T> && scalar_arg<std::remove_cvref_t<std::ranges::range_reference_t<T>>>;
    }

    template <typename T>
    concept arg = detail::scalar_arg<std::remove_cvref_t<T>> || detail::range_arg<std::remove_cvref_t<T>>;

    namespace detail {
        class command_args final {
        public:
            template <arg... Args>
            explicit command_args(const Args&... values) {
                argv_.reserve((entries(values) + ... + 0));
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

            // How many argv entries an argument will contribute, so the
            // vector is reserved once. A range contributes one per element
            // rather than one in total, which is the whole point of DEL over
            // a vector of keys; a range that cannot say its size in advance
            // falls back to one and reallocates if it was wrong.
            template <typename T>
            [[nodiscard]] static std::size_t entries(const T& v) noexcept {
                if constexpr (std::ranges::sized_range<const T&> && !text_arg<std::remove_cvref_t<T>>) {
                    return std::ranges::size(v);
                } else {
                    return 1;
                }
            }

            // Rendered numbers need an address that stays put while later
            // arguments are added, which is why they cannot live in the
            // argv vector's own storage. A fixed arena holds the first few:
            // to_chars needs at most 24 bytes for any built-in type, and a
            // command with more than eight numbers in it is not the common
            // case. The list is the overflow path, and stays empty -- and
            // unallocated, on either standard library -- until it is needed.
            static constexpr std::size_t slot = 32;
            static constexpr std::size_t slots = 8;

            template <number_arg T>
            std::string_view render(const T v) {
                if (used_ + slot <= arena_.size()) {
                    char* const at = arena_.data() + used_;
                    if (const auto [end, ec] = std::to_chars(at, at + slot, v); ec == std::errc{}) {
                        used_ += slot;
                        return {at, static_cast<std::size_t>(end - at)};
                    }
                }
                char buf[64];
                const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
                overflow_.emplace_front(buf, end);
                return overflow_.front();
            }

            std::array<char, slot * slots> arena_{};
            std::size_t used_ = 0;
            std::forward_list<std::string> overflow_;
            std::vector<std::string_view> argv_;
        };
    }
}
