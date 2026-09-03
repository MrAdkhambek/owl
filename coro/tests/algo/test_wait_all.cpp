#include <chrono>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include <coro/executors/timer_scheduler.h>
#include <coro/task.h>
#include <coro/run/sync_wait.h>
#include <coro/algo/wait_all.h>
#include "support/helpers.h"

using namespace std::chrono_literals;
using namespace coro_test;

namespace {
    // Bumps a counter on destruction, so a test can prove the child's frame
    // was actually freed -- including the last completer's, which used to be
    // stranded.
    int frames_destroyed = 0;

    struct destruction_marker {
        ~destruction_marker() {
            ++frames_destroyed;
        }
    };

    // Sleeps `delay`, then returns `value`. The marker rides along in the
    // frame so its destructor reports when that frame is released.
    coro::task<int> marked(coro::timer_scheduler& timer,
                           const std::chrono::milliseconds delay,
                           const int value) {
        destruction_marker marker;
        co_await timer.schedule_after(delay);
        co_return value;
    }

    // Runs a group expected to fail and reports the message that surfaced, so
    // the order-of-failure tests are one assertion each rather than a repeated
    // try/FAIL/catch scaffold.
    template <typename... Ts>
    std::string failure_of(coro::task<Ts>... ts) {
        try {
            static_cast<void>(coro::sync_wait(coro::wait_all(std::move(ts)...)));
        } catch (const std::runtime_error& e) {
            return e.what();
        }
        return "<no exception>";
    }
}

// Results come back as a tuple in argument order, whatever order the bodies
// actually finished in.
TEST(WaitAll, ResultsInArgumentOrder) {
    auto [a, b, c] = coro::sync_wait(coro::wait_all(val(1), val(2), val(3)));
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
    EXPECT_EQ(c, 3);
}

// void bodies occupy their tuple slot with the void_value placeholder, so the
// result layout does not depend on which bodies happened to produce values.
TEST(WaitAll, VoidAndValuedChildrenMix) {
    using coro::detail::void_value;
    auto [x, y] = coro::sync_wait(
        coro::wait_all(quiet(), val(7)));
    static_assert(std::is_same_v<decltype(x), void_value>);
    static_assert(std::is_same_v<decltype(y), int>);
    EXPECT_EQ(y, 7);
}

// The whole point: children overlap. Two sleeps of 50ms and 120ms cost about
// 120ms together -- the longest of them -- not the ~170ms sum that sequential
// driving would take. The window is wide because CI machines jitter, but the
// upper bound has to stay below the sequential figure or it proves nothing.
TEST(WaitAll, ChildrenActuallyOverlap) {
    coro::timer_scheduler timer;

    const auto start = std::chrono::steady_clock::now();
    static_cast<void>(coro::sync_wait(
        coro::wait_all(marked(timer, 50ms, 1), marked(timer, 120ms, 2))));
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    EXPECT_GE(elapsed.count(), 115);
    EXPECT_LT(elapsed.count(), 165);
}

// Fail-late by design: every child runs to completion before anyone can
// observe anything, so the FIRST-ARGUMENT failure is the one that surfaces,
// even though both children threw.
TEST(WaitAll, FirstArgumentExceptionWins) {
    EXPECT_EQ(failure_of(boom("first"), boom("second"), val(3)), "first");
}

// A sibling throwing after another succeeded still surfaces.
TEST(WaitAll, LaterFailureStillSurfaces) {
    EXPECT_EQ(failure_of(val(1), boom("late")), "late");
}

// When several fail, the earliest failing ARGUMENT wins -- not the earliest
// argument overall, which succeeded here.
TEST(WaitAll, EarliestFailingArgumentWins) {
    EXPECT_EQ(failure_of(val(1), boom("second"), boom("third")), "second");
}

// No children at all: completes immediately with an empty tuple.
TEST(WaitAll, EmptyGroupCompletes) {
    const auto nothing = coro::sync_wait(coro::wait_all());
    static_assert(std::is_same_v<decltype(nothing), const std::tuple<>>);
}

// Every child frame is freed exactly once, the LAST completer included -- and
// results still land in argument order even though the middle child finishes
// last. (That final frame used to be stranded in a global parking lot and
// freed twice.)
TEST(WaitAll, EveryChildFrameIsReaped) {
    frames_destroyed = 0;
    {
        coro::timer_scheduler timer;
        auto [a, b, c] = coro::sync_wait(coro::wait_all(
            marked(timer, 3ms, 1), marked(timer, 5ms, 2), marked(timer, 4ms, 3)));
        EXPECT_EQ(a, 1);
        EXPECT_EQ(b, 2);
        EXPECT_EQ(c, 3);
    }
    EXPECT_EQ(frames_destroyed, 3) << "a child frame was leaked or double-freed";
}

// Repeated groups in a row -- the shape a per-group frame leak or double free
// shows up in, which a single group can hide.
TEST(WaitAll, RepeatedGroupsAreClean) {
    coro::timer_scheduler timer;
    frames_destroyed = 0;

    for (int i = 0; i < 25; ++i) {
        auto [a, b] = coro::sync_wait(coro::wait_all(
            marked(timer, 1ms, i), marked(timer, 2ms, i + 1)));
        ASSERT_EQ(a, i);
        ASSERT_EQ(b, i + 1);
    }
    EXPECT_EQ(frames_destroyed, 50);
}

// A throwing child does not stop its siblings: all three bodies run and the
// failure surfaces only afterwards. Without this assertion the exception tests
// above would pass against a fail-fast implementation that skipped them.
TEST(WaitAll, FailingChildDoesNotStopSiblings) {
    reset_started();
    EXPECT_THROW(static_cast<void>(coro::sync_wait(
                     coro::wait_all(boom("first"), val(2), val(3)))),
                 std::runtime_error);
    EXPECT_EQ(start_count(), 3) << "siblings were skipped -- that is fail-fast, not fail-late";
}

// wait_all does NOT flatten: a wait_all handed to another wait_all lands as a
// nested tuple in one component, exactly like any other task's result.
TEST(WaitAll, NestedCompositionKeepsTuplesNested) {
    auto inner = coro::wait_all(val(40), val(2));
    auto [pair, extra] = coro::sync_wait(coro::wait_all(std::move(inner), val(100)));
    static_assert(std::is_same_v<decltype(pair), std::tuple<int, int>>);
    EXPECT_EQ(std::get<0>(pair), 40);
    EXPECT_EQ(std::get<1>(pair), 2);
    EXPECT_EQ(extra, 100);
}
