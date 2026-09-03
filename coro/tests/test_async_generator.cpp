#include <gtest/gtest.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <coro/async_generator.h>
#include <coro/executors/static_thread_pool.h>
#include <coro/task.h>
#include <coro/run/sync_wait.h>

namespace {

    // Collects the whole sequence; also the generic pull driver for other tests.
    template <typename T>
    coro::task<std::vector<T>> drain(coro::async_generator<T> gen) {
        std::vector<T> out;
        while (auto v = co_await gen.next()) {
            out.push_back(std::move(*v));
        }
        co_return out;
    }

    // Takes the generator BY REFERENCE: a by-value parameter would destroy
    // it -- and the suspended body frame -- when this helper finishes, before
    // the caller can observe anything about body lifetime.
    template <typename T>
    coro::task<int> pull_count(coro::async_generator<T>& gen, const int limit) {
        int count = 0;
        while (count < limit && static_cast<bool>(co_await gen.next())) {
            ++count;
        }
        co_return count;
    }

    // The body keeps `kept` across every suspension, so each pull must hand
    // the consumer a copy rather than a borrow.
    coro::async_generator<std::string> yields_kept_lvalue(const int n) {
        const std::string kept = "kept";
        for (int i = 0; i < n; ++i) {
            co_yield kept;
        }
    }

    // The idiom the const lvalue overload exists for: a range-for over someone
    // else's container, binding each element by const reference.
    coro::async_generator<std::string> echo(const std::vector<std::string>& in) {
        for (const auto& s : in) {
            co_yield s;
        }
    }

    coro::async_generator<int> first_n(const int n) {
        for (int i = 0; i < n; ++i) {
            co_yield i;
        }
    }

    // Named generator taking its instrumentation by reference -- never a lambda
    // coroutine under sanitizers.
    coro::async_generator<int> counting_infinite(int* runs) {
        ++*runs;
        for (;;) {
            co_yield 1;
        }
    }

    coro::async_generator<std::unique_ptr<int>> move_only_elements() {
        for (int i = 0; i < 3; ++i) {
            co_yield std::make_unique<int>(i * 11);
        }
    }

    coro::async_generator<int> throws_after_first() {
        co_yield 1;
        throw std::runtime_error("body exploded");
    }

    // RAII marker: proves abandoning a half-consumed generator still runs body
    // locals' destructors.
    struct Guard {
        int* live;

        explicit Guard(int* l) : live(l) {
            ++*live;
        }

        ~Guard() {
            --*live;
        }
    };

    coro::async_generator<int> guarded(int* live) {
        Guard marker{live};
        for (;;) {
            co_yield 1;
        }
    }

    coro::task<bool> pulls_then_throws_then_ends() {
        coro::async_generator<int> gen = throws_after_first();
        const bool first_ok = static_cast<bool>(co_await gen.next());

        // co_await is illegal inside a catch handler, so the catch only flags;
        // the follow-up pull happens after.
        bool caught = false;
        try {
            static_cast<void>(co_await gen.next());
        } catch (const std::runtime_error&) {
            caught = true;
        }
        if (!caught) co_return false;

        const bool after_throw_has_value = static_cast<bool>(co_await gen.next());
        co_return first_ok && !after_throw_has_value;
    }

    // The whole point of an ASYNC generator: a body that suspends on something
    // else between elements. Every element crosses a thread boundary here.
    coro::async_generator<int> hopping(coro::static_thread_pool& pool, const int n) {
        for (int i = 0; i < n; ++i) {
            co_await pool.schedule();
            co_yield i;
        }
    }

    coro::async_generator<int> hopping_then_throws(coro::static_thread_pool& pool) {
        co_await pool.schedule();
        co_yield 1;
        co_await pool.schedule();
        throw std::runtime_error("body exploded off-thread");
    }

    // Drains a generator and records which thread each element was delivered on.
    coro::task<std::vector<int>> drain_recording(coro::async_generator<int> gen,
                                                 std::set<std::thread::id>& seen) {
        std::vector<int> out;
        while (auto v = co_await gen.next()) {
            seen.insert(std::this_thread::get_id());
            out.push_back(*v);
        }
        co_return out;
    }

