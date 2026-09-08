#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <fstr/fstr.h>

#include "sql/dialect.h"

namespace sql {
    namespace detail {
        // Parsed once, at compile time, by the placeholder_count/Style
        // constants below. There is no quote-awareness: a literal ? or $
        // inside a string constant counts as a bind site and demands an
        // argument -- the workaround is the one you want anyway, lift the
        // literal out and pass it as a bind ("SELECT ?" with "what?").
        struct placeholders final {
            std::size_t count{};
            std::optional<sql::dialect> style{};
        };

        constexpr placeholders parse_statement(const std::string_view v) {
            std::size_t qmarks = 0;
            int max_n = 0;
            std::uint64_t seen = 0;
            int style = 0;  // 0 none, 1 sqlite (?), 2 postgres ($n)

            const auto digit = [](const char c) { return c >= '0' && c <= '9'; };

            for (std::size_t i = 0; i < v.size(); ++i) {
                const char c = v[i];
                if (c == '?') {
                    if (i + 1 < v.size() && digit(v[i + 1]))
                        throw std::runtime_error("sql: numbered parameters (?NNN) are not supported");
                    if (style == 2) throw std::runtime_error("sql: cannot mix ? and $n bind sites");
                    style = 1;
                    ++qmarks;
                } else if (c == '$') {
                    if (i + 1 >= v.size() || !digit(v[i + 1]))
                        throw std::runtime_error("sql: named parameters ($name) are not supported");
                    if (style == 1) throw std::runtime_error("sql: cannot mix ? and $n bind sites");
                    style = 2;
                    int n = 0;
                    while (i + 1 < v.size() && digit(v[i + 1])) {
                        n = n * 10 + (v[++i] - '0');
                        if (n > 64) throw std::runtime_error("sql: more than 64 positional parameters");
                    }
                    max_n = n > max_n ? n : max_n;
                    seen |= std::uint64_t{1} << (n - 1);
                } else if (c == ':' && i + 1 < v.size() && v[i + 1] == ':') {
                    // A postgres cast, not a bind site -- skip both colons.
                    ++i;
                } else if (c == ':') {
                    throw std::runtime_error("sql: named parameters (:name) are not supported");
                } else if (c == '@') {
                    throw std::runtime_error("sql: named parameters (@name) are not supported");
                }
            }

            if (style == 2) {
                for (int i = 0; i < max_n; ++i)
                    if (!(seen >> i & 1))
                        throw std::runtime_error("sql: holes in $n bind sites ($1 and $3 without $2)");
                return placeholders{static_cast<std::size_t>(max_n), sql::dialect::postgres};
            }
            if (style == 1) return placeholders{qmarks, sql::dialect::sqlite};
            return placeholders{0, std::nullopt};
        }
    }

    // How many arguments a statement demands, and in which dialect's style.
    // Evaluating either constant runs the parser, so every violation above
    // surfaces as a compile error here rather than a runtime surprise (a ?5
    // allocates five sqlite parameters while a naive scan counts one).
    template <fstr::fstr S>
    inline constexpr std::size_t placeholder_count = detail::parse_statement(S.view()).count;

    template <fstr::fstr S>
    inline constexpr std::optional<sql::dialect> placeholder_style = detail::parse_statement(S.view()).style;
}
