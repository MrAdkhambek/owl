#pragma once

// Fixtures shared by the suite. These were previously copy-pasted into four
// test files and had drifted apart -- one copy's boom() had stopped bumping
// the counter, which quietly weakened every count-based assertion around it.

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

#include <coro/async_generator.h>
#include <coro/executors/timer_scheduler.h>
#include <coro/task.h>

namespace coro_test {
    // Bumped by every fixture body below. Several assertions turn on how many
    // bodies actually ran: laziness (nothing starts before its co_await) and
    // wait_all's fail-late policy (a throwing sibling does not stop the
    // others, so every body still counts).
    //
    // Atomic because the scheduling suite runs these same bodies on pool
    // workers. The pool's mutex handoff already orders the increments, so
    // this is about keeping the tests clean under a sanitizer, not about
    // fixing a live race.
    inline std::atomic<int> started{0};

    inline void reset_started() noexcept {
        started.store(0, std::memory_order_relaxed);
    }

    inline int start_count() noexcept {
        return started.load(std::memory_order_relaxed);
    }

    inline coro::task<int> val(const int v) {
        started.fetch_add(1, std::memory_order_relaxed);
        co_return v;
    }

    inline coro::task<> quiet() {
        started.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    // The co_return is load-bearing, not decoration. Without a coroutine
    // keyword this would be an ordinary function that throws during ARGUMENT
    // evaluation, before the combinator under test is ever entered -- which
    // silently turns every exception assertion into a tautology.
    inline coro::task<int> boom(const char* what) {
        started.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error(what);
        co_return 0;
    }

    // The message-less form the rest of the suite uses.
    inline coro::task<int> boom() {
        return boom("boom");
    }

    inline coro::task<int&> give_ref(int& slot) {
        co_return slot;
    }

    // Drains a whole sequence into a vector -- the collector most flow tests
    // end with.
    template <typename T>
    inline coro::task<std::vector<T>> collect(coro::async_generator<T> g) {
        std::vector<T> out;
        while (auto v = co_await g.next()) out.push_back(std::move(*v));
        co_return out;
    }

    // A finite timed sequence: 0..n-1, one element every `step`. When
    // `finished` is given, it is bumped once the body runs out -- how
    // teardown tests prove an abandoned pump actually drained its source.
    inline coro::async_generator<int> ticks(coro::timer_scheduler& timer,
                                            const std::chrono::milliseconds step,
                                            const int n,
                                            std::atomic<int>* finished = nullptr) {
        for (int i = 0; i < n; ++i) {
            co_await timer.schedule_after(step);
            co_yield i;
        }
        if (finished) finished->fetch_add(1, std::memory_order_relaxed);
    }

    // A synchronous sequence: every element yields without suspending, so a
    // source like this runs out before any timer-driven sibling can fire.
    //
    // BY VALUE, not by const reference. This is a coroutine, and it is lazy:
    // the body does not run until the generator is first pulled, long after
    // the full-expression that built the argument has ended. A reference
    // parameter would dangle for every caller who writes the braced list
    // inline -- `seq({1, 2, 3})` -- which is how every caller writes it.
    inline coro::async_generator<int> seq(std::vector<int> xs) {
        for (const int x : xs) co_yield x;
    }
}
