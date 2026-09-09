#pragma once

// client: the worker's one command connection, shared by every caller on
// that worker. ONE thread, no locks; a const API over mutable internals,
// because a handler extracts a const client& -- a source is something you
// use, not something you own -- exactly as sql::pool.
//
// Why one connection and not a pool: Redis answers in order on a
// connection, so a FIFO of waiters is the entire matching scheme, and N
// callers appending to one output buffer get pipelining for free. A pool
// would buy concurrency Redis does not need and cost a lease type.
//
// Why no pump task: the head waiter reads its own reply and hands the
// baton to the next, so the client owns no coroutine frame and no frame
// can be destroyed while it runs -- every frame belongs to the caller
// awaiting it. Waiters are pushed in arrival order, so the head is the
// oldest and its deadline the earliest. A connection is opened lazily by
// the first command; callers that arrive meanwhile park on the connect
// queue and are woken together. Every waiter node lives in its caller's
// frame, so parking allocates nothing. Resumption runs sql::pool's drain
// loop, so a chain of waiters that complete without suspending never
// deepens the stack.
//
// Failure on the head's wait: a timeout is the head's timeout, everyone
// behind it gets connection; EOF or a protocol error is connection for
// all. The connection is dropped before anyone is resumed, so a waiter
// that retries reopens, with no backoff. A push on this connection is
// dropped: nothing here subscribes.
//
// close() refuses new commands with closed, fails callers parked on the
// connect queue, and lets in-flight commands finish -- sql's rule for a
// leased connection. Destroying a client with commands in flight is a
// contract violation and asserts in debug builds.

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <coro/io/reactor_ref.h>
#include <coro/task.h>

#include "redis/args.h"
#include "redis/config.h"
#include "redis/connection.h"
#include "redis/detail/deadline.h"
#include "redis/error.h"
#include "redis/reply.h"

namespace redis {
    class client final {
    public:
        client(config cfg, const coro::reactor_ref io) : cfg_(std::move(cfg)), io_(io) {
        }

        ~client() {
            close();
            assert(replies_.empty() && "redis::client destroyed with commands in flight");
        }

        client(const client&) = delete;
        client& operator=(const client&) = delete;

        // The primitive every command above is written over. argv is
        // borrowed until the command is queued, which happens before the
        // first suspension after the connection exists.
        [[nodiscard]] coro::task<std::expected<reply, error>> send(const std::span<const std::string_view> argv) const {
            if (closed_) co_return std::unexpected(closed_error());
            if (auto ready = co_await ensure(); !ready) co_return std::unexpected(std::move(ready.error()));
            if (!conn_->append(argv)) co_return std::unexpected(error{error_kind::connection, "connection is broken"});

            waiter w{};
            w.deadline = detail::deadline_for(cfg_.command_timeout);
            replies_.push(&w);
            if (replies_.head != &w) {
                co_await park{&w};
                if (w.failure) co_return std::unexpected(std::move(*w.failure));
            }

            // Head: the next reply on the wire is ours.
            auto r = co_await read_head(w);
            [[maybe_unused]] waiter* const popped = replies_.pop();
            assert(popped == &w);
            if (!r && r.error().kind() != error_kind::command) co_return std::unexpected(fail_rest(std::move(r.error())));
            pass_baton();
            co_return std::move(r);
        }

        void close() {
            if (closed_) return;
            closed_ = true;
            while (waiter* const w = connects_.pop()) {
                w->closed = true;
                resume(w);
            }
            if (replies_.empty()) conn_.reset();
        }

        [[nodiscard]] bool connected() const noexcept {
            return conn_ != nullptr && conn_->ok();
        }

        [[nodiscard]] std::size_t in_flight() const noexcept {
            return replies_.size;
        }

        // subscribe() opens its own connections from these.
        [[nodiscard]] const config& cfg() const noexcept {
            return cfg_;
        }

        [[nodiscard]] coro::reactor_ref io() const noexcept {
            return io_;
        }

        // The command API, as members: a client is the one thing a command
        // needs. sql spells it query<"...">(pool, args) because a query runs
        // against any driver's pool or transaction; here there is one client
        // type, so the object in hand is the whole story.
        //
        // Arguments are borrowed for the life of the returned task -- the
        // frame holds references, not copies -- so await in the same
        // full-expression that built the task; naming the task and awaiting
        // it later with temporaries as arguments dangles.

        template <arg... Args>
            requires (sizeof...(Args) > 0)
        [[nodiscard]] coro::task<std::expected<reply, error>> try_command(const Args&... args) const {
            const detail::command_args argv{args...};
            co_return co_await send(argv.argv());
        }

        template <arg... Args>
            requires (sizeof...(Args) > 0)
        [[nodiscard]] coro::task<reply> command(const Args&... args) const {
            auto r = co_await try_command(args...);
            if (!r) throw std::move(r.error());
            co_return std::move(*r);
        }

