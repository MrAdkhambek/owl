#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <unistd.h>

#include <coro/io/reactor.h>
#include <coro/run/sync_wait.h>

#include <sql/reactor.h>
#include <sql/resume.h>

using namespace std::chrono_literals;

namespace {
    struct counting_post final {
        std::atomic<int> posts{0};

        std::suspend_never schedule() const noexcept {
            return {};
        }

        void post(const std::coroutine_handle<> h) {
            posts.fetch_add(1);
            h.resume();
        }
    };

    TEST(Resumer, ErasesPostableAndDefaultsToInline) {
        counting_post p;
        const sql::resumer wired{&p};
        const sql::resumer empty{};

        EXPECT_TRUE(static_cast<bool>(wired));
        EXPECT_FALSE(static_cast<bool>(empty));
        EXPECT_EQ(p.posts.load(), 0);

        // An empty resumer resumes inline: posting a finished handle runs
        // it right here. Full delivery is proven by test_sqlite's affinity
        // tests -- resuming a live parked coroutine from the reactor thread.
    }

    TEST(AnyReactor, DefaultOwnsANativeReactor) {
        const sql::any_reactor reactor;

        EXPECT_EQ(coro::sync_wait(reactor.sleep(1ms)), coro::wait_status::timeout);
        // release is a no-op for reactors without the hook.
        reactor.release(123);
    }

    TEST(AnyReactor, WrapsWithoutOwning) {
        coro::native_reactor native;
        const sql::any_reactor reactor{&native};

        EXPECT_EQ(coro::sync_wait(reactor.sleep(1ms)), coro::wait_status::timeout);
    }

    TEST(AnyReactor, WaitWakesOnFdReady) {
        const sql::any_reactor reactor;
        int fds[2];
        ASSERT_EQ(::pipe(fds), 0);
        EXPECT_EQ(::write(fds[1], "x", 1), 1);

        EXPECT_EQ(coro::sync_wait(reactor.wait(fds[0], coro::interest::read, 2s)), coro::wait_status::ready);

        ::close(fds[0]);
        ::close(fds[1]);
    }

    TEST(AnyReactor, SatisfiesTheConcept) {
        static_assert(coro::io_reactor<sql::any_reactor>);
    }
}
