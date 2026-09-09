#pragma once

// scheduler_ref: the non-owning handle that lets the sqlite driver hop back
// onto whatever scheduler its pool was built on -- a context pointer plus a
// function pointer, filled in by a template constructor from any
// coro::postable. This is the one indirection the library allows itself: a
// function-pointer call at the hop boundary, in exchange for keeping
// sql::pool<sql::sqlite> the spelling in every handler and keeping sql free
// of h2o. The handle owns nothing; the scheduler must outlive every pool
// built on it. A default-constructed handle is null: schedule() is ready at
// once and post() resumes inline, because "here" is the only place it has.
//
// reactor_ref lives in coro/io/reactor_ref.h now, so a driver outside sql
// (redis) can wait on a descriptor without depending on sql; the alias keeps
// every existing spelling.

#include <coroutine>

#include <coro/concepts/scheduler.h>
#include <coro/io/reactor_ref.h>

namespace sql {
    using reactor_ref = coro::reactor_ref;

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
