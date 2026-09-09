#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <h2o.h>

#include <sql/sql.h>
#ifdef OWL_ENABLE_REDIS
#include <redis/redis.h>
#endif
#include <owl/core/state.h>
#include <owl/detail.h>
#include <owl/extract/from_context.h>
#include <owl/server.h>

#include "support/temp_db.h"

namespace {
    struct App final {
    };

#ifdef OWL_ENABLE_REDIS
    // OWL_TEST_REDIS=host:port; port 1 when unset, so a test that wires the
    // client without a server still builds a Context.
    redis::config live_redis() {
        redis::config cfg{.port = 1};
        const char* const env = std::getenv("OWL_TEST_REDIS");
        if (env == nullptr) return cfg;
        const std::string_view spec{env};
        const auto colon = spec.rfind(':');
        cfg.host = std::string{spec.substr(0, colon)};
        cfg.port = colon == std::string_view::npos ? 6379 : static_cast<std::uint16_t>(std::stoi(std::string{spec.substr(colon + 1)}));
        return cfg;
    }
#endif

    // An h2o context wired the way Server wires a worker -- the same
    // make_dispatcher, then h2o_context_init running on_context_init --
    // without a listener.
    struct Fixture final {
        sql_test::temp_db db{"wiring"};
        h2o_globalconf_t globalconf{};
        h2o_context_t ctx{};
        h2o_conn_t conn{};
        h2o_req_t req{};
        owl::detail::Dispatcher<App>* dispatcher = nullptr;

