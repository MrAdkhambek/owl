#include <coroutine>
#include <cstddef>
#include <exception>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <coro/executors/static_thread_pool.h>
#include <coro/run/schedule_on.h>
#include <coro/sync/async_mutex.h>
#include <coro/task.h>
#include <coro/run/sync_wait.h>
#include <coro/algo/wait_all.h>

namespace {
    // A fire-and-forget coroutine: it starts on call and destroys its own frame
    // when it ends.
    //
    // The suite needs it because every interesting property of a mutex is about
    // a waiter that has ARRIVED but not yet acquired -- and coro::task is lazy,
    // so a task cannot be parked on the mutex unless something is already
    // awaiting it, which would block the test itself.
    //
    // With this, a test takes the lock on the main thread, calls a few of these
    // (each runs until it parks), and the mutex's whole state machine becomes
    // observable from a single thread with no scheduler, no sleeps, and no
    // races: unlock() is then the only thing that can move it.
    struct detached {
        struct promise_type {
            detached get_return_object() const noexcept {
                return {};
            }

            std::suspend_never initial_suspend() const noexcept {
                return {};
            }

            std::suspend_never final_suspend() const noexcept {
                return {};
            }

            void return_void() const noexcept {
            }

            // A test fixture that throws has no one to deliver the exception
            // to; failing loudly beats unwinding out of resume().
            void unhandled_exception() const {
                std::terminate();
            }
        };
    };

    // Parks on `m`, then appends `id` once it gets in. The guard releases on
    // the way out, which is what lets a chain of these drain by itself.
    detached enter(coro::async_mutex& m, std::vector<int>& log, const int id) {
        auto guard = co_await m.scoped_lock();
        log.push_back(id);
    }

    // The same, through the bare lock()/unlock() pair rather than a guard.
    detached enter_bare(coro::async_mutex& m, std::vector<int>& log, const int id) {
        co_await m.lock();
        log.push_back(id);
        m.unlock();
    }

    // Parks, and once it is in, starts ANOTHER waiter from inside its own
    // critical section -- so the newcomer arrives while the queue is still
    // draining, and lands on the arrival stack rather than in the batch that
    // unlock() already reversed.
    detached enter_spawning(coro::async_mutex& m, std::vector<int>& log,
                            const int id, const int spawn_id) {
        auto guard = co_await m.scoped_lock();
        log.push_back(id);
        enter(m, log, spawn_id);
    }

    // Records the thread the waiter actually woke up on.
    detached enter_noting_thread(coro::async_mutex& m, std::thread::id& where) {
        auto guard = co_await m.scoped_lock();
        where = std::this_thread::get_id();
    }

    // Parks a waiter from wherever this task happens to be running, then ends.
    // The detached waiter it started stays parked after the task completes,
    // which is what lets a test suspend on one thread and unlock on another.
    coro::task<> park_waiter(coro::async_mutex& m, std::thread::id& where) {
        enter_noting_thread(m, where);
        co_return;
    }

    // Throws with the guard in scope, so the release has to happen during
    // unwinding rather than at a normal scope exit.
    detached enter_and_throw(coro::async_mutex& m, bool& caught) {
        try {
            auto guard = co_await m.scoped_lock();
            throw std::runtime_error("boom");
        } catch (const std::runtime_error&) {
            caught = true;
        }
    }

    // Moves the guard into an inner scope, which is where the release must
    // happen, then re-takes the mutex. Leaving this coroutine destroys the
    // outer, moved-from guard -- which owns nothing and must therefore leave
    // that second acquisition alone.
    detached enter_moving_guard(coro::async_mutex& m, bool& retaken) {
        auto guard = co_await m.scoped_lock();
        {
            const coro::async_mutex_lock moved{std::move(guard)};
        }
        retaken = m.try_lock();
    }

    // Read, pause, write back. A plain int, deliberately: without real
    // exclusion the wide window between the read and the write loses updates,
    // and without a properly ordered handoff the accesses are a data race.
    coro::task<> bump_under_lock(coro::async_mutex& m, int& counter, const int iterations) {
        for (int i = 0; i < iterations; ++i) {
            auto guard = co_await m.scoped_lock();
            const int seen = counter;
            std::this_thread::yield();
            counter = seen + 1;
        }
    }
}

