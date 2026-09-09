#include <chrono>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include <coro/io/reactor.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

using namespace std::chrono_literals;

namespace {
    // A pipe gives a readable fd on demand: nothing is readable until
    // something is written, which is exactly the edge the reactor parks on.
    class pipe_pair {
    public:
        pipe_pair() { EXPECT_EQ(::pipe(fds_), 0); }

        ~pipe_pair() {
            if (fds_[0] >= 0) ::close(fds_[0]);
            if (fds_[1] >= 0) ::close(fds_[1]);
        }

        pipe_pair(const pipe_pair&) = delete;
        pipe_pair& operator=(const pipe_pair&) = delete;

        [[nodiscard]] int read_end() const noexcept { return fds_[0]; }

        void write_byte() const { EXPECT_EQ(::write(fds_[1], "x", 1), 1); }

    private:
        int fds_[2]{-1, -1};
    };

    // A connected AF_UNIX pair: full duplex, so one end can carry a read
    // wait and a write wait at the same time.
    class socket_pair {
    public:
        socket_pair() { EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_), 0); }

        ~socket_pair() {
            if (fds_[0] >= 0) ::close(fds_[0]);
            if (fds_[1] >= 0) ::close(fds_[1]);
        }

        socket_pair(const socket_pair&) = delete;
        socket_pair& operator=(const socket_pair&) = delete;

        [[nodiscard]] int near() const noexcept { return fds_[0]; }

        void write_from_far() const { EXPECT_EQ(::write(fds_[1], "x", 1), 1); }

    private:
        int fds_[2]{-1, -1};
    };

    // An fd number the kernel will refuse: it was a pipe end, and is closed.
    int closed_fd() {
        int fds[2]{-1, -1};
        EXPECT_EQ(::pipe(fds), 0);
        ::close(fds[0]);
        ::close(fds[1]);
        return fds[0];
    }

    coro::task<coro::wait_status> wait_on(coro::native_reactor& reactor, const int fd,
                                          const coro::interest want, const std::chrono::milliseconds timeout) {
        co_return co_await reactor.wait(fd, want, timeout);
    }
}

TEST(Reactor, SleepReturnsTimeoutAfterTheDuration) {
    coro::native_reactor reactor;

    auto body = [&reactor]() -> coro::task<coro::wait_status> {
        co_return co_await reactor.sleep(30ms);
    };

    const auto start = std::chrono::steady_clock::now();
    const auto status = coro::sync_wait(body());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(status, coro::wait_status::timeout);
    EXPECT_GE(elapsed, 25ms);
}

TEST(Reactor, WaitTimesOutWhenNothingArrives) {
    coro::native_reactor reactor;
    const pipe_pair pipes;

    auto body = [&reactor, &pipes]() -> coro::task<coro::wait_status> {
        co_return co_await reactor.wait(pipes.read_end(), coro::interest::read, 30ms);
    };

    EXPECT_EQ(coro::sync_wait(body()), coro::wait_status::timeout);
}

TEST(Reactor, WaitBecomesReadyWhenTheFdIsWritten) {
    coro::native_reactor reactor;
    const pipe_pair pipes;

    auto body = [&reactor, &pipes]() -> coro::task<coro::wait_status> {
        // Make the fd readable before parking, so the test does not depend
        // on a race between this thread and the reactor's.
        pipes.write_byte();
        co_return co_await reactor.wait(pipes.read_end(), coro::interest::read, 2s);
    };

    EXPECT_EQ(coro::sync_wait(body()), coro::wait_status::ready);
}

// A negative timeout means "forever"; combined with an fd that becomes
// readable, that must still resolve.
TEST(Reactor, NegativeTimeoutWaitsIndefinitelyButStillWakes) {
    coro::native_reactor reactor;
    const pipe_pair pipes;

    auto body = [&reactor, &pipes]() -> coro::task<coro::wait_status> {
        pipes.write_byte();
        co_return co_await reactor.wait(pipes.read_end(), coro::interest::read, -1ms);
    };

    EXPECT_EQ(coro::sync_wait(body()), coro::wait_status::ready);
}

TEST(Reactor, SatisfiesItsOwnConcept) {
    static_assert(coro::io_reactor<coro::native_reactor>);
}

// A descriptor the kernel refuses to register -- closed, so EBADF -- must
// come back as error rather than leave the coroutine parked on nothing. The
// `fd < 0` guard cannot catch it: the number is fine, the descriptor is not.
TEST(Reactor, RegistrationFailureReportsErrorInsteadOfParkingForever) {
    coro::native_reactor reactor;
    EXPECT_EQ(coro::sync_wait(wait_on(reactor, closed_fd(), coro::interest::read, -1ms)),
              coro::wait_status::error);
}

// The same with a deadline. The timer is registered in the same batch as
// the fd, after it; a batch that stops at the fd's failure never arms the
// timer either, so this used to park forever as well.
TEST(Reactor, RegistrationFailureWithADeadlineStillReportsError) {
    coro::native_reactor reactor;
    EXPECT_EQ(coro::sync_wait(wait_on(reactor, closed_fd(), coro::interest::read, 200ms)),
              coro::wait_status::error);
}

