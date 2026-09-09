#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <h2o.h>

#include <sql/sql.h>
#include <owl/core/state.h>
#include <owl/detail.h>
#include <owl/extract/from_context.h>
#include <owl/server.h>

namespace {
    struct App final {
    };

    std::string temp_db_path() {
        return (std::filesystem::temp_directory_path() / ("owl_wiring_" + std::to_string(::getpid()) + ".db")).string();
    }

    // An h2o context plus the dispatcher's own on_context_init: exactly the
    // per-worker wiring Server runs, without a listener.
    struct Fixture final {
        h2o_globalconf_t globalconf{};
        h2o_context_t ctx{};
        h2o_conn_t conn{};
        h2o_req_t req{};
        owl::detail::Dispatcher<App>* dispatcher = nullptr;

        // Same order as Server: the handler is created before the context,
        // because h2o_context_init sizes its per-handler slot array from the
        // handlers registered so far and then runs every on_context_init.
        Fixture(const bool wire_sqlite, const char* const psql_dsn) {
            h2o_config_init(&globalconf);
            auto* const hostconf = h2o_config_register_host(&globalconf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
            auto* const pathconf = h2o_config_register_path(hostconf, "/", 0);

            dispatcher = reinterpret_cast<owl::detail::Dispatcher<App>*>(
                h2o_create_handler(pathconf, sizeof(owl::detail::Dispatcher<App>)));
            dispatcher->super.on_context_init = &owl::detail::on_context_init<App>;
            dispatcher->super.on_context_dispose = &owl::detail::on_context_dispose<App>;
            std::construct_at(&dispatcher->state, std::make_shared<App>());
#ifdef OWL_ENABLE_POSTGRESQL
            std::construct_at(&dispatcher->psql_config);
            if (psql_dsn != nullptr) dispatcher->psql_config = sql::psql::config{.dsn = psql_dsn};
#else
            (void)psql_dsn;
#endif
#ifdef OWL_ENABLE_SQLITE
            std::construct_at(&dispatcher->sqlite_config);
            if (wire_sqlite) dispatcher->sqlite_config = sql::sqlite::config{.path = temp_db_path()};
#else
            (void)wire_sqlite;
#endif

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
            std::filesystem::remove(temp_db_path());
        }

        // Not const: h2o_context_get_handler_context wants a mutable context.
        const owl::Context<App>& context() {
            return *static_cast<const owl::Context<App>*>(h2o_context_get_handler_context(&ctx, &dispatcher->super));
        }

        // Pumps the loop on its own thread, the way a worker owns it, until
        // `finished` flips; returns that thread's id for the on-loop check.
        template <typename Body>
        std::thread::id run_on_loop(Body body) {
            std::atomic<bool> finished{false};
            std::thread::id loop_id;
            std::thread loop_thread([&] {
                loop_id = std::this_thread::get_id();
                while (!finished.load()) h2o_evloop_run(ctx.loop, 5);
            });
            coro::sync_wait([&]() -> coro::task<> {
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
    ASSERT_TRUE(context.sqlite.has_value());

    const auto* const request = owl::Request::from(&fx.req);
    const auto db = owl::fromContextRef<sql::pool<sql::sqlite>>(context, *request);
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(*db, &*context.sqlite);

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
    ASSERT_TRUE(context.psql.has_value());

    const auto* const request = owl::Request::from(&fx.req);
    const auto pg = owl::fromContextRef<sql::pool<sql::psql>>(context, *request);
    ASSERT_TRUE(pg.has_value());

    // A handler already runs on the worker, so the psql pool arms the
    // loop_reactor from the loop thread; this body starts on the test
    // thread and must hop there first -- the reactor's h2o state is
    // loop-thread-only. (The sqlite pool needs no hop: its first move is to
    // the connection thread, and it comes back through the loop's queue.)
    std::thread::id resumed_on;
    const auto loop_id = fx.run_on_loop([&]() -> coro::task<> {
        co_await context.loop.schedule();
        const auto r = co_await sql::query<"SELECT $1::int AS answer">(**pg, 42);
        resumed_on = std::this_thread::get_id();
        EXPECT_EQ(r[0]["answer"].as<int>(), 42);
    });
    EXPECT_EQ(resumed_on, loop_id);
}
#endif

TEST(OwlWiring, BuilderAcceptsTheDriverConfigs) {
    // Preprocessor lines inside one expression: the chain stays a single
    // rvalue pipeline, with no self-move of the builder.
    const owl::Server<App> server = owl::Server<App>::builder()
                                        .router(owl::Router<App>::make())
                                        .config({.port = 0})
#ifdef OWL_ENABLE_SQLITE
                                        .with_sqlite({.path = temp_db_path()})
#endif
#ifdef OWL_ENABLE_POSTGRESQL
                                        .with_psql({.dsn = "postgres://127.0.0.1:1/nope"})
#endif
                                        .build_with(std::make_shared<App>());
    EXPECT_NE(server.port(), 0);
    std::filesystem::remove(temp_db_path());
}
