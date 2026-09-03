#include <chrono>
#include <gtest/gtest.h>
#include <unistd.h>

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
