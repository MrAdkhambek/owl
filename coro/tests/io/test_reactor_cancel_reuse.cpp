// A cancel names a request, and that request can finish on its own before
// the cancel is applied. If its memory then comes back as a new request
// before the reactor gets to the cancel, the cancel must not land on it.
//
// That needs the address to be reused, which the system allocator does only
// sometimes. This binary replaces the global allocator with one that hands
// the block freed last straight back to the next allocation of the same
// size on the same thread, which makes the reuse certain. It is a binary of
// its own so that no other test runs on that allocator.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <gtest/gtest.h>
#include <new>
#include <thread>
#include <utility>

#include <coro/io/reactor.h>
#include <coro/task.h>

#include "support/pipe.h"

using namespace std::chrono_literals;
using coro_test::pipe_pair;

namespace {
    // Every block carries its size in front of it, so the unsized delete
    // knows what it is keeping. Sixteen bytes keeps what follows aligned
    // the way operator new promises.
    struct header final {
        std::size_t size;
        std::size_t pad;
    };
    static_assert(sizeof(header) == 16);

    // The one block per thread freed last. The final one a thread keeps is
    // never returned, which a test binary can afford.
    thread_local header* spare = nullptr;
}

void* operator new(const std::size_t size) {
    if (spare != nullptr && spare->size == size) return std::exchange(spare, nullptr) + 1;
    auto* const h = static_cast<header*>(std::malloc(sizeof(header) + size));
    if (h == nullptr) throw std::bad_alloc{};
    h->size = size;
    return h + 1;
}

void operator delete(void* const p) noexcept {
    if (p == nullptr) return;
    std::free(std::exchange(spare, static_cast<header*>(p) - 1));
}

void operator delete(void* const p, std::size_t) noexcept {
    operator delete(p);
}

// The reactor thread is held while the first fd turns readable and only
// then is its wait cancelled, so the readiness is seen first and the wait
// finishes as ready. The same coroutine's next wait is then allocated where
// the first one was. The cancel still queued for the first must not end it.
TEST(ReactorCancelReuse, ACancelForAWaitThatFinishedFirstSparesTheNextWait) {
    coro::native_reactor reactor;
    const pipe_pair blocker;
    const pipe_pair first;
    const pipe_pair second;

    // Parks, and once cancelled holds the reactor thread until released.
    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
    const auto hold = [&]() -> coro::task<> {
        (void)co_await reactor.wait(blocker.read_end(), coro::interest::read, -1ms);
        holding.store(true);
        while (!release.load()) std::this_thread::sleep_for(1ms);
    };

    std::atomic<coro::wait_status> first_status{coro::wait_status::error};
    std::atomic<coro::wait_status> second_status{coro::wait_status::error};
    std::atomic<bool> done{false};
    const auto both = [&]() -> coro::task<> {
        first_status.store(co_await reactor.wait(first.read_end(), coro::interest::read, -1ms));
        second_status.store(co_await reactor.wait(second.read_end(), coro::interest::read, -1ms));
        done.store(true);
    };

    auto holder = hold();
    auto waiter = both();
    holder.start();
    waiter.start();
    std::this_thread::sleep_for(50ms);

    reactor.cancel(blocker.read_end());
    while (!holding.load()) std::this_thread::sleep_for(1ms);

    first.write_byte();
    reactor.cancel(first.read_end());
    release.store(true);

    // Long enough for a stale cancel to land, if one is left queued.
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(first_status.load(), coro::wait_status::ready) << "setup: the readiness was expected to win";
    second.write_byte();
    for (int i = 0; i < 200 && !done.load(); ++i) std::this_thread::sleep_for(5ms);

    ASSERT_TRUE(done.load());
    EXPECT_EQ(second_status.load(), coro::wait_status::ready) << "a cancel meant for the first wait fell on the second";
}
