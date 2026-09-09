#include <gtest/gtest.h>

#include <algorithm>
#include <coroutine>
#include <vector>

#include <coro/sync/waiter_queue.h>

namespace {
    struct node final {
        node* next{};
        node* ready_next{};
        std::coroutine_handle<> h{};
        int id = 0;
    };

    using queue = coro::waiter_queue<node, &node::next>;

    // Parks the awaiting coroutine in a node, the way every user of these
    // queues does. Nothing resumes it but the drain.
    struct park final {
        node* n;

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(const std::coroutine_handle<> h) const noexcept {
            n->h = h;
        }

        void await_resume() const noexcept {
        }
    };

    // Fire and forget: the frame runs to its first suspend on creation and
    // frees itself when it completes, so the test owns nothing.
    struct detached final {
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

            void unhandled_exception() const noexcept {
            }
        };
    };
}

TEST(WaiterQueue, IsAFifoAndCountsItself) {
    queue q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
    EXPECT_EQ(q.pop(), nullptr);
    EXPECT_EQ(q.front(), nullptr);

    node a{.id = 1}, b{.id = 2}, c{.id = 3};
    q.push(&a);
    q.push(&b);
    q.push(&c);
    EXPECT_EQ(q.size(), 3u);
    EXPECT_EQ(q.front(), &a);
    EXPECT_FALSE(q.empty());

    EXPECT_EQ(q.pop()->id, 1);
    EXPECT_EQ(q.pop()->id, 2);
    EXPECT_EQ(q.pop()->id, 3);
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(WaiterQueue, PoppingClearsTheLinkSoANodeCanBeRequeued) {
    queue q;
    node a{.id = 1}, b{.id = 2};
    q.push(&a);
    q.push(&b);
    EXPECT_EQ(q.pop(), &a);
    EXPECT_EQ(a.next, nullptr);
    q.push(&a);
    EXPECT_EQ(q.pop(), &b);
    EXPECT_EQ(q.pop(), &a);
}

// A node in two queues at once is what the second link buys: taking it out
// of one must leave the other's ordering untouched.
TEST(WaiterQueue, TwoLinksKeepTwoQueuesIndependent) {
    queue by_next;
    coro::waiter_queue<node, &node::ready_next> by_ready;
    node a{.id = 1}, b{.id = 2};
    by_next.push(&a);
    by_next.push(&b);
    by_ready.push(&a);

    EXPECT_EQ(by_next.size(), 2u);
    EXPECT_EQ(by_ready.size(), 1u);
    EXPECT_EQ(by_ready.pop(), &a);
    EXPECT_EQ(by_next.pop(), &a);
    EXPECT_EQ(by_next.pop(), &b);
}

// The point of the drain: each resumed waiter resumes the next before it
// returns, which without queueing would nest four deep.
TEST(ResumeDrain, ChainedResumptionsRunInOrderAndNeverNest) {
    coro::resume_drain<node, &node::ready_next> drain;
    std::vector<node> nodes(4);
    for (int i = 0; i < 4; ++i) nodes[static_cast<std::size_t>(i)].id = i;

    std::vector<int> order;
    int depth = 0;
    int deepest = 0;

    auto body = [&](node& self, node* const next) -> detached {
        co_await park{&self};
        ++depth;
        deepest = std::max(deepest, depth);
        order.push_back(self.id);
        if (next != nullptr) drain.resume(next);
        --depth;
    };

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        body(nodes[i], i + 1 < nodes.size() ? &nodes[i + 1] : nullptr);
    }
    EXPECT_FALSE(drain.draining());

    drain.resume(&nodes[0]);

    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3}));
    EXPECT_EQ(deepest, 1);
    EXPECT_FALSE(drain.draining());
}
