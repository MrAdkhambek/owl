#pragma once

// One hiredis context on a non-blocking fd, driven step by step from
// coroutines: append to hiredis's output buffer, write-wait until it is
// flushed, read-wait until the reader holds a whole reply. This is libpq's
// shape in sql::psql one to one, and for the same reason: hiredis's own
// blocking and async APIs both want to own the wait, and here the reactor
// owns it.
//
// open() is the only way in, and it finishes the handshake before handing
// the connection out: the TCP connect (completed by hand -- writable, then
// SO_ERROR -- because redisCheckConnectDone is not an installed symbol),
// HELLO 3 with AUTH when there is a password, SELECT when db is not 0. So
// every connection that exists speaks RESP3 and is authenticated.
//
// append() also writes whatever the socket takes right now, without
// waiting. That is deliberate: a command is on the wire before its caller
// suspends, even while someone else is parked reading an earlier reply,
// which is what keeps concurrent callers pipelined instead of serialized
// on round trips. flush() finishes what append() could not.
//
// Failure is data. A wait that runs out finishes the connection and reports
// timeout, because the reply stream is ambiguous afterwards; a socket or
// protocol error marks it broken and reports connection; a top-level error
// reply is error{command} and the connection is kept. The fd is released to
// the reactor before hiredis closes it, always: a reactor keys whatever it
// keeps by descriptor number, and the number must be out of its map before
// hiredis frees it for the next connection to take.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/socket.h>

#include <hiredis/hiredis.h>

#include <coro/concepts/io_reactor.h>
#include <coro/io/deadline.h>
#include <coro/io/reactor_ref.h>
#include <coro/task.h>

#include "redis/args.h"
#include "redis/config.h"
#include "redis/error.h"
#include "redis/reply.h"

