#pragma once

// sync_wait -- the bridge from synchronous code into coroutine-land.
// It starts a task's body *eagerly on this thread* (the one place the
// library's usual laziness is overridden), then blocks the thread on a
// condition variable until the task finishes, then returns the value
// or rethrows what the body threw. Blocking is required, not a
// shortcut: the task may suspend and be resumed by other threads (a
// pool, a timer), and this thread has nothing better to do than sleep
// until it is done.

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <type_traits>
#include <utility>

#include "coro/task.h"

namespace coro {
    namespace detail {
        // The blocking half: a boolean behind a mutex/condvar pair.
        struct sync_event {
            std::mutex mutex;
            std::condition_variable signal;
            bool completed = false;

            // The notify happens WITH the lock held, against the usual
            // advice. The waiter is free to return the instant it sees
            // `completed` -- it need never be woken -- and once it has
            // returned, sync_wait destroys this event. Signalling after the
            // unlock would let that destruction race a notify_all still in
            // flight on a condition variable that no longer exists.
            // Holding the lock keeps the waiter out until the signal is
            // fully sent; the wake-then-block-on-mutex cost that the advice
            // avoids is nothing next to a use-after-free.
            void mark_done() {
                const std::lock_guard lock(mutex);
                completed = true;
                signal.notify_all();
            }

            void wait_until_done() {
                std::unique_lock lock(mutex);
                signal.wait(lock, [this] {
                    return completed;
                });
            }
        };

        // The driver coroutine: it co_awaits the user's task inline, so
        // the task runs on whatever thread starts the driver, and its
        // promise remembers the event to fire when the body finishes.
        template <typename T>
        class sync_wait_task {
        public:
            struct promise_type : detail::returns<T> {
                // The task<T>& parameter is how the promise sees the
                // coroutine's arguments; only the event is kept.
                promise_type(sync_event& ev, task<T>&) noexcept : event(&ev) {
                }

                sync_event* event = nullptr;

                sync_wait_task get_return_object() noexcept {
                    return sync_wait_task{
                        std::coroutine_handle<promise_type>::from_promise(*this)
                    };
                }

                // Eager: the body runs as soon as the caller starts it.
                std::suspend_never initial_suspend() const noexcept {
                    return {};
                }

                void unhandled_exception() noexcept {
                    this->set_exception(std::current_exception());
                }

                // Fires the event instead of resuming anyone: nobody is
                // waiting in coroutine-land -- the waiting is done by a
                // blocked thread.
                struct done_awaiter {
                    bool await_ready() const noexcept {
                        return false;
                    }

                    void await_suspend(std::coroutine_handle<promise_type> me) const noexcept {
                        me.promise().event->mark_done();
                    }

                    void await_resume() const noexcept {
                    }
                };

                done_awaiter final_suspend() noexcept {
                    return {};
                }
            };

            using handle_type = std::coroutine_handle<promise_type>;

            explicit sync_wait_task(handle_type handle) noexcept : handle_(handle) {
            }

            sync_wait_task(sync_wait_task&& o) noexcept : handle_(std::exchange(o.handle_, {})) {
            }

            sync_wait_task& operator=(sync_wait_task&& o) noexcept {
                if (this != &o) {
                    if (handle_) handle_.destroy();
                    handle_ = std::exchange(o.handle_, {});
                }
                return *this;
            }

            sync_wait_task(const sync_wait_task&) = delete;
            sync_wait_task& operator=(const sync_wait_task&) = delete;

            ~sync_wait_task() {
                if (handle_) handle_.destroy();
            }

            // Blocks until the event fires, then tears the frame down
            // (it is parked at final_suspend, so destroy is the only
            // way out) and finally produces the value or rethrows. The
            // releaser keeps that teardown happening even when take()
            // throws.
            T join() {
                handle_.promise().event->wait_until_done();
                struct releaser {
                    handle_type& handle;

                    ~releaser() {
                        handle.destroy();
                        handle = nullptr;
                    }
                } releaser{handle_};
                return handle_.promise().take();
            }

        private:
            handle_type handle_;
        };

        // Exists so T can be deduced from the task while the promise
        // observes the event. The driver's frame is destroyed by
        // sync_wait_task, never here.
        template <typename T>
        sync_wait_task<T> sync_drive([[maybe_unused]] sync_event& event, task<T> t) {
            if constexpr (std::is_void_v<T>) {
                co_await std::move(t);
            } else {
                co_return co_await std::move(t);
            }
        }
    }

    template <typename T>
    [[nodiscard]] T sync_wait(task<T> t) {
        detail::sync_event event;
        auto driver = detail::sync_drive(event, std::move(t));
        return driver.join();
    }
}
