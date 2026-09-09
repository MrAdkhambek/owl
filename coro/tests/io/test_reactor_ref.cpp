#include <gtest/gtest.h>

#include <chrono>
#include <unistd.h>
#include <vector>

#include <coro/io/reactor.h>
#include <coro/io/reactor_ref.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

using namespace std::chrono_literals;

namespace {
    struct recording_reactor final {
        mutable std::vector<int> released;
        coro::wait_status answer = coro::wait_status::ready;

        coro::task<coro::wait_status> wait(int, coro::interest, std::chrono::milliseconds) const {
            co_return answer;
        }

        coro::task<coro::wait_status> sleep(std::chrono::milliseconds) const {
            co_return answer;
        }

        void release(const int fd) const {
            released.push_back(fd);
        }
    };

    struct plain_reactor final {
        coro::task<coro::wait_status> wait(int, coro::interest, std::chrono::milliseconds) const {
            co_return coro::wait_status::timeout;
        }

        coro::task<coro::wait_status> sleep(std::chrono::milliseconds) const {
            co_return coro::wait_status::timeout;
        }
    };

    static_assert(coro::io_reactor<recording_reactor>);
    static_assert(coro::io_reactor<plain_reactor>);
}

TEST(ReactorRef, NullAnswersErrorWithoutParking) {
    const coro::reactor_ref io;
    EXPECT_FALSE(io);
    EXPECT_EQ(io.wait(0, coro::interest::read, 1ms).get(), coro::wait_status::error);
    io.release(0);
}

TEST(ReactorRef, ForwardsWaitAndRelease) {
    recording_reactor r;
    const coro::reactor_ref io{r};
    EXPECT_TRUE(io);
    EXPECT_EQ(io.wait(3, coro::interest::write, 1ms).get(), coro::wait_status::ready);
    r.answer = coro::wait_status::timeout;
    EXPECT_EQ(io.wait(3, coro::interest::write, 1ms).get(), coro::wait_status::timeout);
    io.release(7);
    io.release(8);
    EXPECT_EQ(r.released, (std::vector<int>{7, 8}));
}

TEST(ReactorRef, ReleaseIsANoOpForAReactorWithoutOne) {
    plain_reactor r;
    const coro::reactor_ref io{r};
    io.release(5);
    EXPECT_EQ(io.wait(5, coro::interest::read, 1ms).get(), coro::wait_status::timeout);
}

TEST(ReactorRef, DrivesANativeReactor) {
    coro::native_reactor reactor;
    const coro::reactor_ref io{reactor};
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    EXPECT_EQ(coro::sync_wait(io.wait(fds[0], coro::interest::read, 20ms)), coro::wait_status::timeout);
    EXPECT_EQ(::write(fds[1], "x", 1), 1);
    EXPECT_EQ(coro::sync_wait(io.wait(fds[0], coro::interest::read, 2s)), coro::wait_status::ready);

    ::close(fds[0]);
    ::close(fds[1]);
}
