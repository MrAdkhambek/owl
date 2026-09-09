#include <chrono>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <coro/async_generator.h>
#include <coro/executors/timer_scheduler.h>
#include <coro/task.h>
#include <coro/algo/combine.h>
#include <coro/algo/fmap.h>
#include <coro/run/sync_wait.h>
#include "support/helpers.h"

using namespace std::chrono_literals;
using namespace coro_test;

namespace {
    // How many source bodies have entered. Laziness assertions turn on this
    // reading zero.
    int bodies = 0;

    coro::async_generator<int> counted(const int n) {
        ++bodies;
        for (int i = 0; i < n; ++i) co_yield i;
    }

    coro::async_generator<std::string> letters(const int n) {
        for (int i = 0; i < n; ++i) co_yield std::string(1, static_cast<char>('a' + i));
    }

    coro::async_generator<std::unique_ptr<int>> boxed(const int n) {
        for (int i = 0; i < n; ++i) co_yield std::make_unique<int>(i);
    }

    // Yields once, then fails -- so the throw lands mid-stream rather than
    // during the first pull.
    coro::async_generator<int> throws_on_second() {
        co_yield 0;
        throw std::runtime_error("src");
    }

    // TEST bodies are plain functions, so every pull happens inside a task
    // like these.
    template <typename T>
    coro::task<std::size_t> count_of(coro::async_generator<T> g) {
        std::size_t n = 0;
        while (co_await g.next()) ++n;
        co_return n;
    }

    // Takes two elements and walks away, leaving the sources mid-stream for
    // the destructor to clean up.
    coro::task<int> take_two_then_drop(coro::async_generator<std::tuple<int, int>> g) {
        int last = -1;
        for (int i = 0; i < 2; ++i) last = std::get<0>(*co_await g.next());
        co_return last;
    }

    coro::task<std::vector<int>> deref_all(coro::async_generator<std::tuple<std::unique_ptr<int>, int>> g) {
        std::vector<int> out;
        while (auto v = co_await g.next()) out.push_back(*std::get<0>(*v));
        co_return out;
    }
}

// The core contract: element i of the left meets element i of the right.
TEST(Combine, PairsSourcesInLockstep) {
    const auto out = coro::sync_wait(collect(coro::combine(seq({1, 2, 3}), seq({10, 20, 30}))));

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], (std::tuple{1, 10}));
    EXPECT_EQ(out[1], (std::tuple{2, 20}));
    EXPECT_EQ(out[2], (std::tuple{3, 30}));
}

// The two sides need not share an element type -- that is the point of the
// combinator, not an afterthought.
TEST(Combine, SourcesMayHoldDifferentTypes) {
    const auto out = coro::sync_wait(collect(coro::combine(seq({1, 2}), letters(2))));

    static_assert(std::is_same_v<decltype(out), const std::vector<std::tuple<int, std::string>>>);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(std::get<1>(out[0]), "a");
    EXPECT_EQ(std::get<1>(out[1]), "b");
}

// Whichever side runs dry first ends the stream; the survivor's extras have
// nothing to pair with and never surface.
TEST(Combine, ShorterLeftSourceEndsTheStream) {
    EXPECT_EQ(coro::sync_wait(count_of(coro::combine(seq({1, 2}), seq({1, 2, 3, 4, 5})))), 2u);
}

TEST(Combine, ShorterRightSourceEndsTheStream) {
    EXPECT_EQ(coro::sync_wait(count_of(coro::combine(seq({1, 2, 3, 4, 5}), seq({1, 2})))), 2u);
}

TEST(Combine, EmptySourceYieldsNothing) {
    EXPECT_EQ(coro::sync_wait(count_of(coro::combine(seq({}), seq({1, 2, 3})))), 0u);
    EXPECT_EQ(coro::sync_wait(count_of(coro::combine(seq({1, 2, 3}), seq({})))), 0u);
}

