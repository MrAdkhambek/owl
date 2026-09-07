#pragma once

// The request worker's event loop as a coro scheduler. Models scheduler,
// delayed_scheduler, and postable. Work posted here always resumes on
// this worker -- the invariant that lets a handler hop off to a pool
// and back without pinning the request to a different thread.
//
//     co_await pool.schedule();            // runs on the pool
//     co_await loop.schedule();            // back on this worker
//     loop.post(handle);                   // same hop, no await
//     co_await loop.schedule_after(20ms);  // timer on this loop
//
// hop_ (the worker's h2o_multithread_receiver_t) is the only
// thread-safe delivery. A 0-delay h2o_timer was rejected as the
// cross-thread path: libh2o forbids linking a timer off the loop
// thread. Without a hop, post() still uses that timer, so it is
// loop-thread only -- fine for tests, not for a pool wakeup.

#include <chrono>
#include <coroutine>

#include <h2o.h>

#include <coro/concepts/delayed_scheduler.h>

#include "owl/extract/from_context.h"
#include "owl/core/token.h"

namespace owl {
    namespace detail {
        struct LoopHopMsg {
            h2o_multithread_message_t super{};
            std::coroutine_handle<> waiting{};
        };

        // timer is first so the h2o callback's timer pointer is the message.
        struct LoopTimerMsg {
            h2o_timer_t timer{};
            std::coroutine_handle<> waiting{};
        };

        // delete-then-resume: the message must be gone before the
        // continuation runs, or a nested post() could see a stale node.
        inline void on_loop_hop(h2o_multithread_receiver_t*, h2o_linklist_t* const messages) {
            while (!h2o_linklist_is_empty(messages)) {
                auto* const msg = reinterpret_cast<LoopHopMsg*>(messages->next);
                h2o_linklist_unlink(&msg->super.link);
                const auto waiting = msg->waiting;
                delete msg;
                if (waiting) waiting.resume();
            }
        }

        inline void on_loop_timer(h2o_timer_t* const t) {
            const auto* const msg = reinterpret_cast<LoopTimerMsg*>(t);
            const auto waiting = msg->waiting;
            delete msg;
            if (waiting) waiting.resume();
        }
    }

    class loop_scheduler final {
    public:
        // TLS so a handler can take loop_scheduler as an extractor
        // without the request carrying the loop. enter/leave bracket
        // the worker's run loop; current() is null off a worker.
        static void enter(loop_scheduler& io) noexcept {
            current_ = &io;
        }

        static void leave() noexcept {
            current_ = nullptr;
        }

        [[nodiscard]] static loop_scheduler* current() noexcept {
            return current_;
        }

        explicit loop_scheduler(h2o_loop_t* const loop, h2o_multithread_receiver_t* const hop = nullptr) noexcept
            : loop_(loop),
              hop_(hop) {
        }

        // postable contract: callable from ordinary code. hop_ if present,
        // else a 0-delay timer (loop thread only). Fire-and-forget -- there
        // is no awaiter to unlink -- which is why schedule_after is separate.
        void post(const std::coroutine_handle<> continuation) const {
            if (hop_ != nullptr) {
                auto* const msg = new detail::LoopHopMsg{.super = {}, .waiting = continuation};
                h2o_multithread_send_message(hop_, &msg->super);
                return;
            }
            auto* const msg = new detail::LoopTimerMsg{.waiting = continuation};
            h2o_timer_init(&msg->timer, &detail::on_loop_timer);
            h2o_timer_link(loop_, 0, &msg->timer);
        }

        // Awaitable wrapper around post(). A null loop never suspends:
        // there is nowhere to go, which is what a dummy scheduler in
        // tests relies on.
        [[nodiscard]] auto schedule() const noexcept {
            struct awaiter {
                loop_scheduler io;

                [[nodiscard]] bool await_ready() const noexcept {
                    return io.loop_ == nullptr;
                }

                void await_suspend(const std::coroutine_handle<> h) const {
                    io.post(h);
                }

                void await_resume() const noexcept {
                }
            };
            return awaiter{*this};
        }

        // Parks on this loop's timer. The awaiter owns the timer so a
        // destroyed coroutine unlinks it; post() cannot. ceil to
        // milliseconds because that is h2o's tick.
        [[nodiscard]] auto
        schedule_after(const std::chrono::steady_clock::duration delay) const noexcept {
            struct awaiter {
                h2o_timer_t timer{};
                h2o_loop_t* loop{};
                std::chrono::milliseconds delay{};
                std::coroutine_handle<> waiting{};

                [[nodiscard]] bool await_ready() const noexcept {
                    return loop == nullptr || delay <= std::chrono::milliseconds::zero();
                }

                void await_suspend(const std::coroutine_handle<> h) {
                    waiting = h;
                    h2o_timer_init(&timer, [](h2o_timer_t* const t) {
                        reinterpret_cast<awaiter*>(t)->waiting.resume();
                    });
                    h2o_timer_link(loop, static_cast<std::uint64_t>(delay.count()), &timer);
                }

                void await_resume() const noexcept {
                }

                ~awaiter() {
                    if (h2o_timer_is_linked(&timer))
                        h2o_timer_unlink(&timer);
                }
            };
            return awaiter{
                .timer = {},
                .loop = loop_,
                .delay = std::chrono::ceil<std::chrono::milliseconds>(delay),
            };
        }

    private:
        inline static thread_local loop_scheduler* current_ = nullptr;

        h2o_loop_t* loop_{};
        h2o_multithread_receiver_t* hop_{};
    };

    static_assert(coro::postable<loop_scheduler>);
    static_assert(coro::scheduler<loop_scheduler>);
    static_assert(coro::delayed_scheduler<loop_scheduler>);

    // Specialized here rather than in from_context.h so that header stays free
    // of the h2o and coro dependencies this one drags in.
    template <>
    struct FromContext<loop_scheduler> {
        template <typename S>
        std::expected<loop_scheduler, KickToken> operator()(const Context<S>&, const Request&) const {
            if (const auto* const io = loop_scheduler::current(); io != nullptr) return *io;
            return KickToken::internal_error("not on an h2o worker");
        }
    };
}
