#include <gtest/gtest.h>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <tuple>

#include <coro/task.h>

#include <sql/concepts.h>
#include <sql/error.h>

#include "support/fake_driver.h"

namespace {
    struct no_abandon final {
        struct config final { unsigned connections = 1; };
        struct io final {};
        using result = sql_test::fake::result;
        class connection final {
        public:
            static coro::task<std::expected<std::unique_ptr<connection>, sql::error>> open(const config&, const io&);
            bool ok() const noexcept { return true; }
            template <fstr::fstr Q, sql::bindable... Args>
            coro::task<std::expected<result, sql::error>> execute(const Args&...);
        };
    };
}

TEST(Concepts, FakeIsADriverAndItsResultIsResultLike) {
    static_assert(sql::driver<sql_test::fake>);
    static_assert(sql::result_like<sql_test::fake::result>);
    static_assert(!sql::driver<no_abandon>);
    static_assert(!sql::driver<int>);
    static_assert(!sql::result_like<int>);
}

TEST(FakeDriver, ScriptedRowsIndexAndConvert) {
    sql_test::fake::script s;
    s.rows.push_back({{"1", "a"}, {"2", std::nullopt}});
    auto body = [&]() -> coro::task<> {
        auto c = co_await sql_test::fake::connection::open({.connections = 1, .script_ = &s}, {});
        EXPECT_TRUE(c.has_value());
        if (!c) co_return;
        const auto r = co_await (*c)->execute<"SELECT 1">();
        EXPECT_TRUE(r.has_value());
        if (!r) co_return;
        EXPECT_EQ(r->rows(), 2u);
        EXPECT_EQ(r->columns(), 2u);
        EXPECT_EQ(r->affected(), 1u);
        EXPECT_EQ(((*r)[0].as<int, std::string>()), (std::tuple<int, std::string>{1, "a"}));
        EXPECT_EQ((*r)[0]["c1"].as<std::string>(), "a");
        EXPECT_EQ((*r)[1][1].as<std::optional<int>>(), std::nullopt);
        EXPECT_TRUE((*r)[1][1].is_null());
        EXPECT_THROW((void)(*r)[1][1].as<int>(), sql::error);
        EXPECT_THROW((void)(*r)[2], sql::error);
        EXPECT_THROW((void)(*r)[0][5], sql::error);
        EXPECT_THROW((void)(*r)[0]["nope"], sql::error);
        int seen = 0;
        for (const auto row : *r) seen += row[0].as<int>();
        EXPECT_EQ(seen, 3);
        EXPECT_EQ(s.log, (std::vector<std::string>{"SELECT 1"}));
    };
    auto t = body();
    t.start();
    EXPECT_TRUE(t.done());
}

TEST(FakeDriver, SuspendParksUntilResumed) {
    sql_test::fake::script s;
    s.suspend = true;
    bool finished = false;
    auto body = [&]() -> coro::task<> {
        auto c = co_await sql_test::fake::connection::open({.connections = 1, .script_ = &s}, {});
        (void)co_await (*c)->execute<"SELECT 2">();
        finished = true;
    };
    auto t = body();
    t.start();
    EXPECT_FALSE(finished);
    EXPECT_EQ(s.parked.size(), 1u);
    s.resume_all();
    EXPECT_TRUE(finished);
    EXPECT_TRUE(t.done());
}

TEST(FakeDriver, ScriptedFailuresAndBrokenConnections) {
    sql_test::fake::script s;
    s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "bad"}));
    s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::connection, "gone"}));
    auto body = [&]() -> coro::task<> {
        auto c = co_await sql_test::fake::connection::open({.connections = 1, .script_ = &s}, {});
        EXPECT_EQ((co_await (*c)->execute<"A">()).error().kind(), sql::error_kind::query);
        EXPECT_TRUE((*c)->ok());
        EXPECT_EQ((co_await (*c)->execute<"B">()).error().kind(), sql::error_kind::connection);
        EXPECT_FALSE((*c)->ok());
    };
    auto t = body();
    t.start();
    EXPECT_TRUE(t.done());
    EXPECT_EQ(s.destroyed, 1);
}
