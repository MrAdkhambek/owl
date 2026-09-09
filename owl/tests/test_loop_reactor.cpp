#include <gtest/gtest.h>

#include <chrono>
#include <unistd.h>

#include <coro/run/sync_wait.h>

#include <owl/coro/loop_reactor.h>

using namespace std::chrono_literals;

namespace {
    // Drives the task on the loop and hands back what it answered: a wait
    // that finished without a status (a body that fell off its end) throws
    // here, where the test can see it, rather than going unnoticed.
    coro::wait_status pump(h2o_loop_t* const loop, coro::task<coro::wait_status>&& work) {
        work.start();
        for (int i = 0; i < 2000 && !work.done(); ++i) {
            h2o_evloop_run(loop, 5);
        }
        EXPECT_TRUE(work.done());
        if (!work.done()) return coro::wait_status::error;
        return work.result();
    }

    TEST(LoopReactor, SleepAnswersTimeoutAfterTheDuration) {
        h2o_loop_t* const loop = h2o_evloop_create();
        const owl::loop_reactor reactor{loop};

        auto body = [&]() -> coro::task<coro::wait_status> {
            co_return co_await reactor.sleep(30ms);
        };
        EXPECT_EQ(pump(loop, body()), coro::wait_status::timeout);

        h2o_evloop_destroy(loop);
    }

    TEST(LoopReactor, WaitBecomesReadyWhenTheFdIsReadable) {
        h2o_loop_t* const loop = h2o_evloop_create();
        const owl::loop_reactor reactor{loop};
        int fds[2];
        ASSERT_EQ(::pipe(fds), 0);
        EXPECT_EQ(::write(fds[1], "x", 1), 1);

        auto body = [&]() -> coro::task<coro::wait_status> {
            co_return co_await reactor.wait(fds[0], coro::interest::read, 2s);
        };
        EXPECT_EQ(pump(loop, body()), coro::wait_status::ready);

        reactor.release(fds[0]);  // detach the h2o socket before the fd dies
        ::close(fds[0]);
        ::close(fds[1]);
        h2o_evloop_destroy(loop);
    }

    TEST(LoopReactor, WaitTimesOutOnAQuietFd) {
        h2o_loop_t* const loop = h2o_evloop_create();
        const owl::loop_reactor reactor{loop};
        int fds[2];
        ASSERT_EQ(::pipe(fds), 0);

        auto body = [&]() -> coro::task<coro::wait_status> {
            co_return co_await reactor.wait(fds[0], coro::interest::read, 20ms);
        };
        EXPECT_EQ(pump(loop, body()), coro::wait_status::timeout);

        reactor.release(fds[0]);  // detach the h2o socket before the fd dies
        ::close(fds[0]);
        ::close(fds[1]);
        h2o_evloop_destroy(loop);
    }

    TEST(LoopReactor, ReleaseSurvivesTheFd) {
        h2o_loop_t* const loop = h2o_evloop_create();
        const owl::loop_reactor reactor{loop};
        int fds[2];
        ASSERT_EQ(::pipe(fds), 0);

        // Wrap the fd by waiting on it, then release: the fd must remain
        // owned by the caller (PQfinish's job later).
        auto body = [&]() -> coro::task<coro::wait_status> {
            co_return co_await reactor.wait(fds[0], coro::interest::read, 20ms);
        };
        EXPECT_EQ(pump(loop, body()), coro::wait_status::timeout);
        reactor.release(fds[0]);

        char buf[1] = {};
        EXPECT_EQ(::write(fds[1], "x", 1), 1);
        EXPECT_EQ(::read(fds[0], buf, 1), 1);

        reactor.release(fds[0]);  // detach the h2o socket before the fd dies
        ::close(fds[0]);
        ::close(fds[1]);
        h2o_evloop_destroy(loop);
    }

    TEST(LoopReactor, NullReactorAnswersErrorWithoutParking) {
        const owl::loop_reactor reactor;

        EXPECT_EQ(coro::sync_wait(reactor.wait(0, coro::interest::read, 1ms)), coro::wait_status::error);
        EXPECT_EQ(coro::sync_wait(reactor.sleep(1ms)), coro::wait_status::error);
    }

    TEST(LoopReactor, SatisfiesTheConcept) {
        static_assert(coro::io_reactor<owl::loop_reactor>);
    }
}
