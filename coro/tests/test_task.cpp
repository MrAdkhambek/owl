#include <gtest/gtest.h>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#include <coro/task.h>
#include <coro/algo/fmap.h>
#include <coro/run/sync_wait.h>
#include <coro/algo/wait_all.h>
#include "support/helpers.h"

using namespace coro_test;

namespace {
    // A failed co_return cannot be probed directly, so this asks the promise
    // base the same question the compiler does.
    template <typename T, typename From>
    concept can_co_return = requires(coro::detail::returns<T>& promise, From&& from) {
        promise.return_value(std::forward<From>(from));
    };

    coro::task<bool> ref_alias_check(int& slot) {
        int& got = co_await give_ref(slot);
        co_return &got == &slot;
    }

    // Converting co_return: an int flows into a task<long long>.
    coro::task<long long> widen(const int v) {
        co_return v;
    }

    // Deep await chain: exercises symmetric transfer staying flat on the
    // stack. The depth has to be large enough that a non-symmetric
    // implementation would actually overflow -- a few hundred frames would
    // pass either way and prove nothing.
    coro::task<int> chain(const int depth) {
        if (depth == 0) co_return 7;
        co_return co_await chain(depth - 1);
    }

    coro::task<> await_same_task_twice() {
        auto t = val(1);
        static_cast<void>(co_await std::move(t));
        static_cast<void>(co_await std::move(t));
    }

    coro::task<> await_empty_task() {
        coro::task<> gone{};
        static_cast<void>(co_await std::move(gone));
    }
}

TEST(Task, DeliversValue) {
    reset_started();
    EXPECT_EQ(coro::sync_wait(val(42)), 42);
    EXPECT_EQ(start_count(), 1);
}

TEST(Task, VoidBodyCompletes) {
    reset_started();
    coro::sync_wait(quiet());
    EXPECT_EQ(start_count(), 1);
}

TEST(Task, RoutesExceptionsToAwaiter) {
    reset_started();
    EXPECT_THROW(static_cast<void>(coro::sync_wait(boom())), std::runtime_error);
}

TEST(Task, ReferenceResultAliasesSourceSlot) {
    int slot = 5;
    EXPECT_TRUE(coro::sync_wait(ref_alias_check(slot)));
}

TEST(Task, CoReturnConvertsArgumentType) {
    EXPECT_EQ(coro::sync_wait(widen(9)), 9L);
}

TEST(Task, DeepAwaitChainStaysFlat) {
    EXPECT_EQ(coro::sync_wait(chain(100'000)), 7);
}

TEST(Task, SecondAwaitThrowsLogicError) {
    reset_started();
    EXPECT_THROW(static_cast<void>(coro::sync_wait(await_same_task_twice())),
                 std::logic_error);
}

TEST(Task, EmptyTaskAwaitThrowsLogicError) {
    EXPECT_THROW(static_cast<void>(coro::sync_wait(await_empty_task())),
                 std::logic_error);
}

TEST(Task, ComposesWithWaitAllAndFmap) {
    reset_started();
    auto [a, b] = coro::sync_wait(coro::wait_all(
        val(2) | coro::fmap([](int x) {
            return x * 10;
        }),
        val(3)));
    EXPECT_EQ(a, 20);
    EXPECT_EQ(b, 3);
    EXPECT_EQ(start_count(), 2);
}

// A task<T&> stores the address of what the body co_returned, so binding that
// to a temporary would dangle the moment the co_return statement ends. Value
// results are unaffected -- they are moved into the slot, not pointed at.
TEST(Task, ReferenceResultRejectsTemporaries) {
    static_assert(can_co_return<int, int>, "a value result accepts a prvalue");
    static_assert(can_co_return<long long, int>, "a value result accepts a converted prvalue");
    static_assert(can_co_return<int&, int&>, "a reference result accepts an lvalue");
    static_assert(can_co_return<const int&, const int&>, "a const reference result accepts an lvalue");

    static_assert(!can_co_return<const int&, int>,
                  "a reference result must reject a prvalue: the temporary dies too early");
    static_assert(!can_co_return<int&, int>,
                  "likewise for a non-const reference result");
    SUCCEED();
}
