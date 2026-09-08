#include <gtest/gtest.h>

#include <atomic>
#include <coroutine>
#include <memory>
#include <thread>

#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <h2o.h>

#include <sql/sql.h>
#include <owl/core/state.h>
#include <owl/detail.h>

namespace {
    struct App final {};

    struct Fixture final {
        h2o_globalconf_t globalconf{};
        h2o_context_t ctx{};
        h2o_conn_t conn{};
        h2o_req_t req{};
        owl::detail::Dispatcher<App>* dispatcher = nullptr;

        Fixture() {
            h2o_config_init(&globalconf);
            auto* const hostconf = h2o_config_register_host(&globalconf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
            auto* const pathconf = h2o_config_register_path(hostconf, "/", 0);
            h2o_context_init(&ctx, h2o_evloop_create(), &globalconf);
            conn.ctx = &ctx;
            conn.hosts = globalconf.hosts;

            dispatcher = reinterpret_cast<owl::detail::Dispatcher<App>*>(
                h2o_create_handler(pathconf, sizeof(owl::detail::Dispatcher<App>)));
            dispatcher->super.on_context_init = &owl::detail::on_context_init<App>;
            dispatcher->super.on_context_dispose = &owl::detail::on_context_dispose<App>;
            std::construct_at(&dispatcher->state, std::make_shared<App>());
#ifdef OWL_ENABLE_SQLITE
            std::construct_at(&dispatcher->sqlite,
                              std::make_shared<sql::pool<sql::sqlite>>(sql::sqlite::config{.path = ":memory:"}));
#endif

            h2o_mem_init_pool(&req.pool);
            req.conn = &conn;
            req.pathconf = pathconf;
            req.method = {.base = const_cast<char*>("GET"), .len = 3};
            req.query_at = SIZE_MAX;
            req.version = 0x101;
            req.res.content_length = SIZE_MAX;

            // The real wiring: this is what the Server constructor would
            // trigger per worker.
            dispatcher->super.on_context_init(&dispatcher->super, &ctx);
        }

        ~Fixture() {
            // h2o_context_dispose invokes on_context_dispose for every
            // registered handler; calling it here too would double-free the
            // handler context.
            h2o_mem_clear_pool(&req.pool);
            h2o_loop_t* const loop = ctx.loop;
            h2o_context_dispose(&ctx);
            h2o_evloop_destroy(loop);
            h2o_config_dispose(&globalconf);
        }
    };

#ifdef OWL_ENABLE_SQLITE
    TEST(OwlWiring, SqliteSourceResolvesAndQueriesCompleteOnTheLoop) {
        Fixture fx;
        const auto* const context =
            static_cast<const owl::Context<App>*>(h2o_context_get_handler_context(&fx.ctx, &fx.dispatcher->super));
        ASSERT_NE(context, nullptr);
        ASSERT_TRUE(static_cast<bool>(context->sqlite));

        const auto* const request = owl::Request::from(&fx.req);
        const auto db = owl::fromContextRef<sql::sqlite_handle>(*context, *request);
        ASSERT_TRUE(db.has_value());

        // The completion must come back through the worker's loop: pump the
        // loop on a second thread, the way a real worker would own it, and
        // let sync_wait block the main thread until the hop delivers.
        std::atomic<bool> finished{false};
        std::thread loop_thread([&] {
            while (!finished.load()) {
                h2o_evloop_run(fx.ctx.loop, 5);
            }
        });

        auto body = [&]() -> coro::task<void> {
            const auto r = co_await sql::query<"SELECT 42 AS answer">(**db);
            EXPECT_TRUE(r.has_value());
            EXPECT_EQ((*r)[0][0].as<int>(), 42);
            finished.store(true);
        };
        coro::sync_wait(body());

        loop_thread.join();
    }

    TEST(OwlWiring, NullSqliteHandleKicksInternalError) {
        // A Context whose sqlite member was never wired (no with_sqlite).
        owl::Context<App> context{std::make_shared<App>()};
        h2o_req_t req{};
        h2o_mem_init_pool(&req.pool);
        const auto* const request = owl::Request::from(&req);

        const auto db = owl::fromContextRef<sql::sqlite_handle>(context, *request);

        EXPECT_FALSE(db.has_value());
        EXPECT_EQ(db.error().status(), 500);
        h2o_mem_clear_pool(&req.pool);
    }
#endif

#ifdef OWL_ENABLE_POSTGRESQL
    TEST(OwlWiring, UnwiredPsqlExtractorKicksInternalError) {
        Fixture fx;  // no psql config stored: the member stays null
        const auto* const context =
            static_cast<const owl::Context<App>*>(h2o_context_get_handler_context(&fx.ctx, &fx.dispatcher->super));
        ASSERT_NE(context, nullptr);

        const auto* const request = owl::Request::from(&fx.req);
        const auto pg = owl::fromContextRef<sql::pool<sql::psql>>(*context, *request);

        EXPECT_FALSE(pg.has_value());
        EXPECT_EQ(pg.error().status(), 500);
    }
#endif
}