        // The seven the cache, rate-limit and pub/sub cases ask for. Each
        // maps one reply shape and nothing more; everything else is
        // command(). A try_ never throws for a server or link failure, but
        // still throws error{conversion} if the server answers a shape the
        // helper did not promise, because reading a reply is synchronous.

        [[nodiscard]] coro::task<std::expected<std::optional<std::string>, error>>
        try_get(const std::string_view key) const {
            auto r = co_await try_command("GET", key);
            if (!r) co_return std::unexpected(std::move(r.error()));
            co_return r->template as<std::optional<std::string>>();
        }

        [[nodiscard]] coro::task<std::optional<std::string>> get(const std::string_view key) const {
            auto r = co_await try_get(key);
            if (!r) throw std::move(r.error());
            co_return std::move(*r);
        }

        [[nodiscard]] coro::task<std::expected<void, error>>
        try_set(const std::string_view key, const std::string_view value) const {
            auto r = co_await try_command("SET", key, value);
            if (!r) co_return std::unexpected(std::move(r.error()));
            co_return std::expected<void, error>{};
        }

        [[nodiscard]] coro::task<std::expected<void, error>>
        try_set(const std::string_view key, const std::string_view value, const std::chrono::milliseconds ttl) const {
            auto r = co_await try_command("SET", key, value, "PX", ttl.count());
            if (!r) co_return std::unexpected(std::move(r.error()));
            co_return std::expected<void, error>{};
        }

        [[nodiscard]] coro::task<> set(const std::string_view key, const std::string_view value) const {
            auto r = co_await try_set(key, value);
            if (!r) throw std::move(r.error());
        }

        [[nodiscard]] coro::task<>
        set(const std::string_view key, const std::string_view value, const std::chrono::milliseconds ttl) const {
            auto r = co_await try_set(key, value, ttl);
            if (!r) throw std::move(r.error());
        }

        // One key or a range of keys: both are args, and DEL takes a list.
        template <arg Keys>
        [[nodiscard]] coro::task<std::expected<std::int64_t, error>> try_del(const Keys& keys) const {
            auto r = co_await try_command("DEL", keys);
            if (!r) co_return std::unexpected(std::move(r.error()));
            co_return r->template as<std::int64_t>();
        }

        template <arg Keys>
        [[nodiscard]] coro::task<std::int64_t> del(const Keys& keys) const {
            auto r = co_await try_del(keys);
            if (!r) throw std::move(r.error());
            co_return *r;
        }

        [[nodiscard]] coro::task<std::expected<std::int64_t, error>> try_incr(const std::string_view key) const {
            auto r = co_await try_command("INCR", key);
            if (!r) co_return std::unexpected(std::move(r.error()));
            co_return r->template as<std::int64_t>();
        }

        [[nodiscard]] coro::task<std::int64_t> incr(const std::string_view key) const {
            auto r = co_await try_incr(key);
            if (!r) throw std::move(r.error());
            co_return *r;
        }

        // true when the key existed and now has the TTL.
        [[nodiscard]] coro::task<std::expected<bool, error>>
        try_expire(const std::string_view key, const std::chrono::seconds ttl) const {
            auto r = co_await try_command("EXPIRE", key, ttl.count());
            if (!r) co_return std::unexpected(std::move(r.error()));
            co_return r->template as<bool>();
        }

        [[nodiscard]] coro::task<bool> expire(const std::string_view key, const std::chrono::seconds ttl) const {
            auto r = co_await try_expire(key, ttl);
            if (!r) throw std::move(r.error());
            co_return *r;
        }

        // EVAL script numkeys key... arg...; the key count is the range's
        // size, which is why keys must be a range and not a single string.
        template <typename Keys, typename Args>
            requires detail::range_arg<Keys> && detail::range_arg<Args>
        [[nodiscard]] coro::task<std::expected<reply, error>>
        try_eval(const std::string_view script, const Keys& keys, const Args& args) const {
            co_return co_await try_command("EVAL", script, std::ranges::distance(keys), keys, args);
        }

        template <typename Keys, typename Args>
            requires detail::range_arg<Keys> && detail::range_arg<Args>
        [[nodiscard]] coro::task<reply> eval(const std::string_view script, const Keys& keys, const Args& args) const {
            auto r = co_await try_eval(script, keys, args);
            if (!r) throw std::move(r.error());
            co_return std::move(*r);
        }

        // The number of subscribers the message reached.
        [[nodiscard]] coro::task<std::expected<std::int64_t, error>>
        try_publish(const std::string_view channel, const std::string_view payload) const {
            auto r = co_await try_command("PUBLISH", channel, payload);
            if (!r) co_return std::unexpected(std::move(r.error()));
            co_return r->template as<std::int64_t>();
        }

