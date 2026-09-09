#pragma once

// A non-owning handle to any io_reactor: a context pointer plus function
// pointers, filled in by a template constructor from any coro::io_reactor.
// It exists so that an un-templated driver -- sql's postgres connection,
// redis's connection -- can wait on a descriptor through whatever reactor
// it was built on while its own spelling stays free of the reactor type:
// one function-pointer call at the wait boundary, next to a syscall. The
// handle owns nothing; the reactor must outlive every user. A
// default-constructed handle is null: wait answers error without parking
// and release is a no-op.
//
// cancel(fd) ends a wait parked on a descriptor, resuming that waiter with
// wait_status::cancelled and leaving the descriptor alone -- the way to
// stop waiting for something that may never arrive.
//
// release(fd) tells the reactor to stop watching a descriptor and to drop
// whatever state it kept for it. The caller's descriptor is not touched, so
// it does not matter whether the caller has already closed it. A reactor
// that keeps no per-fd state (native_reactor) has no release; the thunk is
// then a no-op, so drivers call it unconditionally.

#include <chrono>

#include "coro/concepts/io_reactor.h"
#include "coro/task.h"

namespace coro {
    class reactor_ref final {
    public:
        reactor_ref() = default;

        template <io_reactor R>
        explicit reactor_ref(R& reactor) noexcept
            : ctx_(&reactor), wait_(&wait_thunk<R>), release_(&release_thunk<R>), cancel_(&cancel_thunk<R>) {
        }

        [[nodiscard]] task<wait_status>
        wait(const int fd, const interest want, const std::chrono::milliseconds timeout) const {
            if (ctx_ == nullptr) return null_wait();
            return wait_(ctx_, fd, want, timeout);
        }

        void release(const int fd) const {
            if (ctx_ != nullptr) release_(ctx_, fd);
        }

        // Ends a wait parked on the descriptor: that waiter resumes with
        // wait_status::cancelled, and the descriptor is left alone. A
        // reactor that cannot do it gets a no-op thunk, as with release.
        void cancel(const int fd) const {
            if (ctx_ != nullptr) cancel_(ctx_, fd);
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return ctx_ != nullptr;
        }

    private:
        using wait_fn = task<wait_status> (*)(void*, int, interest, std::chrono::milliseconds);
        using release_fn = void (*)(void*, int);

        static task<wait_status> null_wait() {
            co_return wait_status::error;
        }

        template <typename R>
        static task<wait_status>
        wait_thunk(void* const ctx, const int fd, const interest want, const std::chrono::milliseconds timeout) {
            return static_cast<R*>(ctx)->wait(fd, want, timeout);
        }

        template <typename R>
        static void release_thunk(void* const ctx, const int fd) {
            if constexpr (requires(R& r, const int f) { r.release(f); }) {
                static_cast<R*>(ctx)->release(fd);
            }
        }

        template <typename R>
        static void cancel_thunk(void* const ctx, const int fd) {
            if constexpr (requires(R& r, const int f) { r.cancel(f); }) {
                static_cast<R*>(ctx)->cancel(fd);
            }
        }

        void* ctx_{};
        wait_fn wait_{};
        release_fn release_{};
        release_fn cancel_{};
    };
}
