#pragma once

// Two non-owning handles that let an un-templated driver reach whatever
// reactor or scheduler its pool was built on: a context pointer plus
// function pointers, filled in by a template constructor from any
// coro::io_reactor or coro::postable. This is the one indirection the
// library allows itself (spec requirement 4): a function-pointer call at
// the wait / hop boundary, next to a syscall, in exchange for keeping
// sql::pool<sql::psql> the spelling in every handler and keeping sql free
// of h2o. Neither handle owns anything; the reactor or scheduler must
// outlive every pool built on it. A default-constructed handle is null:
// wait answers error without parking, schedule() is ready at once, and
// post() resumes inline because "here" is the only place it has.

#include <chrono>
#include <coroutine>

#include <coro/concepts/io_reactor.h>
#include <coro/concepts/scheduler.h>
#include <coro/task.h>

namespace sql {
    class reactor_ref final {
    public:
        reactor_ref() = default;

        template <coro::io_reactor R>
        explicit reactor_ref(R& reactor) noexcept
            : ctx_(&reactor), wait_(&wait_thunk<R>), release_(&release_thunk<R>) {
        }

        [[nodiscard]] coro::task<coro::wait_status>
        wait(const int fd, const coro::interest want, const std::chrono::milliseconds timeout) const {
            if (ctx_ == nullptr) return null_wait();
            return wait_(ctx_, fd, want, timeout);
        }

        // Hands a descriptor back before its owner closes it. A reactor
        // that keeps no per-fd state (native_reactor) has no release; the
        // thunk is then a no-op, so drivers can call this unconditionally.
        void release(const int fd) const {
            if (ctx_ != nullptr) release_(ctx_, fd);
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return ctx_ != nullptr;
        }

    private:
        using wait_fn = coro::task<coro::wait_status> (*)(void*, int, coro::interest, std::chrono::milliseconds);
        using release_fn = void (*)(void*, int);

        static coro::task<coro::wait_status> null_wait() {
            co_return coro::wait_status::error;
        }

        template <typename R>
        static coro::task<coro::wait_status>
        wait_thunk(void* const ctx, const int fd, const coro::interest want, const std::chrono::milliseconds timeout) {
            return static_cast<R*>(ctx)->wait(fd, want, timeout);
        }

        template <typename R>
        static void release_thunk(void* const ctx, const int fd) {
            if constexpr (requires(R& r, const int f) { r.release(f); }) {
                static_cast<R*>(ctx)->release(fd);
            }
        }

        void* ctx_{};
        wait_fn wait_{};
        release_fn release_{};
    };

    class scheduler_ref final {
    public:
        scheduler_ref() = default;

        template <coro::postable S>
        explicit scheduler_ref(S& scheduler) noexcept : ctx_(&scheduler), post_(&post_thunk<S>) {
        }

        void post(const std::coroutine_handle<> h) const {
            if (ctx_ != nullptr) post_(ctx_, h);
            else h.resume();
        }

        [[nodiscard]] auto schedule() const noexcept {
            struct awaiter final {
                scheduler_ref self;

                [[nodiscard]] bool await_ready() const noexcept {
                    return !self;
                }

                void await_suspend(const std::coroutine_handle<> h) const {
                    self.post_(self.ctx_, h);
                }

                void await_resume() const noexcept {
                }
            };
            return awaiter{*this};
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return ctx_ != nullptr;
        }

    private:
        using post_fn = void (*)(void*, std::coroutine_handle<>);

        template <typename S>
        static void post_thunk(void* const ctx, const std::coroutine_handle<> h) {
            static_cast<S*>(ctx)->post(h);
        }

        void* ctx_{};
        post_fn post_{};
    };
}