// The terminating step pulls both sides at once, so by the time one side is
// known to be dry the other's next element has already been produced -- and
// is dropped. That is the price of pulling concurrently. combine owns both
// sources, so the element itself is unobservable; a side effect of producing
// it (a counter here, a socket read in life) is not undone.
TEST(Combine, TerminatingStepPullsOneSurplusElement) {
    int produced = 0;
    auto counting = [&produced](const int n) -> coro::async_generator<int> {
        for (int i = 0; i < n; ++i) {
            ++produced;
            co_yield i;
        }
    };

    EXPECT_EQ(coro::sync_wait(count_of(coro::combine(seq({1}), counting(5)))), 1u);
    EXPECT_EQ(produced, 2) << "one paired, one surplus";
}

// Building the pipeline must not start either body. Only the first pull does.
TEST(Combine, BuildingStartsNoBody) {
    bodies = 0;
    {
        auto zipped = coro::combine(counted(3), counted(3));
        EXPECT_EQ(bodies, 0);
        // zipped is dropped here, still never pulled -- both sources go with
        // it, having never run a statement.
    }
    EXPECT_EQ(bodies, 0);
}

TEST(Combine, FirstPullStartsBothBodies) {
    bodies = 0;
    EXPECT_EQ(coro::sync_wait(count_of(coro::combine(counted(2), counted(2)))), 2u);
    EXPECT_EQ(bodies, 2);
}

// Abandoning the zip mid-stream tears down both sources with it.
TEST(Combine, AbandoningMidStreamIsClean) {
    bodies = 0;
    EXPECT_EQ(coro::sync_wait(take_two_then_drop(coro::combine(counted(100), counted(100)))), 1);
    EXPECT_EQ(bodies, 2);
}

// Elements are handed over already moved out of their slots, so a move-only
// element type rides through the tuple untouched.
TEST(Combine, MoveOnlyElementsSurvive) {
    const auto out = coro::sync_wait(deref_all(coro::combine(boxed(3), seq({7, 8, 9}))));

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[2], 2);
}

// The three-argument form: the callable comes last and shapes each pair.
TEST(Combine, TransformShapesEachPair) {
    const auto out = coro::sync_wait(collect(coro::combine(
        seq({1, 2, 3}),
        seq({10, 20, 30}),
        [](const int a, const int b) { return a + b; })));

    EXPECT_EQ(out, (std::vector{11, 22, 33}));
}

TEST(Combine, TransformRunsOncePerEmittedPair) {
    int calls = 0;
    const auto out = coro::sync_wait(collect(coro::combine(
        seq({1, 2}),
        seq({10, 20, 30, 40}),          // longer: the extras must not be transformed
        [&](const int a, const int b) {
            ++calls;
            return a + b;
        })));

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(calls, 2);
}

// A coroutine callable is awaited during the very pull that demands the
// element -- task<R> goes in, R comes out.
TEST(Combine, CoroutineCallableIsAwaited) {
    const auto out = coro::sync_wait(collect(coro::combine(
        seq({1, 2}),
        letters(2),
        [](const int a, std::string b) -> coro::task<std::string> {
            co_return std::to_string(a) + b;
        })));

    static_assert(std::is_same_v<decltype(out), const std::vector<std::string>>);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "1a");
    EXPECT_EQ(out[1], "2b");
}

// A coroutine callable must stay as lazy as a plain one.
TEST(Combine, CoroutineCallableStaysLazy) {
    int calls = 0;
    {
        auto g = coro::combine(seq({1, 2}), seq({3, 4}),
                               [&](const int a, const int b) -> coro::task<int> {
                                   ++calls;
                                   co_return a + b;
                               });
        EXPECT_EQ(calls, 0);
    }
    EXPECT_EQ(calls, 0);
}

// The transform is invoked as F&, so a mutable callable keeps its state
// across elements rather than being called on a fresh copy each time.
TEST(Combine, MutableCallableKeepsStateAcrossElements) {
    const auto out = coro::sync_wait(collect(coro::combine(
        seq({0, 0, 0}),
        seq({0, 0, 0}),
        [n = 0](int, int) mutable { return ++n; })));

    EXPECT_EQ(out, (std::vector{1, 2, 3}));
}

