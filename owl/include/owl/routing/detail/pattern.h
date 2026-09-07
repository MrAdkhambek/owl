#pragma once

// Route patterns: /users/{id}/posts/{pid}
//
// A segment is either a literal or exactly "{name}". Parsed at compile time
// so a bad pattern is a static_assert, not a startup exception.

#include <array>
#include <cstddef>
#include <string_view>

#include "owl/http/request.h"
#include "owl/util/util.h"

namespace owl {
    inline constexpr std::size_t max_path_segments = 32;
}

namespace owl::detail {
    struct Segment {
        std::string_view text; // literal text, or the parameter name
        bool is_param = false;
    };

    struct PatternInfo {
        bool ok = false;
        const char* error = "uninitialised";
        std::size_t count = 0;
        std::size_t params = 0;
        std::array<Segment, max_path_segments> segments{};
    };

    [[nodiscard]] constexpr PatternInfo
    parse_pattern(const std::string_view pattern) noexcept {
        PatternInfo info{};
        if (pattern.empty()) {
            info.error = "pattern is empty";
            return info;
        }
        if (pattern.front() != '/') {
            info.error = "pattern must start with '/'";
            return info;
        }
        if (pattern.size() == 1) {
            // "/" is the root: zero segments, which is a valid route.
            info.ok = true;
            info.error = nullptr;
            return info;
        }

        const std::size_t n = pattern.size();
        for (std::size_t i = 0; i < n;) {
            ++i; // skip the '/'
            const std::size_t start = i;
            while (i < n && pattern[i] != '/') ++i;
            const std::string_view seg = pattern.substr(start, i - start);

            if (seg.empty()) {
                info.error = "empty path segment";
                return info;
            }
            if (info.count >= max_path_segments) {
                info.error = "too many segments";
                return info;
            }

            if (seg.front() == '{') {
                if (seg.back() != '}') {
                    info.error = "unterminated '{' in segment";
                    return info;
                }
                const std::string_view name = seg.substr(1, seg.size() - 2);
                if (name.empty()) {
                    info.error = "empty parameter name";
                    return info;
                }
                for (const char c : name) {
                    if (!util::is_word_char(c)) {
                        info.error = "invalid character in parameter name";
                        return info;
                    }
                }
                for (std::size_t k = 0; k < info.count; ++k) {
                    if (info.segments[k].is_param && info.segments[k].text == name) {
                        info.error = "duplicate parameter name";
                        return info;
                    }
                }
                if (info.params >= max_path_params) {
                    info.error = "too many parameters";
                    return info;
                }
                ++info.params;
                info.segments[info.count++] = Segment{name, true};
            } else {
                for (const char c : seg) {
                    if (c == '{' || c == '}') {
                        info.error = "brace inside a literal segment";
                        return info;
                    }
                }
                info.segments[info.count++] = Segment{seg, false};
            }
        }

        info.ok = true;
        info.error = nullptr;
        return info;
    }

    // Splits a request path into segments. Returns npos for anything that
    // cannot match: no leading '/', an empty segment, or too many.
    [[nodiscard]] inline std::size_t
    split_path(const std::string_view path, std::string_view* out, const std::size_t cap) noexcept {
        constexpr auto no_match = static_cast<std::size_t>(-1);

        if (path.empty() || path.front() != '/') [[unlikely]] return no_match;
        if (path.size() == 1) return 0; // "/" is the root

        // Hand-rolled rather than `trimmed | std::views::split('/')`: this
        // runs on every request, almost always over one or two segments,
        // and the range adaptor's machinery does not survive that at -O3.
        // Semantics are unchanged, including that "//", a trailing '/' and
        // an over-long path all report no_match.
        const std::size_t n = path.size();
        std::size_t count = 0;
        std::size_t i = 1; // skip the leading '/'

        for (;;) {
            const std::size_t start = i;
            while (i < n && path[i] != '/') ++i;

            if (i == start) [[unlikely]] return no_match; // "//"
            if (count >= cap) [[unlikely]] return no_match;
            out[count++] = path.substr(start, i - start);

            if (i == n) return count;
            ++i; // step over the '/'
            if (i == n) [[unlikely]] return no_match; // trailing '/'
        }
    }

    template <fstr::fstr Pattern>
    [[nodiscard]] constexpr bool
    declares(const std::string_view name) noexcept {
        constexpr PatternInfo info = parse_pattern(Pattern.view());
        for (std::size_t i = 0; i < info.count; ++i) {
            if (info.segments[i].is_param && info.segments[i].text == name) return true;
        }
        return false;
    }
}