        [[nodiscard]] coro::task<std::int64_t>
        publish(const std::string_view channel, const std::string_view payload) const {
            auto r = co_await try_publish(channel, payload);
            if (!r) throw std::move(r.error());
            co_return *r;
        }

    private:
        // Two links, because a waiter can sit in two queues at once: the
        // head stays in replies_ while it reads, and a baton pass from
        // inside a drain loop parks that same head on ready_. One link
        // shared by both would sever the FIFO behind it.
        struct waiter final {
            waiter* next{};
            waiter* ready_next{};
            std::coroutine_handle<> h{};
            std::optional<detail::clock::time_point> deadline{};
            std::optional<error> failure{};
            bool closed = false;
        };

        // Intrusive FIFO over the waiter nodes, which live in the parked
        // coroutines' frames: nothing is allocated to wait. Link selects
        // which pointer threads the queue.
        template <waiter* waiter::*Link>
        struct waiter_queue final {
            waiter* head{};
            waiter* tail{};
            std::size_t size = 0;

            [[nodiscard]] bool empty() const noexcept {
                return head == nullptr;
            }

            void push(waiter* const w) noexcept {
                w->*Link = nullptr;
                if (tail != nullptr) tail->*Link = w;
                else head = w;
                tail = w;
                ++size;
            }

            [[nodiscard]] waiter* pop() noexcept {
                waiter* const w = head;
                if (w == nullptr) return nullptr;
                head = w->*Link;
                if (head == nullptr) tail = nullptr;
                w->*Link = nullptr;
                --size;
                return w;
            }
        };

        struct park final {
            waiter* w;

            [[nodiscard]] bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(const std::coroutine_handle<> h) const noexcept {
                w->h = h;
            }

            void await_resume() const noexcept {
            }
        };

        [[nodiscard]] coro::task<std::expected<void, error>> ensure() const {
            for (;;) {
                if (closed_) co_return std::unexpected(closed_error());
                if (conn_ != nullptr) {
                    if (conn_->ok()) co_return std::expected<void, error>{};
                    // Broken with commands in flight: the head drops it when
                    // its wait ends. Nothing here may close an fd someone is
                    // parked on.
                    if (!replies_.empty()) co_return std::unexpected(error{error_kind::connection, "connection is broken"});
                    conn_.reset();
                }
                if (opening_) {
                    waiter w{};
                    connects_.push(&w);
                    co_await park{&w};
                    if (w.closed) co_return std::unexpected(closed_error());
                    continue;
                }
                opening_ = true;
                auto opened = co_await connection::open(cfg_, io_);
                opening_ = false;
                if (closed_) {
                    wake_connects();
                    co_return std::unexpected(closed_error());
                }
                if (!opened) {
                    // The first woken waiter becomes the next opener.
                    wake_connects();
                    co_return std::unexpected(std::move(opened.error()));
                }
                conn_ = std::move(*opened);
                wake_connects();
                co_return std::expected<void, error>{};
            }
        }

        void wake_connects() const noexcept {
            while (waiter* const w = connects_.pop()) resume(w);
        }

        // Flush whatever append() left, then one reply; pushes are skipped.
        [[nodiscard]] coro::task<std::expected<reply, error>> read_head(const waiter& w) const {
            for (;;) {
                if (auto f = co_await conn_->flush(detail::remaining(w.deadline)); !f) co_return std::unexpected(std::move(f.error()));
                auto r = co_await conn_->read(detail::remaining(w.deadline));
                if (r && r->type() == reply_type::push) continue;
                co_return std::move(r);
            }
        }

        // The connection is gone before anyone is resumed, so a waiter that
        // retries finds a client it can reopen on.
        [[nodiscard]] error fail_rest(error e) const {
            conn_.reset();
            const std::string dropped = std::string{"connection dropped: "} + e.what();
            while (waiter* const w = replies_.pop()) {
                w->failure = error{error_kind::connection, dropped};
                resume(w);
            }
            return e;
        }

        void pass_baton() const noexcept {
            if (waiter* const next = replies_.head) resume(next);
            else if (closed_) conn_.reset();
        }

        [[nodiscard]] static error closed_error() {
            return error{error_kind::closed, "client is closed"};
        }

        // The drain loop. A resumption that happens while one is already
        // running -- a resumed waiter passing the baton before it suspends
        // -- is queued and run by the outer call, so the depth stays one
        // whatever the chain length.
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

        config cfg_;
        coro::reactor_ref io_;
        mutable std::unique_ptr<connection> conn_;
        mutable waiter_queue<&waiter::next> connects_;
        mutable waiter_queue<&waiter::next> replies_;
        mutable waiter_queue<&waiter::ready_next> ready_;
        mutable bool opening_ = false;
        mutable bool closed_ = false;
        mutable bool draining_ = false;
    };
}
