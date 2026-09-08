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

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sql::detail {
    struct placeholder_scan final {
        unsigned count = 0;
        const char* error = nullptr;
    };

    [[nodiscard]] constexpr placeholder_scan scan_placeholders(const std::string_view q) noexcept {
        std::uint64_t seen = 0;
        unsigned max = 0;
        bool quoted = false;
        for (std::size_t i = 0; i < q.size(); ++i) {
            const char c = q[i];
            if (quoted) {
                if (c == '\'') quoted = false;
                continue;
            }
            if (c == '\'') {
                quoted = true;
                continue;
            }
            if (c != '$') continue;
            std::size_t j = i + 1;
            unsigned n = 0;
            bool digits = false;
            while (j < q.size() && q[j] >= '0' && q[j] <= '9' && n <= 64) {
                n = n * 10 + static_cast<unsigned>(q[j] - '0');
                digits = true;
                ++j;
            }
            if (!digits) continue;
            if (n == 0) return {.count = 0, .error = "placeholder numbering starts at $1"};
            if (n > 64) return {.count = 0, .error = "at most 64 placeholders"};
            seen |= std::uint64_t{1} << (n - 1);
            if (n > max) max = n;
            i = j - 1;
        }
        for (unsigned k = 1; k <= max; ++k) {
            if ((seen & (std::uint64_t{1} << (k - 1))) == 0) {
                return {.count = max, .error = "placeholders must be contiguous from $1"};
            }
        }
        return {.count = max, .error = nullptr};
    }
}
