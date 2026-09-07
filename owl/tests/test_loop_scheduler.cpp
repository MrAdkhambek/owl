#include <chrono>
#include <coroutine>
#include <thread>

#include <gtest/gtest.h>

#include <coro/executors/static_thread_pool.h>
#include <coro/task.h>

#include <owl/coro/loop_scheduler.h>

using namespace std::chrono_literals;

namespace {
    void pump(h2o_loop_t* const loop, coro::task<void>& work) {
        work.start();
        for (int i = 0; i < 200 && !work.done(); ++i) {
            h2o_evloop_run(loop, 5);
        }
        ASSERT_TRUE(work.done());
    }

    struct post_back {
        const owl::loop_scheduler& io;

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(const std::coroutine_handle<> h) const {
            io.post(h);
        }

        void await_resume() const noexcept {
        }
    };
}

TEST(LoopScheduler, ScheduleAfterElapses) {
    h2o_globalconf_t conf{};
    h2o_config_init(&conf);
    h2o_config_register_host(&conf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    h2o_context_t ctx{};
    h2o_context_init(&ctx, h2o_evloop_create(), &conf);
    owl::loop_scheduler io{ctx.loop};

    const auto start = std::chrono::steady_clock::now();
    auto work = [&io]() -> coro::task<void> {
        co_await io.schedule_after(20ms);
    }();
    pump(ctx.loop, work);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 15ms);
    h2o_loop_t* const loop = ctx.loop;
    h2o_context_dispose(&ctx);
    h2o_evloop_destroy(loop);
    h2o_config_dispose(&conf);
}

TEST(LoopScheduler, ScheduleReturnsFromPoolThread) {
    h2o_globalconf_t conf{};
    h2o_config_init(&conf);
    h2o_config_register_host(&conf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    h2o_context_t ctx{};
    h2o_context_init(&ctx, h2o_evloop_create(), &conf);
    h2o_multithread_receiver_t hop{};
    h2o_multithread_register_receiver(ctx.queue, &hop, &owl::detail::on_loop_hop);
    owl::loop_scheduler io{ctx.loop, &hop};
    coro::static_thread_pool pool{1};

    std::thread::id loop_id;
    std::thread::id pool_id;
    std::thread::id back_id;
    auto work = [&]() -> coro::task<void> {
        loop_id = std::this_thread::get_id();
        co_await pool.schedule();
        pool_id = std::this_thread::get_id();
        co_await io.schedule();
        back_id = std::this_thread::get_id();
    }();
    pump(ctx.loop, work);
    EXPECT_NE(pool_id, loop_id);
    EXPECT_EQ(back_id, loop_id);

    h2o_multithread_unregister_receiver(ctx.queue, &hop);
    h2o_loop_t* const loop = ctx.loop;
    h2o_context_dispose(&ctx);
    h2o_evloop_destroy(loop);
    h2o_config_dispose(&conf);
}

TEST(LoopScheduler, PostReturnsFromPoolThread) {
    h2o_globalconf_t conf{};
    h2o_config_init(&conf);
    h2o_config_register_host(&conf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    h2o_context_t ctx{};
    h2o_context_init(&ctx, h2o_evloop_create(), &conf);
    h2o_multithread_receiver_t hop{};
    h2o_multithread_register_receiver(ctx.queue, &hop, &owl::detail::on_loop_hop);
    owl::loop_scheduler io{ctx.loop, &hop};
    coro::static_thread_pool pool{1};

    std::thread::id loop_id;
    std::thread::id pool_id;
    std::thread::id back_id;
    auto work = [&]() -> coro::task<void> {
        loop_id = std::this_thread::get_id();
        co_await pool.schedule();
        pool_id = std::this_thread::get_id();
        co_await post_back{io};
        back_id = std::this_thread::get_id();
    }();
    pump(ctx.loop, work);
    EXPECT_NE(pool_id, loop_id);
    EXPECT_EQ(back_id, loop_id);

    h2o_multithread_unregister_receiver(ctx.queue, &hop);
    h2o_loop_t* const loop = ctx.loop;
    h2o_context_dispose(&ctx);
    h2o_evloop_destroy(loop);
    h2o_config_dispose(&conf);
}

TEST(LoopScheduler, PostWithoutHopResumesOnLoop) {
    h2o_globalconf_t conf{};
    h2o_config_init(&conf);
    h2o_config_register_host(&conf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    h2o_context_t ctx{};
    h2o_context_init(&ctx, h2o_evloop_create(), &conf);
    owl::loop_scheduler io{ctx.loop};

    bool resumed = false;
    auto work = [&]() -> coro::task<void> {
        co_await post_back{io};
        resumed = true;
    }();
    pump(ctx.loop, work);
    EXPECT_TRUE(resumed);

    h2o_loop_t* const loop = ctx.loop;
    h2o_context_dispose(&ctx);
    h2o_evloop_destroy(loop);
    h2o_config_dispose(&conf);
}
