#pragma once

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <coro/task.h>

#include "sql/bind.h"
#include "sql/dialect.h"
#include "sql/error.h"
#include "sql/reactor.h"
#include "sql/resume.h"
#include "sql/result.h"

namespace sql {
    template <typename D>
    class pool;

    template <typename D>
    class tx;

    // What query/execute/transaction operate on: a pool, a sqlite_handle,
    // or a tx. The dialect travels as a static member so the api layer can
    // static_assert the placeholder style against it.
    template <typename S>
    concept source = requires(S& s) {
        S::dialect;
        s.acquire();
    };

    namespace detail {
        // The driver behind a source. Specialized for each source type.
        template <typename S>
        struct driver_traits;

        template <typename D>
        struct driver_traits<pool<D>> {
            using type = D;
        };

        template <typename S>
        inline constexpr bool is_tx_v = requires {
            std::remove_cvref_t<S>::is_tx;
        };
    }

    // The exclusive right to run statements on one connection. Armed
    // checkouts (from acquire) release their connection on destruction;
    // the borrowed copies a tx hands out run on the same connection but
    // never release -- the transaction owns the one release. Moving
    // transfers the release duty.
    template <typename D>
    class checkout final {
    public:
        checkout() = default;

        checkout(checkout&& o) noexcept
            : conn_(std::exchange(o.conn_, nullptr)),
              res_(o.res_),
              owner_(std::exchange(o.owner_, nullptr)) {
        }

        checkout& operator=(checkout&& o) noexcept {
            if (this != &o) release();
            conn_ = std::exchange(o.conn_, nullptr);
            res_ = o.res_;
            owner_ = std::exchange(o.owner_, nullptr);
            return *this;
        }

        ~checkout() {
            release();
        }

        checkout(const checkout&) = delete;
        checkout& operator=(const checkout&) = delete;

        // One statement on the held connection. Never leaves the
        // connection in a borrowed state -- SQL failure is data.
        [[nodiscard]] coro::task<std::expected<result, sql::error>>
        run(const std::string_view sql_text, const std::span<const bind_value> binds) const {
            co_return co_await conn_->run(sql_text, binds, res_);
        }

        // Idempotent; only meaningful on an armed checkout.
        void release() {
            if (owner_ == nullptr) return;
            std::exchange(owner_, nullptr)->release(*std::exchange(conn_, nullptr));
        }

    private:
        friend class pool<D>;
        friend class tx<D>;

        // The owner is held const: checkout and release are const members
        // over mutable internals on purpose -- a source is a capability,
        // not state the caller mutates.
        checkout(typename D::connection& conn, const sql::resumer* res, const pool<D>& owner)
            : conn_(&conn), res_(res), owner_(&owner) {
        }

        // A tx's view of the held connection: runs, never releases.
        [[nodiscard]] static checkout borrowed(typename D::connection& conn, const sql::resumer* res) {
            checkout c;
            c.conn_ = &conn;
            c.res_ = res;
            return c;
        }

        typename D::connection* conn_{};
        const sql::resumer* res_{};
        const pool<D>* owner_{};
    };

    // Connection pool over one driver. The sqlite pool is process-wide
    // (one reactor thread, one connection -- every statement in the
    // process serialises through it, and busy_timeout only ever waits on
    // another process); the postgres pool is per-worker (single-owner by
    // construction). No I/O happens in the constructor: connections open
    // on first acquire, and a failure is an unexpected on that operation.
    template <typename D>
    class pool final {
    public:
        static constexpr sql::dialect dialect = D::dialect;

        explicit pool(const typename D::config cfg)
            : cfg_(std::move(cfg)),
              external_(std::in_place),
              slots_(D::pool_size(cfg_)) {
        }

        pool(const typename D::config cfg, sql::any_reactor reactor)
            requires(!D::owns_thread)
            : cfg_(std::move(cfg)),
              external_(std::move(reactor)),
              slots_(D::pool_size(cfg_)) {
        }

        pool(const pool&) = delete;
        pool& operator=(const pool&) = delete;

