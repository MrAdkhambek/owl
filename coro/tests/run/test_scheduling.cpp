#include <atomic>
#include <cstddef>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <utility>

#include <coro/executors/static_thread_pool.h>
#include <coro/run/resume_on.h>
#include <coro/run/schedule_on.h>
#include <coro/task.h>
#include <coro/algo/fmap.h>
#include <coro/run/sync_wait.h>
#include <coro/algo/wait_all.h>
#include "support/helpers.h"

using namespace coro_test;

namespace {
    coro::task<int> noted(std::thread::id& where, const int value) {
        where = std::this_thread::get_id();
        co_return value;
    }

    // The void-result counterpart of noted(), so schedule_on's void branch is
    // exercised by a body that still reports where it ran.
    coro::task<> noted_void(std::thread::id& where) {
        where = std::this_thread::get_id();
        co_return;
    }

    // Runs after pool.schedule(): whatever thread this reports is the worker
    // that picked the continuation up.
    coro::task<std::thread::id> current_thread_after_schedule(coro::static_thread_pool& pool) {
        co_await pool.schedule();
        co_return std::this_thread::get_id();
    }

    // resume_on moves the CONTINUATION, not the body -- so what matters is
    // where the statement after the co_await lands, which is what these two
    // report. The wrapped body itself runs on the calling thread.
    coro::task<std::thread::id> thread_after_resume_on(coro::static_thread_pool& pool) {
        static_cast<void>(co_await coro::resume_on(pool, val(7)));
        co_return std::this_thread::get_id();
    }

    coro::task<std::thread::id> thread_after_resume_on_void(coro::static_thread_pool& pool) {
        co_await coro::resume_on(pool, quiet());
        co_return std::this_thread::get_id();
    }

    // Six consecutive schedule() hops; round-robin assignment must place
    // adjacent hops on different workers.
    coro::task<> rotation_probe(coro::static_thread_pool& pool, std::thread::id* seen) {
        for (int i = 0; i < 6; ++i) {
            co_await pool.schedule();
            seen[i] = std::this_thread::get_id();
        }
    }

    coro::task<> bump(std::atomic<int>& counter) {
        counter.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
}

TEST(Scheduling, ScheduleResumesOnWorkerNotCaller) {
    const auto caller = std::this_thread::get_id();
    coro::static_thread_pool pool{4};
    EXPECT_NE(coro::sync_wait(current_thread_after_schedule(pool)), caller);
}

TEST(Scheduling, ScheduleOnMovesWorkToWorker) {
    const auto caller = std::this_thread::get_id();
    coro::static_thread_pool pool{4};

    std::thread::id where{};
    EXPECT_EQ(coro::sync_wait(coro::schedule_on(pool, noted(where, 42))), 42);
    EXPECT_NE(where, caller);
}

TEST(Scheduling, ScheduleOnVoidOverloadRunsBodyOnWorker) {
    const auto caller = std::this_thread::get_id();
    coro::static_thread_pool pool{4};

    std::thread::id where{};
    coro::sync_wait(coro::schedule_on(pool, noted_void(where)));
    EXPECT_NE(where, caller);
}

TEST(Scheduling, PipeScheduleOnComposesWithFmap) {
    coro::static_thread_pool pool{4};
    const int r = coro::sync_wait(
        coro::schedule_on(pool, val(9))
        | coro::fmap([](int x) {
            return x + 1;
        }));
    EXPECT_EQ(r, 10);
}

TEST(Scheduling, ResumeOnDeliversResult) {
    coro::static_thread_pool pool{4};
    EXPECT_EQ(coro::sync_wait(coro::resume_on(pool, val(7))), 7);

    auto piped = val(8) | coro::resume_on(pool);
    EXPECT_EQ(coro::sync_wait(std::move(piped)), 8);
}

TEST(Scheduling, ResumeOnMovesContinuationToWorker) {
    const auto caller = std::this_thread::get_id();
    coro::static_thread_pool pool{4};
    EXPECT_NE(coro::sync_wait(thread_after_resume_on(pool)), caller);
}

TEST(Scheduling, ResumeOnVoidOverloadMovesContinuation) {
    const auto caller = std::this_thread::get_id();
    coro::static_thread_pool pool{4};
    EXPECT_NE(coro::sync_wait(thread_after_resume_on_void(pool)), caller);
}

TEST(Scheduling, RoundRobinRotatesAdjacentWorkers) {
    coro::static_thread_pool pool{4};
    std::thread::id seen[6] = {};
    coro::sync_wait(rotation_probe(pool, seen));

    for (int i = 1; i < 6; ++i) {
        EXPECT_NE(seen[i], seen[i - 1]) << "hop " << i;
    }
}

// Both scheduled tasks are started before either is waited on, so they
// overlap on the pool's workers. What this pins down is that scheduled tasks
// compose with wait_all at all, that results still land in argument order,
// and that each task ran off the calling thread.
TEST(Scheduling, WaitAllRunsScheduledTasksConcurrently) {
    const auto caller = std::this_thread::get_id();
    coro::static_thread_pool pool{4};

    std::thread::id first{};
    std::thread::id second{};
    auto [a, b] = coro::sync_wait(coro::wait_all(
        coro::schedule_on(pool, noted(first, 10)),
        coro::schedule_on(pool, noted(second, 20))));

    EXPECT_EQ(a, 10);
    EXPECT_EQ(b, 20);
    EXPECT_NE(first, caller);
    EXPECT_NE(second, caller);
}

TEST(Scheduling, CrossThreadExceptionReachesSyncWait) {
    coro::static_thread_pool pool{4};
    EXPECT_THROW(static_cast<void>(coro::sync_wait(
                     coro::schedule_on(pool, boom()))),
                 std::runtime_error);
}

// Twenty scheduled bumps -- five times the pool's worker count -- all driven
// through one wait_all group: every worker has to pick work up before the
// group drains, and the counter must end at exactly twenty.
TEST(Scheduling, TwentyScheduledTasksAllComplete) {
    coro::static_thread_pool pool{4};
    std::atomic<int> counter{0};

    const auto bump_task = [&] {
        return coro::schedule_on(pool, bump(counter));
    };

    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        static_cast<void>(coro::sync_wait(coro::wait_all(
            ((static_cast<void>(Is), bump_task()))...)));
    }(std::make_index_sequence<20>{});

    EXPECT_EQ(counter.load(), 20);
}
