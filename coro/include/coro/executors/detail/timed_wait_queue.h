#pragma once

// A deadline queue serviced by one dedicated thread. Producers enqueue
// (deadline, continuation) pairs; the thread sleeps until the earliest
// deadline, collects everything due, and delivers each by calling the
// owner's deliver callback. timer_scheduler owns the one instance of
// this class and injects "resume inline" as the callback.
//
// Kept in detail/ because it is policy-free plumbing: it knows nothing
// about coroutines beyond the handle type, and every decision about
// *how* a woken coroutine resumes belongs to the owner.

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace coro::detail {

    class timed_wait_queue {
    public:
        using clock = std::chrono::steady_clock;
        using time_point = clock::time_point;

        // How a due continuation is woken: resume it, post it to a
        // pool -- the owner's choice, made once at construction.
        using deliver_fn = std::function<void(std::coroutine_handle<>)>;

        // Starts the timer thread; the queue is live from here on.
        explicit timed_wait_queue(deliver_fn deliver) : deliver_(std::move(deliver)) {
            thread_ = std::thread([this] {
                run();
            });
        }

        timed_wait_queue(const timed_wait_queue&) = delete;
        timed_wait_queue& operator=(const timed_wait_queue&) = delete;

        // Stops the thread and joins it. Anything still queued at this
        // point is delivered anyway (its sleep ends early) and, in
        // debug builds, asserted about: waiters outliving the queue is
        // a bug, because their sleep silently completing is worse than
        // it not compiling. A waiter woken this way that tries to sleep
        // again is refused by enqueue(), which is what guarantees the
        // join below returns.
        ~timed_wait_queue() {
            {
                const std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            wakeup_.notify_all();
            thread_.join();

#ifndef NDEBUG
            assert(!drained_at_shutdown_ && "timed_wait_queue destroyed with waiters still queued; their sleeps ended early");
#endif
        }

        // Records a continuation to be delivered at deadline. Wakes the
        // timer thread only when this deadline beats the current
        // earliest -- otherwise the sleeper is already set to wake no
        // later than needed.
        //
        // Refused once the queue is shutting down. The shutdown drain
        // wakes every parked continuation early, and one that sleeps
        // again from there -- a periodic loop does exactly that -- would
        // land straight back on the heap and be drained again, at once,
        // forever, with the destructor never returning. Throwing is what
        // ends such a loop: the exception surfaces from the co_await and
        // unwinds the coroutine, and the drain runs dry.
        void enqueue(const time_point deadline, const std::coroutine_handle<> continuation) {
            bool earliest_changed = false;
            {
                const std::lock_guard lock(mutex_);
                if (stopping_) {
                    throw std::logic_error(
                        "timed_wait_queue is shutting down: a continuation it woke early cannot sleep on it again");
                }
                earliest_changed = heap_.empty() || deadline < heap_.top().deadline;
                heap_.push(entry{.deadline = deadline, .continuation = continuation});
            }
            if (earliest_changed) wakeup_.notify_one();
        }

    private:
        struct entry {
            time_point deadline;
            std::coroutine_handle<> continuation;
        };

        // Priority_queue is a max-heap; this comparator inverts it into
        // earliest-deadline-first.
        struct later_first {
            bool operator()(const entry& a, const entry& b) const noexcept {
                return a.deadline > b.deadline;
            }
        };

        // Shutdown path: deliver every queued continuation, whatever
        // its deadline. One pass suffices -- enqueue() refuses anything
        // that arrives while stopping_ is set -- so the heap is empty
        // when this returns. Sets drained_at_shutdown_ as a flag for
        // the destructor's assert.
        void drain_all(std::unique_lock<std::mutex>& lock) {
            if (heap_.empty()) return;
            drained_at_shutdown_ = true;
            std::vector<std::coroutine_handle<>> all;
            all.reserve(heap_.size());
            while (!heap_.empty()) {
                all.push_back(heap_.top().continuation);
                heap_.pop();
            }
            lock.unlock();
            for (const auto continuation : all) deliver_(continuation);
            lock.lock();
        }

        void run() noexcept {

            std::unique_lock lock(mutex_);
            for (;;) {

                while (heap_.empty() && !stopping_) {
                    wakeup_.wait(lock);
                }

                if (stopping_) {
                    drain_all(lock);
                    return;
                }

                const auto next_deadline = heap_.top().deadline;
                wakeup_.wait_until(lock, next_deadline);

                const auto now = clock::now();
                std::vector<std::coroutine_handle<>> due;
                while (!heap_.empty() && heap_.top().deadline <= now) {
                    due.push_back(heap_.top().continuation);
                    heap_.pop();
                }

                if (!due.empty()) {

                    // Delivering may run arbitrary coroutine code, which
                    // may itself enqueue -- so the mutex must not be held
                    // across deliver_. Releasing also stops a slow
                    // delivery from blocking enqueuers.
                    lock.unlock();
                    for (const auto continuation : due) deliver_(continuation);
                    lock.lock();
                }
            }
        }

        deliver_fn deliver_;
        std::mutex mutex_;
        std::condition_variable wakeup_;

        std::priority_queue<entry, std::vector<entry>, later_first> heap_;
        std::thread thread_;
        bool stopping_ = false;

        bool drained_at_shutdown_ = false;
    };
}
