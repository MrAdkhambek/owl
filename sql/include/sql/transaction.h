#pragma once

#include <expected>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <coro/task.h>

#include "sql/dialect.h"
#include "sql/error.h"
#include "sql/execute.h"
#include "sql/pool.h"
#include "sql/query.h"
#include "sql/result.h"

namespace sql {
    namespace detail {
        template <typename Src, typename F, typename T>
        [[nodiscard]] coro::task<std::expected<T, sql::error>> transaction_impl(Src& src, F fn);
    }

    // A transaction's view of one held connection. Copies of it run on the
    // held connection without checking out again; using one after the
    // transaction returned throws.
    template <typename D>
    class tx final {
    public:
        static constexpr sql::dialect dialect = pool<D>::dialect;
        static constexpr bool is_tx = true;

        tx() = default;

        // Never checks out: the transaction already holds the connection.
        [[nodiscard]] coro::task<std::expected<checkout<D>, sql::error>> acquire() const {
            if (state_ == nullptr || state_->expired) {
                throw std::logic_error("sql: tx used after its transaction returned");
            }
            co_return checkout<D>::borrowed(*state_->held.conn_, state_->held.res_);
        }

        [[nodiscard]] coro::task<std::expected<result, sql::error>>
        run(const std::string_view sql_text, const std::span<const bind_value> binds) const {
            auto c = co_await acquire();
            if (!c) co_return std::unexpected(c.error());
            co_return co_await c->run(sql_text, binds);
        }

    private:
        template <typename Src, typename F, typename T>
        friend coro::task<std::expected<T, sql::error>> detail::transaction_impl(Src&, F);

        friend struct detail::driver_traits<tx<D>>;

        struct state final {
            checkout<D> held{};
            bool expired = false;
        };

        explicit tx(std::shared_ptr<state> s) noexcept : state_(std::move(s)) {}

        std::shared_ptr<state> state_;
    };

    namespace detail {
        template <typename D>
        struct driver_traits<tx<D>> {
            using type = D;
        };

        template <typename E>
        inline constexpr bool is_sql_expected_v =
            requires {
                typename E::value_type;
                typename E::error_type;
            } && std::same_as<typename E::error_type, sql::error>;

        template <typename Src, typename F, typename T>
        [[nodiscard]] coro::task<std::expected<T, sql::error>> transaction_impl(Src& src, F fn) {
            using D = typename driver_traits<std::remove_cvref_t<Src>>::type;

            auto leased = co_await src.acquire();
            if (!leased) co_return std::unexpected(leased.error());

            auto state = std::make_shared<typename tx<D>::state>();
            state->held = std::move(*leased);
            const tx<D> handle{state};

            const auto begin = co_await handle.run("BEGIN", {});
            if (!begin) {
                state->held.release();
                state->expired = true;
                co_return std::unexpected(begin.error());
            }

            std::expected<T, sql::error> out{};
            // co_await is forbidden in a catch handler, so the rollback
            // happens after the try block and the exception is rethrown
            // once it completes: the exception propagates after the
            // rollback -- a failed .as<T>() is a programmer error and
            // stays an exception, while SQL failure is data.
            std::exception_ptr pending = nullptr;
            try {
                out = co_await fn(handle);
            } catch (...) {
                pending = std::current_exception();
            }
            if (pending) {
                const auto rb = co_await handle.run("ROLLBACK", {});
                state->held.release();
                state->expired = true;
                (void)rb;  // rollback error cannot beat the in-flight throw
                std::rethrow_exception(pending);
            }

            if (out) {
                const auto commit = co_await handle.run("COMMIT", {});
                state->held.release();
                state->expired = true;
                if (!commit) co_return std::unexpected(commit.error());
                co_return std::move(out);
            }
            const auto rb = co_await handle.run("ROLLBACK", {});
            state->held.release();
            state->expired = true;
            if (!rb) co_return std::unexpected(rb.error());  // the rollback error wins after fn failed
            co_return std::move(out);
        }
    }

    // Callback transaction: one checkout, BEGIN, the body, COMMIT on a
    // value / ROLLBACK on an error or a throw, then release. fn returns
    // task<expected<T, sql::error>>; the result is expected<T, sql::error>.
    //
    // Inside the body every statement goes through the tx, never the pool:
    // the sqlite pool has one connection and the transaction holds it, so
    // a pool-sourced statement in the body waits for a release that cannot
    // happen -- forever, since checkout has no timeout. Postgres only
    // moves the cliff to `size` concurrent transactions.
    template <typename Src, typename F>
    [[nodiscard]] auto transaction(Src& src, F&& fn) {
        static_assert(!detail::is_tx_v<Src>, "sql: nested transactions are not supported (no savepoints in v1)");
        using D = typename detail::driver_traits<std::remove_cvref_t<Src>>::type;
        using body_expected = coro::task_value_t<std::invoke_result_t<F&, tx<D>>>;
        static_assert(detail::is_sql_expected_v<body_expected>,
                      "sql: the transaction body must return task<expected<T, sql::error>>");
        return detail::transaction_impl<Src, std::remove_cvref_t<F>, typename body_expected::value_type>(
            src, std::forward<F>(fn));
    }
}