    coro::task<bool> hopping_exception_reaches_consumer(coro::static_thread_pool& pool) {
        coro::async_generator<int> gen = hopping_then_throws(pool);
        const bool first_ok = static_cast<bool>(co_await gen.next());

        bool caught = false;
        try {
            static_cast<void>(co_await gen.next());
        } catch (const std::runtime_error&) {
            caught = true;
        }
        co_return first_ok && caught;
    }

    coro::task<long long> sum_move_only(coro::async_generator<std::unique_ptr<int>> gen) {
        long long total = 0;
        while (auto v = co_await gen.next()) {
            total += **v;
        }
        co_return total;
    }
}

TEST(AsyncGenerator, YieldsInOrderThenEnds) {
    const auto values = coro::sync_wait(drain(first_n(5)));
    EXPECT_EQ(values, (std::vector{0, 1, 2, 3, 4}));
}

TEST(AsyncGenerator, BodyDoesNotRunUntilFirstPull) {
    int runs = 0;
    auto gen = counting_infinite(&runs);
    EXPECT_EQ(runs, 0);
    EXPECT_EQ(coro::sync_wait(pull_count(gen, 3)), 3);
    EXPECT_EQ(runs, 1);
}

TEST(AsyncGenerator, AbandonedMidSequenceRunsBodyDestructors) {
    int live = 0;
    {
        auto gen = guarded(&live);
        EXPECT_EQ(coro::sync_wait(pull_count(gen, 2)), 2);
        EXPECT_EQ(live, 1);
    }
    EXPECT_EQ(live, 0);
}

TEST(AsyncGenerator, ExceptionSurfacesOnceThenSequenceEnds) {
    EXPECT_TRUE(coro::sync_wait(pulls_then_throws_then_ends()));
}

TEST(AsyncGenerator, MoveOnlyElementsFlowThrough) {
    EXPECT_EQ(coro::sync_wait(sum_move_only(move_only_elements())), 33);
}

TEST(AsyncGenerator, DefaultConstructedReportsEndImmediately) {
    coro::async_generator<int> empty{};
    const auto values = coro::sync_wait(drain(std::move(empty)));
    EXPECT_TRUE(values.empty());
}

// A const lvalue is still an element the body keeps, so the consumer gets its
// own copy. This is the overload that a plain `T&` yield_value could not bind.
TEST(AsyncGenerator, ConstLvalueYieldCopiesToConsumer) {
    const auto values = coro::sync_wait(drain(yields_kept_lvalue(3)));
    EXPECT_EQ(values, (std::vector<std::string>{"kept", "kept", "kept"}));
}

// Yielding through a const reference must leave the source alone -- the body
// does not own those elements and cannot consent to moving them out.
TEST(AsyncGenerator, RangeForOverConstReferenceCopiesElements) {
    const std::vector<std::string> source{"alpha", "beta", "gamma"};
    const auto values = coro::sync_wait(drain(echo(source)));
    EXPECT_EQ(values, source);
    EXPECT_EQ(source, (std::vector<std::string>{"alpha", "beta", "gamma"}));
}

// A generator body that awaits a scheduler between elements is the one thing
// separating async_generator from an ordinary generator, and nothing else in
// the suite exercises it. Order must survive the hops.
TEST(AsyncGenerator, HoppingBodyDeliversEveryElementInOrder) {
    coro::static_thread_pool pool{4};
    std::set<std::thread::id> seen;
    const auto values = coro::sync_wait(drain_recording(hopping(pool, 8), seen));
    EXPECT_EQ(values, (std::vector{0, 1, 2, 3, 4, 5, 6, 7}));
}

// Each element is handed over from whichever worker resumed the body, so the
// consumer migrates with it and never runs on the thread that started the pull.
TEST(AsyncGenerator, HoppingBodyResumesConsumerOnWorkers) {
    const auto caller = std::this_thread::get_id();
    coro::static_thread_pool pool{4};
    std::set<std::thread::id> seen;
    static_cast<void>(coro::sync_wait(drain_recording(hopping(pool, 8), seen)));

    EXPECT_FALSE(seen.empty());
    EXPECT_EQ(seen.count(caller), 0u) << "consumer ran on the calling thread";
    EXPECT_GT(seen.size(), 1u) << "elements never actually changed threads";
}

TEST(AsyncGenerator, ExceptionFromHoppingBodyReachesConsumer) {
    coro::static_thread_pool pool{4};
    EXPECT_TRUE(coro::sync_wait(hopping_exception_reaches_consumer(pool)));
}
