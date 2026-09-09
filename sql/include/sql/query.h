#pragma once

// The four entry points. try_query is the primitive: check out a lease,
// run the literal on it, hand back the driver's expected untouched. The
// other three are thin over it: query throws that error, try_execute maps
// the result to affected(), execute throws. The two static_asserts make a
// malformed or mis-counted placeholder list a compile error at the call
// site, where the literal is in the diagnostic.
//
// Arguments are borrowed for the life of the returned task -- the frame
// holds references, not copies -- so await in the same full-expression
// that built the task, exactly as with when_all over a non-movable child;
// naming the task and awaiting it later with temporaries as arguments
// dangles. The lease is released before the result is handed back, which
// is correct because neither driver's result refers to its connection.

#include <cstdint>
#include <expected>
#include <utility>

#include <coro/task.h>
#include <fstr/fstr.h>

#include "sql/convert.h"
#include "sql/detail/stmt_key.h"
#include "sql/error.h"
#include "sql/pool.h"

namespace sql {
    template <source S>
    using result_t = typename S::driver::result;

    template <fstr::fstr Q, source S, bindable... Args>
    [[nodiscard]] coro::task<std::expected<result_t<S>, error>> try_query(const S& src, const Args&... args) {
        static_assert(detail::stmt_key<Q>::scan.error == nullptr,
                      "sql: malformed placeholders -- $N must run contiguously from $1, at most $64");
        static_assert(detail::stmt_key<Q>::scan.count == sizeof...(Args),
                      "sql: the query's $N placeholder count does not match the argument count");
        auto lease = co_await src.checkout();
        if (!lease) co_return std::unexpected(std::move(lease.error()));
        co_return co_await (*lease)->template execute<Q>(args...);
    }

    template <fstr::fstr Q, source S, bindable... Args>
    [[nodiscard]] coro::task<result_t<S>> query(const S& src, const Args&... args) {
        auto r = co_await try_query<Q>(src, args...);
        if (!r) throw std::move(r.error());
        co_return std::move(*r);
    }

    template <fstr::fstr Q, source S, bindable... Args>
    [[nodiscard]] coro::task<std::expected<std::uint64_t, error>> try_execute(const S& src, const Args&... args) {
        auto r = co_await try_query<Q>(src, args...);
        co_return std::move(r).transform([](const result_t<S>& res) {
            return static_cast<std::uint64_t>(res.affected());
        });
    }

    template <fstr::fstr Q, source S, bindable... Args>
    [[nodiscard]] coro::task<std::uint64_t> execute(const S& src, const Args&... args) {
        auto r = co_await try_query<Q>(src, args...);
        if (!r) throw std::move(r.error());
        co_return static_cast<std::uint64_t>(r->affected());
    }
}
