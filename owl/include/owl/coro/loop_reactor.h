#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <unordered_map>

#include <coro/concepts/io_reactor.h>
#include <coro/task.h>

#include <h2o.h>

namespace owl {
    // coro::io_reactor over an explicit h2o loop: the psql driver's wait
    // mechanism under owl. One h2o_socket per fd, created once and kept for
    // the connection's life -- h2o's evloop socket owns the fd it wraps, so
    // a naive re-wrap would fight PQfinish for the close; release(fd)
    // exports the socket so the fd survives, and is the only legal way to
    // hand the fd back. Read is level-triggered on the evloop backend, so
    // the read callback stops reading before resuming, making each wait
    // one-shot like native_reactor's; notify_write is one-shot by itself.
    // A default-constructed reactor is the null reactor: wait/sleep answer
    // error, no parking. The fd map is worker-thread-only state (the
    // reactor and every connection live on one loop): no locks.
    class loop_reactor final {
    public:
        loop_reactor() = default;

        explicit loop_reactor(h2o_loop_t* const loop) noexcept : loop_(loop) {
        }

        // Movable for on_context_init's `context->reactor = ...` wiring;
        // nothing is armed at that point, so transferring the map is safe.
        loop_reactor(loop_reactor&& o) noexcept : loop_(o.loop_), sockets_(std::move(o.sockets_)) {
            o.loop_ = nullptr;
        }

        loop_reactor& operator=(loop_reactor&& o) noexcept {
            loop_ = o.loop_;
            sockets_ = std::move(o.sockets_);
            o.loop_ = nullptr;
            return *this;
        }

        loop_reactor(const loop_reactor&) = delete;
        loop_reactor& operator=(const loop_reactor&) = delete;

        [[nodiscard]] coro::task<coro::wait_status>
        wait(const int fd, const coro::interest want, const std::chrono::milliseconds timeout) const {
            if (loop_ == nullptr || fd < 0) co_return coro::wait_status::error;
            co_return co_await wait_awaiter{.self = this, .fd = fd, .want = want, .timeout = timeout};
        }

        [[nodiscard]] coro::task<coro::wait_status> sleep(const std::chrono::milliseconds timeout) const {
            if (loop_ == nullptr) co_return coro::wait_status::error;
            if (timeout.count() <= 0) co_return coro::wait_status::ready;
            co_return co_await sleep_awaiter{.self = this, .timeout = timeout};
        }

        // Hands the fd back before PQfinish closes it: export detaches the
        // socket from the loop with the fd surviving in the export info.
        // h2o_socket_dispose_export is deliberately NOT called -- it closes
        // info.fd (verified empirically), which is the very fd PQfinish
        // must close. The exported input buffer is empty on every driver
        // path (statements are fully drained before release), so skipping
        // dispose costs nothing on this rare path. Only legal with no wait
        // armed on the fd.
        void release(const int fd) const {
            const auto it = sockets_.find(fd);
            if (it == sockets_.end()) return;
            h2o_socket_export_t info{};
            h2o_socket_export(it->second.sock, &info);
            (void)info;
            sockets_.erase(it);
        }

    private:
        struct pending;

        struct entry final {
            h2o_socket_t* sock{};
            pending* req{};
        };

        // The one wait's state, parked in the awaiting coroutine's frame.
        // The h2o_timer_t must be first: the timer callback recovers the
        // request from its own pointer, the way LoopTimerMsg does. The
        // socket callback recovers it through sock->data.
        struct pending final {
            h2o_timer_t timer{};
            h2o_socket_t* sock{};
            entry* owner{};
            std::coroutine_handle<> cont{};
            coro::wait_status status = coro::wait_status::error;
        };

        static pending* pending_of(h2o_timer_t* const t) {
            return reinterpret_cast<pending*>(t);
        }

        // One-shot completion: stop everything the request armed, then
        // resume. Safe to reach from the socket callback, the timer
        // callback, and the awaiter's destructor path.
        static void finish(pending* const req, const coro::wait_status status) {
            if (req->owner != nullptr) req->owner->req = nullptr;
            if (req->sock != nullptr) {
                req->sock->data = nullptr;
                h2o_socket_read_stop(req->sock);
            }
            if (h2o_timer_is_linked(&req->timer)) h2o_timer_unlink(&req->timer);
            req->status = status;
            const auto cont = std::exchange(req->cont, nullptr);
            cont.resume();
        }

        static void on_ready(h2o_socket_t* const sock, const char* const err) {
            auto* const req = static_cast<pending*>(sock->data);
            if (req == nullptr) return;
            finish(req, err != nullptr ? coro::wait_status::error : coro::wait_status::ready);
        }

        static void on_timer(h2o_timer_t* const t) {
            finish(pending_of(t), coro::wait_status::timeout);
        }

        struct wait_awaiter final {
            const loop_reactor* self;
            int fd;
            coro::interest want;
            std::chrono::milliseconds timeout;

            pending req{};

            bool await_ready() const noexcept {
                return false;
            }

            bool await_suspend(const std::coroutine_handle<> h) {
                entry& e = self->sockets_[fd];
                if (e.req != nullptr) {
                    // One wait per fd at a time -- a second overlapping
                    // wait is a programming error; fail it instead of
                    // clobbering.
                    req.status = coro::wait_status::error;
                    return false;
                }
                if (e.sock == nullptr) {
                    e.sock = h2o_evloop_socket_create(self->loop_, fd, H2O_SOCKET_FLAG_DONT_READ);
                }
                req.sock = e.sock;
                req.owner = &e;
                req.cont = h;
                e.req = &req;
                e.sock->data = &req;
                h2o_timer_init(&req.timer, &loop_reactor::on_timer);
                if (timeout.count() >= 0) {
                    h2o_timer_link(self->loop_, static_cast<std::uint64_t>(timeout.count()), &req.timer);
                }
                if (want != coro::interest::write) h2o_socket_read_start(e.sock, &loop_reactor::on_ready);
                if (want != coro::interest::read) h2o_socket_notify_write(e.sock, &loop_reactor::on_ready);
                return true;
            }

            [[nodiscard]] coro::wait_status await_resume() const noexcept {
                return req.status;
            }

            // Insurance: if the awaiting coroutine is destroyed while
            // parked (forbidden in v1, but a destroyed coroutine must not
            // leave the loop reaching into dead memory), disarm everything.
            ~wait_awaiter() {
                if (req.cont == nullptr) return;
                if (req.owner != nullptr && req.owner->req == &req) req.owner->req = nullptr;
                if (req.sock != nullptr) {
                    if (req.sock->data == &req) req.sock->data = nullptr;
                    h2o_socket_read_stop(req.sock);
                }
                if (h2o_timer_is_linked(&req.timer)) h2o_timer_unlink(&req.timer);
            }
        };

        struct sleep_awaiter final {
            const loop_reactor* self;
            std::chrono::milliseconds timeout;

            pending req{};

            bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(const std::coroutine_handle<> h) {
                req.cont = h;
                h2o_timer_init(&req.timer, &loop_reactor::on_timer);
                h2o_timer_link(self->loop_, static_cast<std::uint64_t>(timeout.count()), &req.timer);
            }

            [[nodiscard]] coro::wait_status await_resume() const noexcept {
                return req.status;
            }

            ~sleep_awaiter() {
                if (req.cont != nullptr && h2o_timer_is_linked(&req.timer)) h2o_timer_unlink(&req.timer);
            }
        };

        h2o_loop_t* loop_{};
        mutable std::unordered_map<int, entry> sockets_;
    };

    static_assert(coro::io_reactor<loop_reactor>);
}