        ~pool() {
            close();
            // The destructor is a backstop, not the intended path -- it
            // blocks, so it must not run on an event loop thread. Graceful
            // shutdown is co_await pool.drain().
            std::unique_lock lock(mutex_);
            idle_.wait(lock, [this] {
                return leased_ == 0 && waiters_.empty();
            });
        }

        // Marks the pool closed. Pending and subsequent checkouts fail with
        // unexpected(error); in-flight statements finish. Waiters are
        // resumed AFTER the lock is dropped: a resumed coroutine may run
        // arbitrary code, including re-entering the pool.
        void close() const {
            std::vector<const waiter*> to_resume;
            std::coroutine_handle<> drain{};
            {
                const std::lock_guard lock(mutex_);
                closed_ = true;
                while (!waiters_.empty()) {
                    auto* const w = waiters_.front();
                    waiters_.pop_front();
                    w->result = std::unexpected(sql::error{.message = "sql: pool is closed"});
                    to_resume.push_back(w);
                }
                if (idle_locked() && drain_waiter_) drain = std::exchange(drain_waiter_, {});
            }
            for (auto* const w : to_resume) resume_waiter(w);
            if (drain) drain.resume();
        }

        // Awaits until every connection is back and no waiters remain.
        // Implies close(). At most one drain at a time: a second drain
        // would overwrite the waiter slot; not a case the server has.
        [[nodiscard]] coro::task<> drain() const {
            close();
            for (;;) {
                co_await drain_awaiter{this};
                const std::lock_guard lock(mutex_);
                if (idle_locked()) co_return;
            }
        }

        // Checkout is awaitable and FIFO, with no timeout (non-goal). One
        // statement: checkout, run, release. A transaction checks out once
        // and holds the connection for the callback. The resumer is the
        // awaiter's own -- waiters resume through it, so sqlite releases
        // coming from the reactor thread post to the waiter's worker, and
        // postgres releases resume inline (same loop). An empty resumer
        // resumes inline.
        [[nodiscard]] coro::task<std::expected<checkout<D>, sql::error>>
        acquire(const sql::resumer* const res = nullptr) const {
            auto granted = co_await acquire_awaiter{this, res};
            if (!granted) co_return std::unexpected(granted.error());

            slot& s = slots_[*granted];
            if (s.conn == nullptr) {
                auto conn = make_connection();
                auto opened = co_await conn->connect(res);
                if (!opened) {
                    // Give the slot back and let the next waiter try; resume
                    // outside the lock (same rule as release).
                    const waiter* w = nullptr;
                    std::coroutine_handle<> drain{};
                    {
                        const std::lock_guard lock(mutex_);
                        s.leased = false;
                        --leased_;
                        w = grant_next_locked();
                        if (w == nullptr && idle_locked() && drain_waiter_)
                            drain = std::exchange(drain_waiter_, {});
                    }
                    if (w != nullptr) resume_waiter(w);
                    if (drain) drain.resume();
                    co_return std::unexpected(opened.error());
                }
                s.conn = std::move(conn);
            }
            co_return checkout<D>{*s.conn, res, *this};
        }

        void release(typename D::connection& conn) const {
            const waiter* w = nullptr;
            std::coroutine_handle<> drain{};
            {
                const std::lock_guard lock(mutex_);
                for (auto& s : slots_) {
                    if (s.conn.get() == &conn) {
                        s.leased = false;
                        --leased_;
                        // A postgres connection that died under us -- the
                        // server restarted -- is destroyed; the next checkout
                        // opens a fresh one, and only that operation sees the
                        // connect error. Without this, one restart poisons the
                        // slot for the life of the process.
                        if (!conn.healthy()) s.conn.reset();
                        break;
                    }
                }
                w = grant_next_locked();
                if (w == nullptr && idle_locked() && drain_waiter_)
                    drain = std::exchange(drain_waiter_, {});
            }
            if (w != nullptr) resume_waiter(w);
            if (drain) drain.resume();
        }

    private:
        struct slot final {
            mutable std::unique_ptr<typename D::connection> conn{};
            bool leased = false;
        };

        struct waiter final {
            std::coroutine_handle<> cont{};
            const sql::resumer* res{};
            std::expected<std::size_t, sql::error> result{};
        };

        struct acquire_awaiter final {
            const pool* p;
            const sql::resumer* res;
            waiter w{};

