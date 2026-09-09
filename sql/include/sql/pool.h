#pragma once

// pool<D>: the connections of one driver, handed out as leases, on ONE
// thread. Single-threaded by contract -- in owl every pool is per worker
// and lives on that worker's loop -- so there is no mutex, and the
// capability API is const with mutable internals: a handler extracts a
// const pool&, because a source is something you use, not something you
// own.
//
// Connections open lazily up to config.connections. A checkout takes an
// idle one, opens one if there is room, or parks in a FIFO of intrusive
// waiter nodes (no allocation). A released connection goes straight to
// the first waiter. One that reports !ok() is destroyed on release and
// replaced lazily; the first waiter is woken to do the replacing.
// Resumption runs a drain loop rather than a recursion, the way
// async_mutex::unlock does, so a chain of waiters that complete without
// suspending never deepens the stack.
//
// close() fails parked checkouts with error_kind::closed and destroys idle
// connections; a leased one dies on release. Destroying a pool with a
// lease outstanding is a contract violation, like coro's executors, and
// asserts in debug builds.

#include <algorithm>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

#include <coro/task.h>

#include "sql/concepts.h"
#include "sql/error.h"

namespace sql {
    template <driver D>
    class pool;

    // RAII checkout. Move-only; the return happens in the destructor. A
    // lease with no owner -- a transaction's -- returns nothing.
    template <driver D>
    class lease final {
    public:
        using connection = typename D::connection;

        lease() = default;

        lease(connection* const conn, const pool<D>* const owner) noexcept : conn_(conn), owner_(owner) {
        }

        lease(lease&& o) noexcept : conn_(std::exchange(o.conn_, nullptr)), owner_(std::exchange(o.owner_, nullptr)) {
        }

        lease& operator=(lease&& o) noexcept {
            if (this != &o) {
                release();
                conn_ = std::exchange(o.conn_, nullptr);
                owner_ = std::exchange(o.owner_, nullptr);
            }
            return *this;
        }

        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;

        ~lease() {
            release();
        }

        [[nodiscard]] connection* operator->() const noexcept {
            return conn_;
        }

        [[nodiscard]] connection& operator*() const noexcept {
            return *conn_;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return conn_ != nullptr;
        }

    private:
        void release() noexcept {
            if (conn_ != nullptr && owner_ != nullptr) owner_->give_back(conn_);
            conn_ = nullptr;
            owner_ = nullptr;
        }

        connection* conn_{};
        const pool<D>* owner_{};
    };

    // Anything query<> can run against: a pool, or a transaction on one.
    template <typename S>
    concept source = driver<typename S::driver> && requires(const S& s) {
        { s.checkout() } -> std::same_as<coro::task<std::expected<lease<typename S::driver>, error>>>;
    };

    template <driver D>
    class pool final {
    public:
        using driver = D;
        using connection = typename D::connection;

        pool(typename D::config cfg, typename D::io io)
            : cfg_(std::move(cfg)),
              io_(std::move(io)),
              limit_(cfg_.connections == 0 ? 1 : cfg_.connections) {
        }

        ~pool() {
            close();
            assert(connections_.empty() && "sql::pool destroyed with a lease outstanding");
        }

        pool(const pool&) = delete;
        pool& operator=(const pool&) = delete;

        [[nodiscard]] coro::task<std::expected<lease<D>, error>> checkout() const {
            for (;;) {
                if (closed_) co_return std::unexpected(closed_error());
                if (!idle_.empty()) {
                    connection* const c = idle_.back();
                    idle_.pop_back();
                    co_return lease<D>{c, this};
                }
                if (connections_.size() + opening_ < limit_) {
                    ++opening_;
                    auto opened = co_await connection::open(cfg_, io_);
                    --opening_;
                    if (closed_) co_return std::unexpected(closed_error());
                    if (!opened) {
                        // The slot is free again; a waiter parked meanwhile
                        // must get its own chance to open.
                        wake_one();
                        co_return std::unexpected(std::move(opened.error()));
                    }
                    connection* const c = opened->get();
                    connections_.push_back(std::move(*opened));
                    co_return lease<D>{c, this};
                }
                waiter w{};
                co_await park{this, &w};
                if (w.closed) co_return std::unexpected(closed_error());
                if (w.handed != nullptr) co_return lease<D>{w.handed, this};
                // Woken because a slot freed up: loop and open.
            }
        }

        void close() {
            if (closed_) return;
            closed_ = true;
            while (waiter* const w = waiters_.pop()) {
                w->closed = true;
                resume(w);
            }
            for (connection* const c : idle_) destroy(c);
            idle_.clear();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return connections_.size();
        }

        [[nodiscard]] std::size_t idle() const noexcept {
            return idle_.size();
        }

        [[nodiscard]] std::size_t waiting() const noexcept {
            std::size_t n = 0;
            for (const waiter* w = waiters_.head; w != nullptr; w = w->next) ++n;
            return n;
        }

    private:
        friend class lease<D>;

        struct waiter final {
            waiter* next{};
            std::coroutine_handle<> h{};
            connection* handed{};
            bool closed = false;
        };

        // Intrusive FIFO over the waiter nodes, which live in the parked
        // coroutines' frames: nothing is allocated to wait.
        struct waiter_queue final {
            waiter* head{};
            waiter* tail{};

            void push(waiter* const w) noexcept {
                w->next = nullptr;
                if (tail != nullptr) tail->next = w;
                else head = w;
                tail = w;
            }

            [[nodiscard]] waiter* pop() noexcept {
                waiter* const w = head;
                if (w == nullptr) return nullptr;
                head = w->next;
                if (head == nullptr) tail = nullptr;
                w->next = nullptr;
                return w;
            }
        };

        struct park final {
            const pool* self;
            waiter* w;

            [[nodiscard]] bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(const std::coroutine_handle<> h) const {
                w->h = h;
                self->waiters_.push(w);
            }

            void await_resume() const noexcept {
            }
        };

        void give_back(connection* const c) const noexcept {
            if (closed_ || !c->ok()) {
                destroy(c);
                wake_one();
                return;
            }
            if (waiter* const w = waiters_.pop()) {
                w->handed = c;
                resume(w);
                return;
            }
            idle_.push_back(c);
        }

        void wake_one() const noexcept {
            if (waiter* const w = waiters_.pop()) resume(w);
        }

        [[nodiscard]] static error closed_error() {
            return error{error_kind::closed, "pool is closed"};
        }

        // The drain loop. A resumption that happens while one is already
        // running -- a resumed waiter releasing its lease before it
        // suspends -- is queued and run by the outer call, so the depth
        // stays one whatever the chain length.
        void resume(waiter* const w) const noexcept {
            if (draining_) {
                ready_.push(w);
                return;
            }
            draining_ = true;
            w->h.resume();
            while (waiter* const next = ready_.pop()) next->h.resume();
            draining_ = false;
        }

        void destroy(connection* const c) const noexcept {
            std::erase_if(connections_, [c](const auto& owned) {
                return owned.get() == c;
            });
        }

        typename D::config cfg_;
        typename D::io io_;
        unsigned limit_;
        mutable std::vector<std::unique_ptr<connection>> connections_;
        mutable std::vector<connection*> idle_;
        mutable waiter_queue waiters_;
        mutable waiter_queue ready_;
        mutable unsigned opening_ = 0;
        mutable bool closed_ = false;
        mutable bool draining_ = false;
    };
}
