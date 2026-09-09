#pragma once

// Counts $N placeholders in a query literal at compile time. A constexpr
// function returning a status rather than a consteval that fails, so the
// rejection lives in query.h's static_asserts (where the literal is in the
// diagnostic) while the scanner itself stays unit-testable.
//
// Postgres reads $N natively; sqlite accepts it too (verified 2026-09-08),
// which is why one scanner serves both dialects. Text between single
// quotes is skipped ('' is the escape, which simply toggles twice). A $
// not followed by digits -- dollar quoting, $identifier -- is ignored;
// dollar-quoted bodies are documented as unsupported inside a literal.
#pragma once

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string_view>

namespace sql::detail {
    struct placeholder_scan final {
        unsigned count = 0;
        const char* error = nullptr;
    };

    [[nodiscard]] constexpr placeholder_scan scan_placeholders(std::string_view q) noexcept {
        std::uint64_t seen = 0;
        unsigned max = 0;
        bool quoted = false;

        while (!q.empty()) {
            if (quoted) {
                const auto pos = q.find('\'');
                if (pos == std::string_view::npos) break;
                q.remove_prefix(pos + 1);
                quoted = false;
                continue;
            }

            const auto pos = q.find_first_of("'$");
            if (pos == std::string_view::npos) break;

            const char c = q[pos];
            q.remove_prefix(pos + 1);

            if (c == '\'') {
                quoted = true;
                continue;
            }

            auto digits = q | std::views::take_while([](const char ch) {
                return ch >= '0' && ch <= '9';
            });
            const auto dist = std::ranges::distance(digits);
            if (dist == 0) continue;

            unsigned n = 0;
            for (const char ch : digits) {
                if (n > 64) break;
                n = n * 10 + static_cast<unsigned>(ch - '0');
            }
            q.remove_prefix(dist);

            if (n == 0) return {.count = 0, .error = "placeholder numbering starts at $1"};
            if (n > 64) return {.count = 0, .error = "at most 64 placeholders"};

            seen |= std::uint64_t{1} << (n - 1);
            max = std::max(max, n);
        }

        if (const std::uint64_t expected = max == 0 ? 0 : (~std::uint64_t{0} >> (64 - max)); seen != expected) {
            return {.count = max, .error = "placeholders must be contiguous from $1"};
        }

        return {max, nullptr};
    }
}
