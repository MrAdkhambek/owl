#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <coro/async_generator.h>
#include <coro/task.h>
#include <coro/algo/fmap.h>
#include <coro/run/sync_wait.h>
#include "support/helpers.h"

using namespace coro_test;

namespace {
    int applied = 0;

    int fn_copies = 0;

    // Reports how often it was copied, so the tests below can tell a stored
    // callable from a borrowed reference to the caller's object.
    struct CountedFn {
        int bias;

        explicit CountedFn(const int b) noexcept : bias(b) {
        }

        CountedFn(const CountedFn& o) noexcept : bias(o.bias) {
            ++fn_copies;
        }

        CountedFn(CountedFn&&) noexcept = default;

        int operator()(const int x) const {
            return x + bias;
        }
    };

    // A callable that cannot be copied at all: it can only reach a transform
    // by being moved in.
    struct MoveOnlyFn {
        std::unique_ptr<int> bias;

        explicit MoveOnlyFn(const int b) : bias(std::make_unique<int>(b)) {
        }

        MoveOnlyFn(MoveOnlyFn&&) noexcept = default;
        MoveOnlyFn(const MoveOnlyFn&) = delete;

        int operator()(const int x) const {
            return x + *bias;
        }
    };

    coro::async_generator<long long> numbers(const int n) {
        for (long long i = 1; i <= n; ++i) {
            co_yield i;
        }
    }

    // TEST bodies are plain functions, so pulls happen in this helper instead.
    coro::task<long long> pull_four(coro::async_generator<long long>& gen) {
        long long last = 0;
        for (int i = 0; i < 4; ++i) {
            last = *co_await gen.next();
        }
        co_return last;
    }
}

TEST(Fmap, TransformsTaskResult) {
    reset_started();
    applied = 0;
    auto mapped = coro::fmap(val(21), [&](const int x) {
        ++applied;
        return x * 2;
    });
    EXPECT_EQ(applied, 0);
    EXPECT_EQ(coro::sync_wait(std::move(mapped)), 42);
    EXPECT_EQ(start_count(), 1);
    EXPECT_EQ(applied, 1);
}

TEST(Fmap, ChainOfTwoTransforms) {
    auto c = coro::fmap(
        coro::fmap(val(9), [](int x) {
            return std::to_string(x);
        }),
        [](std::string s) {
            return s + "!";
        });
    const std::string s = coro::sync_wait(std::move(c));
    EXPECT_EQ(s, "9!");
}

TEST(Fmap, VoidSourceBecomesValue) {
    reset_started();
    const int v = coro::sync_wait(
        coro::fmap(quiet(), [] {
            return 7;
        }));
    EXPECT_EQ(v, 7);
    EXPECT_EQ(start_count(), 1);
}

TEST(Fmap, SourceExceptionPropagatesUnchanged) {
    EXPECT_THROW(static_cast<void>(coro::sync_wait(
                     coro::fmap(boom(), [](int) { return 1; }))),
                 std::runtime_error);
}

TEST(Fmap, CallableExceptionPropagates) {
    EXPECT_THROW(static_cast<void>(coro::sync_wait(
                     coro::fmap(val(3), [](int) -> int { throw std::logic_error("f"); }))),
                 std::logic_error);
}

TEST(Fmap, RvaluePipeAppliesOnce) {
    applied = 0;
    auto t = val(5)
        | coro::fmap([](int x) {
            ++applied;
            return x + 1;
        })
        | coro::fmap([](int x) {
            ++applied;
            return x * 2;
        });
    EXPECT_EQ(applied, 0);
    EXPECT_EQ(coro::sync_wait(std::move(t)), 12);
    EXPECT_EQ(applied, 2);
}

TEST(Fmap, NamedConstTransformIsReusable) {
    applied = 0;
    const auto transform = coro::fmap([](int x) {
        ++applied;
        return x + x;
    });
    EXPECT_EQ(coro::sync_wait(val(3) | transform), 6);
    EXPECT_EQ(coro::sync_wait(val(4) | transform), 8);
    EXPECT_EQ(applied, 2);
}

TEST(Fmap, GeneratorMappingStaysPullDriven) {
    applied = 0;
    auto squares = numbers(1000)
        | coro::fmap([&](const long long x) {
            ++applied;
            return x * x;
        });

    EXPECT_EQ(applied, 0);
    EXPECT_EQ(coro::sync_wait(pull_four(squares)), 16);
    EXPECT_EQ(applied, 4);

    // Abandoning the mapped generator here destroys the source with it.
}

// fmap's factory deduces its parameter through a forwarding reference, so the
// stored type has to be decayed. Without that, a named lvalue deduces to a
// reference and the transform aliases the caller's object -- which dangles the
// moment the transform outlives it. These two pin the callable down as owned.
TEST(Fmap, TransformFromNamedLvalueOwnsTheCallable) {
    fn_copies = 0;
    CountedFn fn{100};
    auto transform = coro::fmap(fn);
    static_assert(!std::is_reference_v<decltype(transform.func)>);
    EXPECT_EQ(fn_copies, 1);
    EXPECT_NE(static_cast<const void*>(&transform.func), static_cast<const void*>(&fn));
    EXPECT_EQ(coro::sync_wait(val(5) | transform), 105);
}

TEST(Fmap, TransformFromConstLvalueOwnsTheCallable) {
    fn_copies = 0;
    const CountedFn fn{200};
    auto transform = coro::fmap(fn);
    static_assert(!std::is_reference_v<decltype(transform.func)>);
    EXPECT_EQ(fn_copies, 1);
    EXPECT_NE(static_cast<const void*>(&transform.func), static_cast<const void*>(&fn));
    EXPECT_EQ(coro::sync_wait(val(7) | transform), 207);
}

// The flip side: decaying the stored type must not cost move-only callables
// their way in.
TEST(Fmap, TransformFromRvalueMovesMoveOnlyCallable) {
    auto transform = coro::fmap(MoveOnlyFn{50});
    static_assert(std::is_same_v<decltype(transform.func), MoveOnlyFn>);
    EXPECT_EQ(coro::sync_wait(val(5) | std::move(transform)), 55);
}

// A callable returning nothing maps a task<int> to a task<> -- the compiler
// routes `co_return void-expression;` to return_void().
TEST(Fmap, VoidReturningCallableYieldsVoidTask) {
    reset_started();
    int seen = 0;
    auto t = val(7) | coro::fmap([&](const int x) { seen = x; });
    static_assert(std::is_same_v<decltype(t), coro::task<>>);
    coro::sync_wait(std::move(t));
    EXPECT_EQ(seen, 7);
    EXPECT_EQ(start_count(), 1);
}
