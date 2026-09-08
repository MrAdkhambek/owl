#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <string>
#include <type_traits>
#include <utility>

#include <coro/task.h>

#include <fstr/fstr.h>

#include "sql/bind.h"
#include "sql/dialect.h"
#include "sql/error.h"
#include "sql/placeholders.h"
#include "sql/pool.h"

namespace sql {
    namespace detail {
        template <typename Src, std::size_t N>
        [[nodiscard]] coro::task<std::expected<std::size_t, sql::error>>
        execute_impl(Src& src, const std::string sql_text, const std::array<bind_value, N> binds) {
            auto leased = co_await src.acquire();
            if (!leased) co_return std::unexpected(leased.error());
            auto r = co_await leased->run(sql_text, binds);
            if (!r) co_return std::unexpected(r.error());
            co_return r->rows_affected();
        }
    }

    // Affected-row count, no rows materialised. Use query when you need
    // rows (SELECT, INSERT ... RETURNING): a DML query without RETURNING
    // yields an empty result whose has_rows_affected may still be true.
    template <fstr::fstr S, source Src, typename... Args>
    [[nodiscard]] auto execute(Src& src, Args&&... args)
        -> coro::task<std::expected<std::size_t, sql::error>> {
        static_assert(sql::placeholder_count<S> == sizeof...(Args),
                      "sql: the statement's bind count does not match the arguments");
        static_assert(!sql::placeholder_style<S> || *sql::placeholder_style<S> == std::remove_cvref_t<Src>::dialect,
                      "sql: the statement's placeholder style does not match this source's dialect");
        return detail::execute_impl<Src, sizeof...(Args)>(src, std::string{S.view()},
                                                          detail::make_binds(std::forward<Args>(args)...));
    }
}
