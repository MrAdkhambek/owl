#include <coroutine>
#include <exception>
#include <expected>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <coro/concepts/awaitable.h>
#include <coro/task.h>
#include <coro/result/as_result.h>
#include <coro/result/result.h>
#include <coro/run/sync_wait.h>
#include <coro/algo/wait_all.h>
#include "support/helpers.h"

using namespace coro_test;

namespace {
    // The void counterpart of boom(): proves the void overload catches too.
    coro::task<> boom_void() {
        started.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("void boom");
        co_return;
    }
}

// The outcome type has its own spelled-out name at namespace scope, and
// as_result is typed in exactly those terms. The alias's requires-clause is
// what rejects reference payloads, which is why as_result carries no check
// of its own.
TEST(as_result_t, ResultAliasTypesTheOutcome) {
    static_assert(std::is_same_v<coro::result<int>, std::expected<int, std::exception_ptr>>);
    static_assert(std::is_same_v<coro::result<void>, std::expected<void, std::exception_ptr>>);
    static_assert(std::is_same_v<decltype(coro::as_result(val(1))), coro::task<coro::result<int>>>);
    static_assert(std::is_same_v<decltype(coro::as_result(quiet())), coro::task<coro::result<void>>>);
    SUCCEED();
}

// Success keeps the value and engages the expected.
TEST(as_result_t, ValueTaskYieldsEngagedExpected) {
    reset_started();
    const auto r = coro::sync_wait(coro::as_result(val(7)));
    static_assert(std::is_same_v<decltype(r), const coro::result<int>>);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 7);
    EXPECT_EQ(start_count(), 1) << "the body did not run";
}

// A void body maps to expected<void, E>: engaged on success, no payload.
TEST(as_result_t, VoidTaskYieldsEngagedVoidExpected) {
    reset_started();
    const auto r = coro::sync_wait(coro::as_result(quiet()));
    static_assert(std::is_same_v<decltype(r), const coro::result<void>>);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(start_count(), 1);
}

// Failure lands in the error slot as an exception_ptr; rethrowing it gives
// back the original type and message.
TEST(as_result_t, ThrowingTaskYieldsDisengagedExpected) {
    reset_started();
    const auto r = coro::sync_wait(coro::as_result(boom("boom")));
    ASSERT_FALSE(r.has_value());
    ASSERT_NE(r.error(), nullptr);

    try {
        std::rethrow_exception(r.error());
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "boom");
    } catch (...) {
        FAIL() << "the exception did not survive as a runtime_error";
    }
    EXPECT_EQ(start_count(), 1) << "the throwing body did not run";
}

// Same protocol for a void body that throws.
TEST(as_result_t, VoidThrowingTaskYieldsError) {
    const auto r = coro::sync_wait(coro::as_result(boom_void()));
    ASSERT_FALSE(r.has_value());
    try {
        std::rethrow_exception(r.error());
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "void boom");
    }
}

// Lazy like every combinator: building the wrapper runs nothing.
TEST(as_result_t, NothingRunsUntilAwaited) {
    reset_started();
    auto t = coro::as_result(val(1));
    EXPECT_EQ(start_count(), 0) << "the source ran before its await";
    static_cast<void>(coro::sync_wait(std::move(t)));
    EXPECT_EQ(start_count(), 1);
}

// The point of the combinator: under wait_all, one child's failure becomes an
// ordinary value instead of unwinding the group -- both outcomes survive.
TEST(as_result_t, FailuresSurviveInsideWaitAll) {
    reset_started();
    auto [a, b] = coro::sync_wait(coro::wait_all(
        coro::as_result(boom("first")), val(2)));

    ASSERT_FALSE(a.has_value());
    try {
        std::rethrow_exception(a.error());
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "first");
    }
    EXPECT_EQ(b, 2);
    // Both children ran even though the first argument failed.
    EXPECT_EQ(start_count(), 2);
}

// The partly-applied form pipes, and consumes an rvalue source exactly as the
// call form does.
TEST(as_result_t, PipeFormMatchesCallForm) {
    const auto ok = coro::sync_wait(val(5) | coro::as_result());
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 5);

    const auto bad = coro::sync_wait(boom("piped") | coro::as_result());
    ASSERT_FALSE(bad.has_value());
    try {
        std::rethrow_exception(bad.error());
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "piped");
    }
}

// coro::as_result has two overloads with different return shapes: the
// task one allocates a frame and yields task<result<T>>, the awaitable
// one is a zero-allocation adapter yielding as_result_t<Aw>. They used to
// live in different headers, so which one you got depended on your
// include set. Including as_result.h alone must now give you both.
TEST(as_result_t, BothOverloadsLiveInOneHeader) {
    // Partial ordering picks the task overload for a task.
    static_assert(std::is_same_v<
                      decltype(coro::as_result(std::declval<coro::task<int>>())),
                      coro::task<coro::result<int>>>,
                  "a task must select the frame-allocating overload");

    // A non-task awaitable selects the adapter.
    struct ready_awaiter {
        [[nodiscard]] bool await_ready() const noexcept { return true; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        [[nodiscard]] int await_resume() const noexcept { return 7; }
    };
    static_assert(coro::awaitable<ready_awaiter>);
    static_assert(std::is_same_v<
                      decltype(coro::as_result(std::declval<ready_awaiter>())),
                      coro::as_result_t<ready_awaiter>>,
                  "a bare awaiter must select the zero-allocation adapter");
}

TEST(as_result_t, AwaitableOverloadCarriesValueThrough) {
    struct ready_awaiter {
        [[nodiscard]] bool await_ready() const noexcept { return true; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        [[nodiscard]] int await_resume() const noexcept { return 7; }
    };

    auto body = []() -> coro::task<int> {
        auto r = co_await coro::as_result(ready_awaiter{});
        co_return r.value();
    };

    EXPECT_EQ(coro::sync_wait(body()), 7);
}

TEST(as_result_t, AwaitableOverloadCapturesThrow) {
    struct throwing_awaiter {
        [[nodiscard]] bool await_ready() const noexcept { return true; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        [[noreturn]] int await_resume() const { throw std::runtime_error("boom"); }
    };

    auto body = []() -> coro::task<bool> {
        auto r = co_await coro::as_result(throwing_awaiter{});
        co_return r.has_value();
    };

    EXPECT_FALSE(coro::sync_wait(body()));
}