        Fixture(const bool wire_sqlite, const char* const psql_dsn, const bool wire_redis = false) {
            h2o_config_init(&globalconf);
            auto* const hostconf = h2o_config_register_host(&globalconf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
            auto* const pathconf = h2o_config_register_path(hostconf, "/", 0);

            owl::detail::DriverConfigs configs;
#ifdef OWL_ENABLE_POSTGRESQL
            if (psql_dsn != nullptr) configs.psql = sql::psql::config{.dsn = psql_dsn};
#else
            (void)psql_dsn;
#endif
#ifdef OWL_ENABLE_SQLITE
            if (wire_sqlite) configs.sqlite = sql::sqlite::config{.path = db.path.string()};
#else
            (void)wire_sqlite;
#endif
#ifdef OWL_ENABLE_REDIS
            if (wire_redis) configs.redis = live_redis();
#else
            (void)wire_redis;
#endif
            dispatcher = owl::detail::make_dispatcher<App>(pathconf, nullptr, nullptr, std::make_shared<App>(), std::move(configs));

            h2o_context_init(&ctx, h2o_evloop_create(), &globalconf);
            conn.ctx = &ctx;
            conn.hosts = globalconf.hosts;

            h2o_mem_init_pool(&req.pool);
            req.conn = &conn;
            req.pathconf = pathconf;
            req.method = {.base = const_cast<char*>("GET"), .len = 3};
            req.query_at = SIZE_MAX;
            req.version = 0x101;
            req.res.content_length = SIZE_MAX;
        }

        ~Fixture() {
            h2o_mem_clear_pool(&req.pool);
            h2o_loop_t* const loop = ctx.loop;
            h2o_context_dispose(&ctx);
            h2o_evloop_destroy(loop);
            h2o_config_dispose(&globalconf);
        }

        // Not const: h2o_context_get_handler_context wants a mutable context.
        const owl::Context<App>& context() {
            return *static_cast<const owl::Context<App>*>(h2o_context_get_handler_context(&ctx, &dispatcher->super));
        }

        // Pumps the loop on its own thread, the way a worker owns it, and
        // runs the body there, the way a handler runs: the pools and the
        // reactor are loop-thread-only. Returns that thread's id for the
        // on-loop check.
        template <typename Body>
        std::thread::id run_on_loop(Body body) {
            std::atomic<bool> finished{false};
            std::thread::id loop_id;
            std::thread loop_thread([&] {
                loop_id = std::this_thread::get_id();
                while (!finished.load()) h2o_evloop_run(ctx.loop, 5);
            });
            coro::sync_wait([&]() -> coro::task<> {
                co_await context().loop.schedule();
                co_await body();
                finished.store(true);
            }());
            loop_thread.join();
            return loop_id;
        }
    };
}

#ifdef OWL_ENABLE_SQLITE
TEST(OwlWiring, SqliteQueryCompletesOnTheWorkerLoop) {
    Fixture fx{true, nullptr};
    const auto& context = fx.context();
    ASSERT_NE(context.sqlite, nullptr);

    const auto* const request = owl::Request::from(&fx.req);
    const auto db = owl::fromContextRef<sql::pool<sql::sqlite>>(context, *request);
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(*db, context.sqlite.get());

    const auto all = owl::extract_all<const sql::pool<sql::sqlite>&>(context, *request);
    ASSERT_TRUE(all.has_value());

    std::thread::id resumed_on;
    const auto loop_id = fx.run_on_loop([&]() -> coro::task<> {
        const auto r = co_await sql::query<"SELECT 42 AS answer">(**db);
        resumed_on = std::this_thread::get_id();
        EXPECT_EQ(r[0]["answer"].as<int>(), 42);
    });
    EXPECT_EQ(resumed_on, loop_id);
}

TEST(OwlWiring, UnwiredSqlitePoolKicks500) {
    Fixture fx{false, nullptr};
    const auto* const request = owl::Request::from(&fx.req);
    const auto db = owl::fromContextRef<sql::pool<sql::sqlite>>(fx.context(), *request);
    EXPECT_FALSE(db.has_value());
    EXPECT_EQ(db.error().status(), 500);
}
#endif

#ifdef OWL_ENABLE_POSTGRESQL
TEST(OwlWiring, UnwiredPsqlPoolKicks500) {
    Fixture fx{false, nullptr};
    const auto* const request = owl::Request::from(&fx.req);
    const auto pg = owl::fromContextRef<sql::pool<sql::psql>>(fx.context(), *request);
    EXPECT_FALSE(pg.has_value());
    EXPECT_EQ(pg.error().status(), 500);
}

TEST(OwlWiring, PsqlQueryCompletesOnTheWorkerLoop) {
    const char* const dsn = std::getenv("OWL_TEST_PSQL_DSN");
    if (dsn == nullptr) GTEST_SKIP() << "OWL_TEST_PSQL_DSN is not set";
    Fixture fx{false, dsn};
    const auto& context = fx.context();
    ASSERT_NE(context.psql, nullptr);

    const auto* const request = owl::Request::from(&fx.req);
    const auto pg = owl::fromContextRef<sql::pool<sql::psql>>(context, *request);
    ASSERT_TRUE(pg.has_value());

    std::thread::id resumed_on;
    const auto loop_id = fx.run_on_loop([&]() -> coro::task<> {
        const auto r = co_await sql::query<"SELECT $1::int AS answer">(**pg, 42);
        resumed_on = std::this_thread::get_id();
        EXPECT_EQ(r[0]["answer"].as<int>(), 42);
    });
    EXPECT_EQ(resumed_on, loop_id);
}
#endif

#ifdef OWL_ENABLE_REDIS
TEST(OwlWiring, UnwiredRedisClientKicks500) {
    Fixture fx{false, nullptr, false};
    const auto* const request = owl::Request::from(&fx.req);
    const auto rd = owl::fromContextRef<redis::client>(fx.context(), *request);
    EXPECT_FALSE(rd.has_value());
    EXPECT_EQ(rd.error().status(), 500);
}

TEST(OwlWiring, RedisCommandCompletesOnTheWorkerLoop) {
    if (std::getenv("OWL_TEST_REDIS") == nullptr) GTEST_SKIP() << "OWL_TEST_REDIS is not set";
    Fixture fx{false, nullptr, true};
    const auto& context = fx.context();
    ASSERT_NE(context.redis, nullptr);

    const auto* const request = owl::Request::from(&fx.req);
    const auto rd = owl::fromContextRef<redis::client>(context, *request);
    ASSERT_TRUE(rd.has_value());
    EXPECT_EQ(*rd, context.redis.get());

    const auto all = owl::extract_all<const redis::client&>(context, *request);
    ASSERT_TRUE(all.has_value());

    std::thread::id resumed_on;
    const auto loop_id = fx.run_on_loop([&]() -> coro::task<> {
        const auto r = co_await redis::command(**rd, "PING");
        resumed_on = std::this_thread::get_id();
        EXPECT_EQ(r.as<std::string>(), "PONG");
        EXPECT_TRUE((*rd)->connected());
    });
    EXPECT_EQ(resumed_on, loop_id);
}
#endif

TEST(OwlWiring, BuilderAcceptsTheDriverConfigs) {
    const sql_test::temp_db db{"builder"};
    // Preprocessor lines inside one expression: the chain stays a single
    // rvalue pipeline, with no self-move of the builder.
    const owl::Server<App> server = owl::Server<App>::builder()
                                        .router(owl::Router<App>::make())
                                        .config({.port = 0})
#ifdef OWL_ENABLE_SQLITE
                                        .with_sqlite({.path = db.path.string()})
#endif
#ifdef OWL_ENABLE_POSTGRESQL
                                        .with_psql({.dsn = "postgres://127.0.0.1:1/nope"})
#endif
#ifdef OWL_ENABLE_REDIS
                                        .with_redis({.port = 1})
#endif
                                        .build_with(std::make_shared<App>());
    EXPECT_NE(server.port(), 0);
}