// One descriptor, two directions. A write-readiness wait that completes must
// take down only its own registration: the read wait still parked on the
// same fd has to survive it, and wake when data arrives.
TEST(Reactor, CompletingAWriteWaitLeavesAReadWaitOnTheSameFdArmed) {
    coro::native_reactor reactor;
    const socket_pair sockets;

    coro::wait_status reader_saw = coro::wait_status::error;
    std::thread reader([&] {
        reader_saw = coro::sync_wait(wait_on(reactor, sockets.near(), coro::interest::read, 2s));
    });
    // Let the reactor thread arm the read wait before the write wait arrives.
    std::this_thread::sleep_for(30ms);

    // A fresh socket is writable at once.
    EXPECT_EQ(coro::sync_wait(wait_on(reactor, sockets.near(), coro::interest::write, 2s)),
              coro::wait_status::ready);

    sockets.write_from_far();
    reader.join();
    EXPECT_EQ(reader_saw, coro::wait_status::ready) << "the read wait was unregistered by the write wait's completion";
}

// A timed wait that completes on readiness must take its timer with it.
// kqueue processes a batch of deletes only up to the first one that fails --
// here the oneshot read filter, already gone because it fired -- so a naive
// batch leaves the timer armed against a request that no longer exists. The
// next wait() frame is the same size and is allocated the moment the last
// one is freed, so its request lands on that same address and is woken by
// the stale timer with a timeout it never asked for.
TEST(Reactor, TimerOfAReadyWaitDoesNotFireIntoALaterWait) {
    coro::native_reactor reactor;
    const pipe_pair first;
    const pipe_pair later;

    auto body = [&]() -> coro::task<std::pair<coro::wait_status, coro::wait_status>> {
        first.write_byte();
        const auto ready = co_await reactor.wait(first.read_end(), coro::interest::read, 100ms);
        const auto untimed = co_await reactor.wait(later.read_end(), coro::interest::read, -1ms);
        co_return std::pair{ready, untimed};
    };

    std::thread writer([&later] {
        std::this_thread::sleep_for(300ms);
        later.write_byte();
    });
    const auto start = std::chrono::steady_clock::now();
    const auto [ready, untimed] = coro::sync_wait(body());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    writer.join();

    EXPECT_EQ(ready, coro::wait_status::ready);
    EXPECT_EQ(untimed, coro::wait_status::ready) << "the stale timer fired into the next wait";
    EXPECT_GE(elapsed, 250ms) << "an untimed wait ended early";
}

// A wait with no deadline on something that never becomes readable would
// park forever; cancel is how a caller ends it. The descriptor is left
// open -- only its owner closes it -- and the waiter learns why it woke.
TEST(Reactor, CancelEndsAParkedWaitAndLeavesTheFdOpen) {
    coro::native_reactor reactor;
    const pipe_pair pipe;

    std::thread stopper([&] {
        std::this_thread::sleep_for(50ms);
        reactor.cancel(pipe.read_end());
    });
    const auto status = coro::sync_wait([&]() -> coro::task<coro::wait_status> {
        co_return co_await reactor.wait(pipe.read_end(), coro::interest::read, -1ms);
    }());
    stopper.join();

    EXPECT_EQ(status, coro::wait_status::cancelled);
    // Still the caller's descriptor, still usable.
    pipe.write_byte();
    EXPECT_EQ(coro::sync_wait([&]() -> coro::task<coro::wait_status> {
                  co_return co_await reactor.wait(pipe.read_end(), coro::interest::read, 2s);
              }()),
              coro::wait_status::ready);
}

// A cancel names the waits that exist when it is called. One issued with
// nothing parked has nothing to do, and must not be held against the next
// wait on that descriptor -- the loop makes the race, if there were one,
// overwhelmingly likely to be hit.
TEST(Reactor, ACancelDoesNotFallOnALaterWaitOnTheSameFd) {
    coro::native_reactor reactor;
    const pipe_pair pipe;
    for (int i = 0; i < 40; ++i) {
        reactor.cancel(pipe.read_end());
        pipe.write_byte();
        EXPECT_EQ(coro::sync_wait([&]() -> coro::task<coro::wait_status> {
                      co_return co_await reactor.wait(pipe.read_end(), coro::interest::read, 2s);
                  }()),
                  coro::wait_status::ready);
        char drained = 0;
        EXPECT_EQ(::read(pipe.read_end(), &drained, 1), 1);
    }
}

TEST(Reactor, CancelOfAnIdleDescriptorIsANoOp) {
    coro::native_reactor reactor;
    const pipe_pair pipe;
    reactor.cancel(pipe.read_end());
    reactor.cancel(-1);
    pipe.write_byte();
    EXPECT_EQ(coro::sync_wait([&]() -> coro::task<coro::wait_status> {
                  co_return co_await reactor.wait(pipe.read_end(), coro::interest::read, 2s);
              }()),
              coro::wait_status::ready);
}