// The pipe spelling of the bare zip, and its chain with fmap's own pipe.
TEST(Combine, PipeSpellingZipsTwoSources) {
    const auto out = coro::sync_wait(collect(seq({1, 2}) | seq({10, 20})));

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], (std::tuple{1, 10}));
}

TEST(Combine, PipeChainsIntoFmap) {
    const auto out = coro::sync_wait(collect(
        seq({1, 2}) | letters(2) | coro::fmap([](std::tuple<int, std::string> t) {
            return std::to_string(std::get<0>(t)) + std::get<1>(t);
        })));

    EXPECT_EQ(out, (std::vector<std::string>{"1a", "2b"}));
}

// Exceptions travel unchanged, whichever side they come from.
TEST(Combine, LeftSourceExceptionPropagates) {
    EXPECT_THROW(static_cast<void>(coro::sync_wait(count_of(
                     coro::combine(throws_on_second(), seq({1, 2, 3}))))),
                 std::runtime_error);
}

TEST(Combine, RightSourceExceptionPropagates) {
    EXPECT_THROW(static_cast<void>(coro::sync_wait(count_of(
                     coro::combine(seq({1, 2, 3}), throws_on_second())))),
                 std::runtime_error);
}

TEST(Combine, CallableExceptionPropagates) {
    EXPECT_THROW(static_cast<void>(coro::sync_wait(count_of(
                     coro::combine(seq({1}), seq({2}),
                                   [](int, int) -> int { throw std::logic_error("f"); })))),
                 std::logic_error);
}

TEST(Combine, CoroutineCallableExceptionPropagates) {
    EXPECT_THROW(static_cast<void>(coro::sync_wait(count_of(
                     coro::combine(seq({1}), seq({2}),
                                   [](int, int) -> coro::task<int> {
                                       throw std::logic_error("f");
                                       co_return 0;
                                   })))),
                 std::logic_error);
}

// The load-bearing performance property: the two sources are pulled
// CONCURRENTLY, not one after the other. Three pairs, each side waiting 40ms
// per element, costs ~120ms concurrently against ~240ms sequentially. The
// bound is deliberately slack -- it only has to separate the two regimes.
TEST(Combine, PullsBothSourcesConcurrently) {
    coro::timer_scheduler timer;

    const auto start = std::chrono::steady_clock::now();
    const auto n = coro::sync_wait(count_of(coro::combine(ticks(timer, 40ms, 3), ticks(timer, 40ms, 3))));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(n, 3u);
    EXPECT_GE(elapsed, 100ms);   // three sequential rounds still have to happen
    EXPECT_LT(elapsed, 220ms);   // but not six -- that would be sequential pulling
}

// A source that ends without producing silences the stream even when the
// other side is still waiting on a timer.
TEST(Combine, ExhaustedSyncSourceEndsTimedStream) {
    coro::timer_scheduler timer;

    EXPECT_EQ(coro::sync_wait(count_of(coro::combine(seq({}), ticks(timer, 20ms, 3)))), 0u);
}

// The constraint fmap carries, carried here too: the pairing callable's
// result has to be an object, so void, references, and their task forms are
// all quietly removed from the overload set rather than erupting in a body.
namespace {
    coro::async_generator<int> ints();

    auto f_void  = [](int, int) {};
    auto f_tvoid = [](int, int) -> coro::task<void> { co_return; };
    auto f_ref   = [](int, int) -> int& { static int s; return s; };
    auto f_plain = [](const int a, const int b) { return a + b; };
    auto f_task  = [](const int a, const int b) -> coro::task<int> { co_return a + b; };

    template <typename F>
    concept combinable = requires(F f) { coro::combine(ints(), ints(), f); };

    static_assert(!combinable<decltype(f_void)>, "a void-returning callable has no element to yield");
    static_assert(!combinable<decltype(f_tvoid)>, "task<void> carries no value either");
    static_assert(!combinable<decltype(f_ref)>, "async_generator cannot hold a reference");
    static_assert(combinable<decltype(f_plain)>);
    static_assert(combinable<decltype(f_task)>);
}