namespace redis {
    class connection final {
    public:
        static coro::task<std::expected<std::unique_ptr<connection>, error>>
        open(const config& cfg, const coro::reactor_ref io, std::stop_token stop = {}) {
            redisOptions opts{};
            REDIS_OPTIONS_SET_TCP(&opts, cfg.host.c_str(), cfg.port);
            opts.options |= REDIS_OPT_NONBLOCK | REDIS_OPT_PREFER_IPV4;
            redisContext* const raw = redisConnectWithOptions(&opts);
            if (raw == nullptr) co_return std::unexpected(error{error_kind::connect, "redisConnectWithOptions: out of memory"});
            auto c = std::unique_ptr<connection>(new connection{raw, io});
            if (raw->err != 0) co_return std::unexpected(error{error_kind::connect, raw->errstr});
            // A stop asked for while the connect or the handshake is parked
            // ends that wait the way it ends any other on this connection.
            const std::stop_callback wake{stop, [conn = c.get()] {
                conn->cancel_wait();
            }};

            const coro::deadline deadline{cfg.connect_timeout};
            // The non-blocking connect is in flight: writable means it ended,
            // one way or the other, and SO_ERROR says which.
            if (auto stopped = c->cancelled()) co_return std::unexpected(std::move(*stopped));
            const auto st = co_await io.wait(c->fd_, coro::interest::write, deadline.remaining());
            if (auto stopped = c->cancelled()) co_return std::unexpected(std::move(*stopped));
            if (st == coro::wait_status::timeout) co_return std::unexpected(error{error_kind::timeout, "connect timed out"});
            if (st != coro::wait_status::ready) co_return std::unexpected(error{error_kind::connect, "reactor failed while connecting"});
            int so_error = 0;
            socklen_t len = sizeof so_error;
            if (::getsockopt(c->fd_, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) so_error = errno;
            if (so_error != 0) {
                co_return std::unexpected(error{error_kind::connect, std::string{"connect: "} + std::strerror(so_error)});
            }

            if (auto hs = co_await c->handshake(cfg, deadline); !hs) co_return std::unexpected(std::move(hs.error()));
            co_return std::move(c);
        }

        ~connection() {
            finish();
        }

        connection(const connection&) = delete;
        connection& operator=(const connection&) = delete;

        [[nodiscard]] bool ok() const noexcept {
            return ok_;
        }

        // -1 once finished.
        [[nodiscard]] int fd() const noexcept {
            return fd_;
        }

        // Ends whatever wait this connection is parked in, and refuses
        // any it has not started yet. The flag is what makes the second
        // half true: a reactor cancel for a descriptor with nothing parked
        // on it has nothing to do and is dropped, so a stop that lands in
        // the window between deciding to read and parking would otherwise
        // be lost and the read would wait forever. It is never cleared --
        // a cancelled connection is being abandoned, not reused -- and it
        // is atomic because the asking thread need not be this one.
        void cancel_wait() const noexcept {
            cancelled_.store(true, std::memory_order_release);
            if (fd_ >= 0) io_.cancel(fd_);
        }

        // Queues one command and writes what the socket takes now. false
        // means hiredis refused or the socket failed; the connection is then
        // broken and the caller reports connection.
        [[nodiscard]] bool append(const std::span<const std::string_view> argv) {
            if (!ok_ || conn_ == nullptr) return false;
            // Unzipped into members rather than locals: hiredis wants two
            // parallel C arrays, every command needs the same two, and a
            // connection runs one command at a time. Clearing keeps the
            // capacity, so only the first command on a connection allocates.
            ptrs_.clear();
            lens_.clear();
            ptrs_.reserve(argv.size());
            lens_.reserve(argv.size());
            for (const std::string_view a : argv) {
                ptrs_.push_back(a.data());
                lens_.push_back(a.size());
            }
            if (redisAppendCommandArgv(conn_, static_cast<int>(argv.size()), ptrs_.data(), lens_.data()) != REDIS_OK) {
                ok_ = false;
                return false;
            }
            int done = 0;
            if (redisBufferWrite(conn_, &done) != REDIS_OK) {
                ok_ = false;
                return false;
            }
            return true;
        }

        // Write-waits until hiredis's output buffer is empty.
        [[nodiscard]] coro::task<std::expected<void, error>> flush(const std::chrono::milliseconds budget) {
            if (!ok_) co_return std::unexpected(error{error_kind::connection, "connection is broken"});
            const coro::deadline deadline{budget};
            for (;;) {
                if (auto stopped = cancelled()) co_return std::unexpected(std::move(*stopped));
                int done = 0;
                if (redisBufferWrite(conn_, &done) != REDIS_OK) co_return std::unexpected(broken());
                if (done != 0) co_return std::expected<void, error>{};
                if (auto failed = after_wait(co_await io_.wait(fd_, coro::interest::write, deadline.remaining()))) {
                    co_return std::unexpected(std::move(*failed));
                }
            }
        }

        // One reply, push replies included as-is; read-waits as needed. A
        // top-level error reply comes back as error{command}.
        [[nodiscard]] coro::task<std::expected<reply, error>> read(const std::chrono::milliseconds budget) {
            if (!ok_) co_return std::unexpected(error{error_kind::connection, "connection is broken"});
            const coro::deadline deadline{budget};
            for (;;) {
                if (auto stopped = cancelled()) co_return std::unexpected(std::move(*stopped));
                void* raw = nullptr;
                if (redisGetReplyFromReader(conn_, &raw) != REDIS_OK) co_return std::unexpected(broken());
                if (raw != nullptr) {
                    auto* const node = static_cast<redisReply*>(raw);
                    reply r{node};
                    if (node->type == REDIS_REPLY_ERROR) {
                        co_return std::unexpected(error{error_kind::command, std::string{node->str, node->len}});
                    }
                    co_return std::move(r);
                }
                if (auto failed = after_wait(co_await io_.wait(fd_, coro::interest::read, deadline.remaining()))) {
                    co_return std::unexpected(std::move(*failed));
                }
                if (redisBufferRead(conn_) != REDIS_OK) co_return std::unexpected(broken());
            }
        }

    private:
        connection(redisContext* const conn, const coro::reactor_ref io) noexcept : conn_(conn), io_(io), fd_(conn->fd) {
        }

        [[nodiscard]] coro::task<std::expected<void, error>>
        handshake(const config& cfg, const coro::deadline deadline) {
            std::vector<std::string_view> hello{"HELLO", "3"};
            if (!cfg.password.empty()) {
                hello.push_back("AUTH");
                hello.push_back(cfg.username.empty() ? std::string_view{"default"} : std::string_view{cfg.username});
                hello.push_back(cfg.password);
            }
            if (auto r = co_await exchange(hello, deadline); !r) co_return std::unexpected(as_connect(std::move(r.error()), true));
            if (cfg.db != 0) {
                const detail::command_args select{"SELECT", cfg.db};
                if (auto r = co_await exchange(select.argv(), deadline); !r) {
                    co_return std::unexpected(as_connect(std::move(r.error()), false));
                }
            }
            co_return std::expected<void, error>{};
        }

        // One command, one reply, on a connection nobody else is using yet.
        [[nodiscard]] coro::task<std::expected<reply, error>>
        exchange(const std::span<const std::string_view> argv, const coro::deadline deadline) {
            if (!append(argv)) co_return std::unexpected(broken());
            if (auto f = co_await flush(deadline.remaining()); !f) co_return std::unexpected(std::move(f.error()));
            co_return co_await read(deadline.remaining());
        }

        // A handshake failure is a connect failure whatever step said it,
        // except a timeout, which stays one. A pre-6 server answers HELLO
        // with an unknown-command error; name the real problem.
        [[nodiscard]] static error as_connect(error e, const bool hello) {
            if (e.kind() == error_kind::timeout) return e;
            const std::string_view msg = e.what();
            if (hello && e.kind() == error_kind::command && msg.starts_with("ERR unknown command")) {
                return error{error_kind::connect, "server does not speak RESP3 (HELLO): Redis 6 or newer is required"};
            }
            return error{error_kind::connect, std::string{msg}};
        }

        // What one readiness wait came to. nullopt means ready; otherwise
        // the error to report, with the connection already finished
        // (timeout) or marked broken (anything else).
        [[nodiscard]] std::optional<error> after_wait(const coro::wait_status st) {
            if (st == coro::wait_status::ready) return cancelled();
            if (auto stopped = cancelled()) return stopped;
            if (st == coro::wait_status::timeout) {
                finish();
                return error{error_kind::timeout, "command timed out"};
            }
            // Cancelled or failed, the reply stream is now at an unknown
            // offset either way, so the connection does not go back in use.
            ok_ = false;
            if (st == coro::wait_status::cancelled) return error{error_kind::connection, "wait cancelled"};
            return error{error_kind::connection, "reactor failed while waiting on the connection"};
        }

        // Asked to stop, whether or not a wait was parked to be cancelled.
        [[nodiscard]] std::optional<error> cancelled() noexcept {
            if (!cancelled_.load(std::memory_order_acquire)) return std::nullopt;
            ok_ = false;
            return error{error_kind::connection, "wait cancelled"};
        }

        [[nodiscard]] error broken() noexcept {
            ok_ = false;
            return error{error_kind::connection, conn_ != nullptr && conn_->err != 0 ? conn_->errstr : "connection is broken"};
        }

        // Drop the reactor's wrapper first, then let hiredis close the fd,
        // then null everything so a second call -- the destructor after a
        // timeout -- is a no-op. The reactor closes only its own copy of the
        // descriptor, so this order is about keeping the number out of its
        // map, not about who closes what.
        void finish() noexcept {
            if (conn_ == nullptr) return;
            if (fd_ >= 0) io_.release(fd_);
            redisFree(conn_);
            conn_ = nullptr;
            fd_ = -1;
            ok_ = false;
        }

        mutable std::atomic<bool> cancelled_{false};
        std::vector<const char*> ptrs_;
        std::vector<std::size_t> lens_;
        redisContext* conn_;
        coro::reactor_ref io_;
        int fd_;
        bool ok_ = true;
    };
}
