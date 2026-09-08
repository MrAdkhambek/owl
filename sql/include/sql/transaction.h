#pragma once

// transaction: BEGIN, the body, then COMMIT on a value or ROLLBACK on an
// error, all on one leased connection. The body sees that connection as a
// transaction_source -- a source whose checkout is the connection itself
// -- so query<> and friends run inside the transaction unchanged.
//
// Errors are data (spec requirement 8) and the body's expected is the
// contract: an error in it rolls back and comes back as-is, a rollback
// failure losing to it. The library's one catch lives here: a body that
// throws (a throwing query<> inside it, say) is rolled back and rethrown
// with the connection kept, because abandoning a connection per exception
// would turn every routine SQL error into a reconnect. Only a ROLLBACK
// that itself fails abandons the connection, so a dirty one never returns
// to the pool. co_await is illegal inside a catch handler, which is why
// the exception is captured there and the rollback runs after the try.
//
// transaction_body<F, D> is the callback contract: invocable with an
// lvalue transaction_source<D>, returning exactly task<expected<T, error>>.
// A wrong shape fails at the call site with the concept in the
// diagnostic. transaction takes only a pool, so a nested transaction is a
// compile error rather than a runtime BEGIN inside BEGIN.

#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include <coro/task.h>
#include <fstr/fstr.h>

#include "sql/concepts.h"
#include "sql/error.h"
#include "sql/pool.h"

namespace sql {
    // The body's view of the connection: copyable, so `auto tx` works, with
    // a checkout that answers a non-returning lease. Its guard state lives
    // in transaction's frame, not here.
    template <driver D>
    class transaction_source final {
    public:
        using driver = D;

        explicit transaction_source(typename D::connection& conn) noexcept : conn_(&conn) {
        }

        [[nodiscard]] coro::task<std::expected<lease<D>, error>> checkout() const {
            co_return lease<D>{conn_, nullptr};
        }

    private:
        typename D::connection* conn_;
    };

    // The body's return type unwrapped to T. Left undefined for anything
    // but task<expected<T, error>>, which is what lets the concept below
    // name it as the whole shape check.
    template <typename Task>
    struct transaction_result;

    template <typename T>
    struct transaction_result<coro::task<std::expected<T, error>>> {
        using type = T;
    };

    template <typename F, typename D>
    using transaction_body_result_t = std::invoke_result_t<F&, transaction_source<D>&>;

    template <typename F, typename D>
    concept transaction_body = driver<D>
        && std::invocable<F&, transaction_source<D>&>
        && requires { typename transaction_result<transaction_body_result_t<F, D>>::type; };

    template <typename F, typename D>
    using transaction_value_t = typename transaction_result<transaction_body_result_t<F, D>>::type;

    namespace detail {
        inline constexpr fstr::fstr begin_sql{"BEGIN"};
        inline constexpr fstr::fstr commit_sql{"COMMIT"};
        inline constexpr fstr::fstr rollback_sql{"ROLLBACK"};

        // ROLLBACK, and if even that fails, make sure the connection can
        // never go back to the pool carrying an open transaction.
        template <driver D>
        coro::task<> rollback(typename D::connection& conn) {
            const auto r = co_await conn.template execute<rollback_sql>();
            if (!r) conn.abandon();
        }
    }

    template <driver D, transaction_body<D> F>
    [[nodiscard]] coro::task<std::expected<transaction_value_t<F, D>, error>>
    transaction(const pool<D>& src, F body) {
        using T = transaction_value_t<F, D>;
        using outcome_t = std::expected<T, error>;

        auto lease = co_await src.checkout();
        if (!lease) co_return std::unexpected(std::move(lease.error()));
        auto& conn = **lease;

        if (auto begun = co_await conn.template execute<detail::begin_sql>(); !begun) {
            co_return std::unexpected(std::move(begun.error()));
        }

        transaction_source<D> tx{conn};
        std::optional<outcome_t> outcome;
        std::exception_ptr thrown;
        try {
            outcome.emplace(co_await std::invoke(body, tx));
        } catch (...) {
            thrown = std::current_exception();
        }
        if (thrown) {
            co_await detail::rollback<D>(conn);
            std::rethrow_exception(thrown);
        }
        if (!*outcome) {
            co_await detail::rollback<D>(conn);
            co_return std::unexpected(std::move(outcome->error()));
        }
        if (auto committed = co_await conn.template execute<detail::commit_sql>(); !committed) {
            co_return std::unexpected(std::move(committed.error()));
        }
        if constexpr (std::is_void_v<T>) {
            co_return outcome_t{};
        } else {
            co_return std::move(**outcome);
        }
    }
}