            bool await_ready() noexcept {
                const std::lock_guard lock(p->mutex_);
                p->check_affinity_locked();
                if (p->closed_) {
                    w.result = std::unexpected(sql::error{.message = "sql: pool is closed"});
                    return true;
                }
                for (std::size_t i = 0; i < p->slots_.size(); ++i) {
                    if (!p->slots_[i].leased) {
                        p->slots_[i].leased = true;
                        ++p->leased_;
                        w.result = i;
                        return true;
                    }
                }
                return false;
            }

            // Re-check under lock: another thread's close() may run between
            // await_ready and suspension.
            bool await_suspend(const std::coroutine_handle<> h) {
                w.cont = h;
                const std::lock_guard lock(p->mutex_);
                p->check_affinity_locked();
                if (p->closed_) {
                    w.result = std::unexpected(sql::error{.message = "sql: pool is closed"});
                    return false;
                }
                p->waiters_.push_back(&w);
                return true;
            }

            [[nodiscard]] std::expected<std::size_t, sql::error> await_resume() const {
                return std::move(w.result);
            }
        };

        struct drain_awaiter final {
            const pool* p;

            bool await_ready() const noexcept {
                const std::lock_guard lock(p->mutex_);
                return p->idle_locked();
            }

            bool await_suspend(const std::coroutine_handle<> h) const {
                const std::lock_guard lock(p->mutex_);
                if (p->idle_locked()) return false;
                p->drain_waiter_ = h;
                return true;
            }

            void await_resume() const noexcept {}
        };

        [[nodiscard]] bool idle_locked() const {
            return leased_ == 0 && waiters_.empty();
        }

        // The postgres pool is single-owner by construction -- every handle
        // to it came out of one worker's Context. A coroutine that migrated
        // workers and checked out is a programming error the affinity tests
        // demonstrate, not a case to make thread-safe; debug builds assert
        // it instead of silently serializing. Lock held.
        void check_affinity_locked() const {
#ifndef NDEBUG
            if constexpr (D::dialect == sql::dialect::postgres) {
                if (owner_thread_ == std::thread::id{}) {
                    owner_thread_ = std::this_thread::get_id();
                } else {
                    assert(owner_thread_ == std::this_thread::get_id());
                }
            }
#else
            (void)0;
#endif
        }

        // Lock held. Resumes a waiter whose slot was granted -- through the
        // waiter's own captured resumer.
        static void resume_waiter(const waiter* w) {
            if (w->res != nullptr && *w->res) {
                w->res->post(w->cont);
            } else {
                w->cont.resume();
            }
        }

        // Lock held. Pops the next waiter, grants it a slot, and records
        // the slot index in its result. The caller resumes it AFTER
        // dropping the lock -- a resumed coroutine may re-enter the pool.
        [[nodiscard]] const waiter* grant_next_locked() const {
            if (waiters_.empty()) return nullptr;
            auto* const w = waiters_.front();
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (!slots_[i].leased) {
                    slots_[i].leased = true;
                    ++leased_;
                    waiters_.pop_front();
                    w->result = i;
                    return w;
                }
            }
            return nullptr;
        }

        [[nodiscard]] std::unique_ptr<typename D::connection> make_connection() const {
            if constexpr (D::owns_thread) {
                return std::make_unique<typename D::connection>(cfg_, thread_);
            } else {
                return std::make_unique<typename D::connection>(cfg_, *external_);
            }
        }

        typename D::config cfg_;
        // Mutable: acquire is const and hands these to the connections it
        // opens; the pool's state-is-mutable-internals rule covers them.
        mutable std::optional<sql::any_reactor> external_;
        mutable std::conditional_t<D::owns_thread, typename D::reactor_type, std::monostate> thread_{};
        mutable std::vector<slot> slots_;

        mutable std::mutex mutex_;
        mutable std::condition_variable idle_;
        mutable std::deque<waiter*> waiters_;
        mutable std::coroutine_handle<> drain_waiter_{};
        mutable std::size_t leased_ = 0;
        mutable bool closed_ = false;
#ifndef NDEBUG
        mutable std::thread::id owner_thread_{};
#endif
    };
}