TEST(AsyncMutex, TryLockSucceedsWhenFree) {
    coro::async_mutex m;
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(AsyncMutex, TryLockFailsWhileHeld) {
    coro::async_mutex m;
    ASSERT_TRUE(m.try_lock());
    EXPECT_FALSE(m.try_lock());
    m.unlock();
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

// The documented fast path: with the mutex free there is nothing to wait for,
// so the awaiter reports ready and the coroutine never suspends at all.
// await_ready() is where that acquisition happens, so calling it here really
// does take the lock -- which the second assertion confirms.
TEST(AsyncMutex, UncontendedLockIsReadyWithoutSuspending) {
    coro::async_mutex m;
    auto op = m.lock();
    EXPECT_TRUE(op.await_ready());
    EXPECT_FALSE(m.try_lock());
    m.unlock();
}

TEST(AsyncMutex, ContendedLockIsNotReady) {
    coro::async_mutex m;
    ASSERT_TRUE(m.try_lock());
    auto op = m.lock();
    EXPECT_FALSE(op.await_ready());
    m.unlock();
}

TEST(AsyncMutex, WaiterParksUntilUnlock) {
    coro::async_mutex m;
    std::vector<int> log;

    ASSERT_TRUE(m.try_lock());
    enter(m, log, 1);
    EXPECT_TRUE(log.empty()) << "waiter got in while the mutex was held";

    m.unlock();
    EXPECT_EQ(log, std::vector{1});
}

// The reason the waiter stack is reversed before it is drained: arrivals push
// LIFO, so handing them the lock in stack order would run 3, 2, 1.
TEST(AsyncMutex, WaitersAcquireInArrivalOrder) {
    coro::async_mutex m;
    std::vector<int> log;

    ASSERT_TRUE(m.try_lock());
    enter(m, log, 1);
    enter(m, log, 2);
    enter(m, log, 3);
    ASSERT_TRUE(log.empty());

    m.unlock();
    EXPECT_EQ(log, (std::vector{1, 2, 3}));
}

// The half-drained case: unlock() holds a reversed batch it has not finished
// spending, and a fresh waiter shows up on the arrival stack behind it. The
// newcomer must queue after the batch -- not jump ahead of it, and not be
// stranded when the batch runs out.
TEST(AsyncMutex, WaiterArrivingMidDrainQueuesBehindTheBatch) {
    coro::async_mutex m;
    std::vector<int> log;

    ASSERT_TRUE(m.try_lock());
    enter_spawning(m, log, 1, 3);
    enter(m, log, 2);
    ASSERT_TRUE(log.empty());

    m.unlock();
    EXPECT_EQ(log, (std::vector{1, 2, 3}));
}

TEST(AsyncMutex, BareLockAndUnlockAlsoQueueInArrivalOrder) {
    coro::async_mutex m;
    std::vector<int> log;

    ASSERT_TRUE(m.try_lock());
    enter_bare(m, log, 1);
    enter_bare(m, log, 2);
    enter_bare(m, log, 3);
    ASSERT_TRUE(log.empty()) << "waiters ran while the mutex was held";

    m.unlock();
    EXPECT_EQ(log, (std::vector{1, 2, 3}));
}

TEST(AsyncMutex, TryLockFailsWhileWaitersAreParked) {
    coro::async_mutex m;
    std::vector<int> log;

    ASSERT_TRUE(m.try_lock());
    enter(m, log, 1);
    // Distinct from TryLockFailsWhileHeld: the state word is now a pointer to
    // the waiter stack rather than the plain "locked, nobody waiting" value.
    EXPECT_FALSE(m.try_lock());

    m.unlock();
}

// Once the last waiter leaves, the mutex has to be back in its initial state
// rather than stuck holding an empty queue.
TEST(AsyncMutex, MutexIsFreeAgainAfterQueueDrains) {
    coro::async_mutex m;
    std::vector<int> log;

    ASSERT_TRUE(m.try_lock());
    enter(m, log, 1);
    enter(m, log, 2);
    ASSERT_TRUE(log.empty());
    m.unlock();
    ASSERT_EQ(log.size(), 2u);

    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(AsyncMutex, ScopedLockReleasesAtScopeExit) {
    coro::async_mutex m;
    std::vector<int> log;

    enter(m, log, 1);
    ASSERT_EQ(log, std::vector{1});
    EXPECT_TRUE(m.try_lock()) << "guard did not release at scope exit";
    m.unlock();
}

TEST(AsyncMutex, ScopedLockReleasesWhileUnwinding) {
    coro::async_mutex m;
    bool caught = false;

    enter_and_throw(m, caught);
    ASSERT_TRUE(caught);
    EXPECT_TRUE(m.try_lock()) << "guard did not release during unwinding";
    m.unlock();
}

TEST(AsyncMutex, MovedFromGuardDoesNotUnlockTwice) {
    coro::async_mutex m;
    bool retaken = false;

    enter_moving_guard(m, retaken);
    // The inner guard released, so the coroutine could take the mutex again.
    ASSERT_TRUE(retaken) << "the moved-to guard never released";
    // And that second acquisition is still standing: destroying the
    // moved-from guard on the way out must not have released it.
    EXPECT_FALSE(m.try_lock()) << "moved-from guard unlocked a mutex it did not own";
    m.unlock();
}

TEST(AsyncMutex, AdoptLockGuardReleasesABareLock) {
    coro::async_mutex m;
    ASSERT_TRUE(m.try_lock());
    {
        const coro::async_mutex_lock guard{m, coro::adopt_lock};
        EXPECT_TRUE(guard.owns_lock());
        EXPECT_TRUE(static_cast<bool>(guard));
        EXPECT_FALSE(m.try_lock());
    }
    EXPECT_TRUE(m.try_lock());
    m.unlock();
}

TEST(AsyncMutex, GuardMoveAssignReleasesThePreviousLock) {
    coro::async_mutex a;
    coro::async_mutex b;
    ASSERT_TRUE(a.try_lock());
    ASSERT_TRUE(b.try_lock());

    coro::async_mutex_lock first{a, coro::adopt_lock};
    coro::async_mutex_lock second{b, coro::adopt_lock};
    first = std::move(second);

    EXPECT_TRUE(first.owns_lock());
    EXPECT_FALSE(second.owns_lock());
    EXPECT_TRUE(a.try_lock()) << "move-assign did not unlock the previous mutex";
    a.unlock();
    EXPECT_FALSE(b.try_lock()) << "move-assign dropped the incoming lock";
    // first still owns b
}

TEST(AsyncMutex, MovedFromGuardDoesNotOwnTheLock) {
    coro::async_mutex m;
    ASSERT_TRUE(m.try_lock());
    coro::async_mutex_lock guard{m, coro::adopt_lock};
    const coro::async_mutex_lock moved{std::move(guard)};
    EXPECT_FALSE(guard.owns_lock());
    EXPECT_TRUE(moved.owns_lock());
}

// Pins the resume policy: unlock() hands the mutex to the next waiter by
// resuming it right here, so the waiter's critical section runs on the
// unlocking thread rather than wherever it originally suspended.
TEST(AsyncMutex, UnlockResumesWaiterOnTheUnlockingThread) {
    coro::static_thread_pool pool{2};
    coro::async_mutex m;

    ASSERT_TRUE(m.try_lock());

    std::thread::id woke_on{};
    // Park the waiter from a pool worker, so "where it suspended" and "where
    // it is resumed" are genuinely different threads.
    coro::sync_wait(coro::schedule_on(pool, park_waiter(m, woke_on)));

    ASSERT_EQ(woke_on, std::thread::id{}) << "waiter ran before the unlock";

    m.unlock();
    EXPECT_EQ(woke_on, std::this_thread::get_id());
}

// Eight tasks on four workers, each taking the mutex twenty-five times. A
// mutex that failed to exclude would lose increments across the read/write
// window; one that excluded without ordering the handoff would be a data race
// under a sanitizer.
TEST(AsyncMutex, ConcurrentTasksSerialiseTheirCriticalSections) {
    coro::static_thread_pool pool{4};
    coro::async_mutex m;
    int counter = 0;

    constexpr int iterations = 25;
    constexpr std::size_t tasks = 8;

    const auto bump = [&] {
        return coro::schedule_on(pool, bump_under_lock(m, counter, iterations));
    };

    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        static_cast<void>(coro::sync_wait(coro::wait_all(
            ((static_cast<void>(Is), bump()))...)));
    }(std::make_index_sequence<tasks>{});

    EXPECT_EQ(counter, static_cast<int>(tasks) * iterations);
    EXPECT_TRUE(m.try_lock()) << "mutex left locked after every task finished";
    m.unlock();
}

// A long queue of waiters, each of which releases the mutex before it
// suspends again, unwinds on the unlocking thread. It has to unwind as a
// loop, not as one nested resume per waiter: a secondary thread's stack --
// 512 KiB on macOS, which is what every pool worker gets -- does not survive
// thousands of nested unlock/resume/body frames.
TEST(AsyncMutex, LongWaiterChainUnwindsWithoutRecursing) {
    coro::async_mutex m;
    std::vector<int> log;
    constexpr int waiters = 20000;
    log.reserve(waiters);

    ASSERT_TRUE(m.try_lock());
    for (int i = 0; i < waiters; ++i) enter(m, log, i);
    ASSERT_TRUE(log.empty());

    std::thread([&m] {
        m.unlock();
    }).join();

    ASSERT_EQ(log.size(), static_cast<std::size_t>(waiters));
    EXPECT_EQ(log.front(), 0);
    EXPECT_EQ(log.back(), waiters - 1);
    EXPECT_TRUE(m.try_lock()) << "mutex left locked after the chain drained";
    m.unlock();
}
