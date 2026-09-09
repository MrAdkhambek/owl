#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/socket.h>
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

    // A write wait that times out is the psql connect_timeout against a
    // silent host: the driver then releases the fd. h2o has no way to cancel
    // a pending notify_write, so release must cope with one still armed.
    TEST(LoopReactor, WriteWaitTimeoutCanBeReleased) {
        h2o_loop_t* const loop = h2o_evloop_create();
        const owl::loop_reactor reactor{loop};
        int fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        ASSERT_EQ(::fcntl(fds[0], F_SETFL, O_NONBLOCK), 0);
        char junk[4096] = {};
        while (::write(fds[0], junk, sizeof junk) > 0) {
        }
        ASSERT_EQ(errno, EAGAIN);

        auto body = [&]() -> coro::task<coro::wait_status> {
            co_return co_await reactor.wait(fds[0], coro::interest::write, 20ms);
        };
        EXPECT_EQ(pump(loop, body()), coro::wait_status::timeout);

        reactor.release(fds[0]);
        ::close(fds[0]);
        ::close(fds[1]);
        h2o_evloop_run(loop, 0);
        h2o_evloop_destroy(loop);
    }

    // Descriptors this process holds open, for the leak guard below.
    int open_fd_count() {
        int open = 0;
        for (int fd = 0; fd < 512; ++fd) {
            if (::fcntl(fd, F_GETFD) != -1) ++open;
        }
        return open;
    }

    // The driver may close its descriptor before the connection is torn
    // down -- libpq drops its socket the moment the backend dies -- and the
    // number is then free for the next connection. Releasing must reach for
    // the reactor's own copy, never that number, or it deregisters and
    // closes whatever has taken it.
    TEST(LoopReactor, ReleaseAfterTheOwnerClosedTheDescriptor) {
        h2o_loop_t* const loop = h2o_evloop_create();
        const owl::loop_reactor reactor{loop};
        int fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

        auto body = [&]() -> coro::task<coro::wait_status> {
            co_return co_await reactor.wait(fds[0], coro::interest::read, 20ms);
        };
        EXPECT_EQ(pump(loop, body()), coro::wait_status::timeout);

        ::close(fds[0]);          // the owner closes first, the way libpq does
        reactor.release(fds[0]);  // and the reactor still has only its own to close

        // Whatever takes the freed number is watchable and still completes.
        int reused[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, reused), 0);
        EXPECT_EQ(::write(reused[1], "x", 1), 1);
        auto again = [&]() -> coro::task<coro::wait_status> {
            co_return co_await reactor.wait(reused[0], coro::interest::read, 2s);
        };
        EXPECT_EQ(pump(loop, again()), coro::wait_status::ready);

        reactor.release(reused[0]);
        ::close(reused[0]);
        ::close(reused[1]);
        ::close(fds[1]);
        h2o_evloop_destroy(loop);
    }

    // The reactor duplicates the descriptor it watches, so every release
    // has to close that duplicate; fifty cycles would otherwise show.
    TEST(LoopReactor, RepeatedWaitsAndReleasesLeakNoDescriptors) {
        h2o_loop_t* const loop = h2o_evloop_create();
        const owl::loop_reactor reactor{loop};
        int fds[2];
        ASSERT_EQ(::pipe(fds), 0);

        const int before = open_fd_count();
        for (int i = 0; i < 50; ++i) {
            auto body = [&]() -> coro::task<coro::wait_status> {
                co_return co_await reactor.wait(fds[0], coro::interest::read, 1ms);
            };
            EXPECT_EQ(pump(loop, body()), coro::wait_status::timeout);
            reactor.release(fds[0]);
        }
        EXPECT_EQ(open_fd_count(), before);

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
