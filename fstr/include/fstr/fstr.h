#pragma once

// A fixed-size string whose text is fixed at compile time.
//
// The constructor is consteval, so an fstr can only be born from a string
// literal -- which is precisely the point: the text is guaranteed to exist
// at compile time, and distinct literals give distinct types. That makes
// fstr usable as a non-type template parameter, where the type itself
// carries the text: tag types, compile-time keys, maps keyed by literal.
//
// N is the buffer size in bytes, trailing NUL included -- a literal of K
// characters yields fstr<K + 1>. The NUL is stored but never counted:
// size(), view(), begin() and end() all speak in terms of the text alone.
// Embedded NULs are rejected at construction, so the text is always
// exactly what the literal says it is.

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fstr {
    // The string itself. Plain aggregate, no state beyond the buffer:
    // the entire value of the type is the promise the consteval
    // constructor makes, and everything else is reading that buffer.
    template <std::size_t N>
    struct fstr {
        // The literal text, trailing NUL included. Left public -- fstr is
        // an aggregate and the NUL terminator means it degrades gracefully
        // wherever a C string is expected.
        char data[N]{};

        // The only way in: a string literal of exactly N bytes, trailing
        // NUL counted. consteval, so evaluation happens at compile time
        // by construction. An embedded NUL would silently truncate the
        // text at every string_view read, so it is a compile error
        // instead of a surprise.
        consteval fstr(const char (&text)[N]) {
            for (std::size_t i = 0; i + 1 < N; ++i) {
                if (text[i] == '\0') throw std::runtime_error("fstr::fstr: embedded NUL in literal");
            }
            std::copy_n(text, N, data); // text and trailing NUL, copied as-is
        }

        // The text without its trailing NUL. Every read of an fstr --
        // conversion, hashing, formatting -- goes through here, so the
        // NUL never leaks into string_view land.
        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return {data, N - 1};
        }

        // Implicit and free: an fstr reads anywhere a string_view does,
        // without spelling out .view().
        constexpr operator std::string_view() const noexcept { return view(); }

        // Length of the text in characters -- N minus the NUL that is
        // stored but not counted.
        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return N - 1;
        }

        // True only for the empty literal "". That is N == 1: a buffer
        // holding nothing but the NUL.
        [[nodiscard]] constexpr bool empty() const noexcept {
            return N == 1;
        }

        // begin/end bracket the text, not the buffer: end() stops at the
        // NUL, so range-for and algorithms see exactly the characters of
        // the literal.
        [[nodiscard]] constexpr const char* begin() const noexcept {
            return data;
        }

        [[nodiscard]] constexpr const char* end() const noexcept {
            return data + (N - 1);
        }

        // Lexicographic, via the defaulted three-way comparison -- the
        // same ordering the equivalent string_views would have.
        auto operator<=>(const fstr&) const = default;

        // The one escape hatch onto the heap, and deliberately explicit:
        // copying into a std::string allocates, so it should be a
        // decision, never an accident of overload resolution.
        [[nodiscard]] explicit constexpr operator std::string() const {
            return std::string{view()};
        }
    };

    // Deduces the buffer size from the literal, NUL included: "abc" gives
    // fstr<4>, "" gives fstr<1>.
    template <std::size_t N>
    fstr(const char (&)[N]) -> fstr<N>;
}

// Formats as the text, forwarding to the string_view formatter. This is
// what makes an fstr format as a string rather than as a range of chars,
// and it means the full set of standard string specifications -- "{:>8}"
// and friends -- works unchanged.
template <std::size_t N>
struct std::formatter<fstr::fstr<N>> : std::formatter<std::string_view> {
    auto format(const fstr::fstr<N>& s, auto& ctx) const {
        return std::formatter<std::string_view>::format(s.view(), ctx);
    }
};

// Hashes exactly as the equivalent string_view would. Equal texts hash
// equal regardless of which type carries them, so an fstr and a
// string_view of the same content are interchangeable as keys of
// hash-based containers.
template <std::size_t N>
struct std::hash<fstr::fstr<N>> {
    [[nodiscard]] std::size_t operator()(const fstr::fstr<N>& s) const noexcept {
        return std::hash<std::string_view>{}(s.view());
    }
};
