#pragma once

#include <array>
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
#include "sql/result.h"

namespace sql {
    namespace detail {
        // The shared body of query/execute: acquire once, run once. The
        // armed checkout releases even if run throws -- SQL failure is
        // data, and the release is not the caller's problem.
        template <typename Src, std::size_t N>
        [[nodiscard]] coro::task<std::expected<result, sql::error>>
        query_impl(Src& src, const std::string sql_text, const std::array<bind_value, N> binds) {
            auto leased = co_await src.acquire();
            if (!leased) co_return std::unexpected(leased.error());
            auto r = co_await leased->run(sql_text, binds);
            co_return std::move(r);
        }
    }

    // Runs one statement and returns its rows. Not a coroutine itself: the
    // binds are materialised eagerly (see bind.h) so the returned task owns
    // everything it needs -- store it, move it, await it later. src is a
    // pool, a sqlite_handle, or a tx, and must outlive the await (the same
    // rule as the pool's checkout). DML without RETURNING yields an empty
    // result; use execute when you only need affected rows.
    //
    // Awaiting a statement inside coro::when_any is forbidden: when_any
    // destroys its losing children, and a statement parked on a reactor
    // there would leave its checkout leaked and its resumer aiming at a
    // dead handle.
    template <fstr::fstr S, source Src, typename... Args>
    [[nodiscard]] auto query(Src& src, Args&&... args)
        -> coro::task<std::expected<result, sql::error>> {
        static_assert(sql::placeholder_count<S> == sizeof...(Args),
                      "sql: the statement's bind count does not match the arguments");
        static_assert(!sql::placeholder_style<S> || *sql::placeholder_style<S> == std::remove_cvref_t<Src>::dialect,
                      "sql: the statement's placeholder style does not match this source's dialect");
        return detail::query_impl<Src, sizeof...(Args)>(src, std::string{S.view()},
                                                        detail::make_binds(std::forward<Args>(args)...));
    }
}
