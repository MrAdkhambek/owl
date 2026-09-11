#include <coroutine>
#include <gtest/gtest.h>
#include <type_traits>

#include <coro/concepts/awaitable.h>
#include <coro/task.h>

namespace {
    struct member_awaitable {
        struct awaiter {
            [[nodiscard]] bool await_ready() const noexcept { return true; }
            void await_suspend(std::coroutine_handle<>) const noexcept {}
            [[nodiscard]] int await_resume() const noexcept { return 1; }
        };

        awaiter operator co_await() const noexcept { return {}; }
    };

    struct bare_awaiter {
        [[nodiscard]] bool await_ready() const noexcept { return true; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        void await_resume() const noexcept {}
    };

    struct not_awaitable {};

    struct free_awaitable {};

    struct free_awaiter {
        [[nodiscard]] bool await_ready() const noexcept { return true; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        [[nodiscard]] int await_resume() const noexcept { return 2; }
    };

    auto operator co_await(free_awaitable) noexcept { return free_awaiter{}; }
}

// The standard allows three spellings of "awaitable"; get_awaiter has to
// normalise all three, so the concept must accept all three.
TEST(AwaitableConcept, AcceptsAllThreeSpellings) {
    static_assert(coro::awaitable<member_awaitable>);
    static_assert(coro::awaitable<bare_awaiter>);
    static_assert(coro::awaitable<free_awaitable>);
    static_assert(coro::awaitable<coro::task<int>>);
    static_assert(!coro::awaitable<not_awaitable>);
}

// An awaitable need not be an awaiter: member/free co_await produce one.
TEST(AwaitableConcept, AwaiterIsTheThreeMethods) {
    static_assert(coro::awaiter<bare_awaiter>);
    static_assert(coro::awaiter<free_awaiter>);
    static_assert(!coro::awaiter<member_awaitable>);
    static_assert(!coro::awaiter<free_awaitable>);
    static_assert(!coro::awaiter<not_awaitable>);
    static_assert(!coro::awaiter<coro::task<int>>);
}

TEST(AwaitableConcept, ResultTypeNamesWhatCoAwaitProduces) {
    static_assert(std::is_same_v<coro::await_result_t<member_awaitable>, int>);
    static_assert(std::is_same_v<coro::await_result_t<bare_awaiter>, void>);
    static_assert(std::is_same_v<coro::await_result_t<free_awaitable>, int>);
    static_assert(std::is_same_v<coro::await_result_t<coro::task<int>>, int>);
}

// await_value_t folds void onto void_value, because a result that has to be
// stored in a tuple slot or a variant alternative cannot be void.
TEST(AwaitableConcept, ValueTypeFoldsVoidOntoVoidStruct) {
    static_assert(std::is_same_v<coro::await_value_t<bare_awaiter>, coro::void_value>);
    static_assert(std::is_same_v<coro::await_value_t<member_awaitable>, int>);
}

// void_value is not std::monostate: two of them in one variant stay
// distinguishable, because variant indices are positional.
TEST(AwaitableConcept, VoidComparesEqualAndIsItsOwnType) {
    EXPECT_EQ(coro::void_value{}, coro::void_value{});
    static_assert(!std::is_same_v<coro::void_value, void>);
}
