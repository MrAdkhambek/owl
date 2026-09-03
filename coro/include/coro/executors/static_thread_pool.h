#pragma once

// A fixed-size pool of worker threads. Coroutines posted here run on
// those threads: schedule() is the awaitable way in, post() the raw
// continuation delivery. Models scheduler and postable.
//
// One queue per worker, filled round-robin -- cheap and contention-
// free, but with no work stealing, so a skewed pattern can leave one
// worker busy while its neighbours nap.

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "coro/concepts/scheduler.h"

namespace coro {
    class static_thread_pool {
    public:
        // Spawns thread_count workers (at least one, even if asked for
        // zero). The pool is never idle-shut: workers live until the
        // destructor.
        explicit static_thread_pool(const std::size_t thread_count = std::thread::hardware_concurrency()) {
            const std::size_t count = thread_count == 0 ? 1 : thread_count;

            slots_.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                slots_.emplace_back(std::make_unique<slot>());
            }
            threads_.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                threads_.emplace_back([this, i] {
#if defined(__APPLE__)
                    char name[16];
                    std::snprintf(name, sizeof(name), "worker-%zu", i);
                    pthread_setname_np(name);
#endif
                    worker(i);
                });
            }
        }

        static_thread_pool(const static_thread_pool&) = delete;
        static_thread_pool& operator=(const static_thread_pool&) = delete;
        static_thread_pool(static_thread_pool&&) = delete;
        static_thread_pool& operator=(static_thread_pool&&) = delete;

        // Stops the workers and joins them. Work already queued is
        // still drained by the workers on their way out -- and then, in
        // debug builds, flagged: destroying a pool with outstanding
        // continuations is a bug (the coroutines that posted them
        // cannot know when they ran), so it asserts.
        ~static_thread_pool() {
            for (const auto& slot : slots_) {
                {
                    const std::lock_guard lock(slot->mutex);
                    slot->stopping = true;
                }
                slot->wakeup.notify_all();
            }
            for (auto& t : threads_) t.join();

#ifndef NDEBUG
            for (const auto& slot : slots_) {
                assert(!slot->drained_at_shutdown && "static_thread_pool destroyed with work still queued");
            }
#endif
        }

        // The scheduler contract: always suspends, and the posting
        // worker resumes the coroutine -- so co_await pool.schedule()
        // is a genuine hop onto a pool thread, never an inline resume.
        auto schedule() noexcept {
            struct awaiter {
                static_thread_pool* pool;

                bool await_ready() const noexcept {
                    return false;
                }

                void await_suspend(std::coroutine_handle<> continuation) {
                    pool->post(continuation);
                }

                void await_resume() const noexcept {
                }
            };
            return awaiter{this};
        }

        // Delivers a continuation round-robin to the next worker's
        // queue. postable contract: callable from ordinary code, which
        // is how other threads (timers, the reactor) hand work back.
        void post(const std::coroutine_handle<> continuation) {
            slot& target = *slots_[next_.fetch_add(1) % slots_.size()];
            {
                const std::lock_guard lock(target.mutex);
                target.queue.push_back(continuation);
            }
            target.wakeup.notify_one();
        }

    private:
        // Per-worker state. Separate mutex/queue per slot is what keeps
        // posting cheap; nothing is shared across workers.
        struct slot {
            std::mutex mutex;
            std::condition_variable wakeup;
            std::deque<std::coroutine_handle<>> queue;
            bool stopping = false;

            bool drained_at_shutdown = false;
        };

        // Pop-and-run forever: continuations are resumed inline, so a
        // coroutine scheduled here *runs* on this worker thread until
        // it suspends.
        void worker(const std::size_t index) const {
            slot& mine = *slots_[index];
            for (;;) {
                std::coroutine_handle<> continuation;
                {
                    std::unique_lock lock(mine.mutex);
                    mine.wakeup.wait(lock, [&mine] {
                        return mine.stopping || !mine.queue.empty();
                    });
                    if (mine.stopping && mine.queue.empty()) return;

                    if (mine.stopping) mine.drained_at_shutdown = true;
                    continuation = mine.queue.front();
                    mine.queue.pop_front();
                }

                continuation.resume();
            }
        }

        std::vector<std::unique_ptr<slot>> slots_;
        std::atomic<std::size_t> next_{0};
        std::vector<std::thread> threads_;
    };

    static_assert(postable<static_thread_pool>);
}
