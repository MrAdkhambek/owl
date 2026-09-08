#include <gtest/gtest.h>

#include <chrono>
#include <coroutine>
#include <thread>
#include <unistd.h>
#include <vector>

#include <coro/executors/static_thread_pool.h>
#include <coro/io/reactor.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <sql/io.h>

using namespace std::chrono_literals;

namespace {
    struct recording_reactor final {
        std::vector<int> released;
        coro::wait_status answer = coro::wait_status::ready;

        coro::task<coro::wait_status> wait(int, coro::interest, std::chrono::milliseconds) {
            co_return answer;
        }

        coro::task<coro::wait_status> sleep(std::chrono::milliseconds) {
            co_return answer;
        }

        void release(const int fd) {
            released.push_back(fd);
        }
    };

    struct plain_reactor final {
        coro::task<coro::wait_status> wait(int, coro::interest, std::chrono::milliseconds) {
            co_return coro::wait_status::timeout;
        }

        coro::task<coro::wait_status> sleep(std::chrono::milliseconds) {
            co_return coro::wait_status::timeout;
        }
    };

    static_assert(coro::io_reactor<recording_reactor>);
    static_assert(coro::io_reactor<plain_reactor>);
}

TEST(ReactorRef, NullAnswersErrorWithoutParking) {
    const sql::reactor_ref io;
    EXPECT_FALSE(io);
    EXPECT_EQ(io.wait(0, coro::interest::read, 1ms).get(), coro::wait_status::error);
    io.release(0);
}

TEST(ReactorRef, ForwardsWaitAndRelease) {
    recording_reactor r;
    const sql::reactor_ref io{r};
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
    const sql::reactor_ref io{r};
    io.release(5);
    EXPECT_EQ(io.wait(5, coro::interest::read, 1ms).get(), coro::wait_status::timeout);
}

TEST(ReactorRef, DrivesANativeReactor) {
    coro::native_reactor reactor;
    const sql::reactor_ref io{reactor};
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);

    EXPECT_EQ(coro::sync_wait(io.wait(fds[0], coro::interest::read, 20ms)), coro::wait_status::timeout);
    EXPECT_EQ(::write(fds[1], "x", 1), 1);
    EXPECT_EQ(coro::sync_wait(io.wait(fds[0], coro::interest::read, 2s)), coro::wait_status::ready);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(SchedulerRef, NullScheduleIsReadyAndPostResumesInline) {
    const sql::scheduler_ref s;
    EXPECT_FALSE(s);
    bool resumed = false;
    auto body = [&]() -> coro::task<> {
        co_await s.schedule();
        resumed = true;
    };
    auto t = body();
    t.start();
    EXPECT_TRUE(t.done());
    EXPECT_TRUE(resumed);
}

TEST(SchedulerRef, ScheduleHopsToThePool) {
    coro::static_thread_pool pool{1};
    const sql::scheduler_ref s{pool};
    EXPECT_TRUE(s);
    const auto main_id = std::this_thread::get_id();
    auto body = [&]() -> coro::task<bool> {
        co_await s.schedule();
        co_return std::this_thread::get_id() != main_id;
    };
    EXPECT_TRUE(coro::sync_wait(body()));
}

TEST(SchedulerRef, PostDeliversAHandle) {
    coro::static_thread_pool pool{1};
    const sql::scheduler_ref s{pool};
    struct manual final {
        sql::scheduler_ref s;
        bool await_ready() const noexcept { return false; }
        void await_suspend(const std::coroutine_handle<> h) const { s.post(h); }
        void await_resume() const noexcept {}
    };
    const auto main_id = std::this_thread::get_id();
    auto body = [&]() -> coro::task<bool> {
        co_await manual{s};
        co_return std::this_thread::get_id() != main_id;
    };
    EXPECT_TRUE(coro::sync_wait(body()));
}
