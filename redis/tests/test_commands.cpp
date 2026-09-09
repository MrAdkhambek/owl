#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <coro/io/reactor.h>
#include <coro/io/reactor_ref.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <redis/client.h>
#include <redis/commands.h>
#include <redis/error.h>
#include <redis/reply.h>

#include "support/fake_server.h"
#include "support/resp.h"

using namespace std::chrono_literals;
using redis_test::cmd;
using redis_test::expect;
using redis_test::expect_eof;
using redis_test::fake_server;
using redis_test::script;
using redis_test::send_bytes;

namespace {
    const std::string hello = cmd({"HELLO", "3"});
    const std::string hello_ok = "%1\r\n$5\r\nproto\r\n:3\r\n";

    struct fixture final {
        coro::native_reactor reactor;
        coro::reactor_ref io{reactor};
    };
}

TEST(Commands, GetSetDelIncrExpire) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{cmd({"GET", "k"})}, send_bytes{"$1\r\nv\r\n"},
        expect{cmd({"GET", "missing"})}, send_bytes{"_\r\n"},
        expect{cmd({"SET", "k", "v"})}, send_bytes{"+OK\r\n"},
        expect{cmd({"SET", "k", "v", "PX", "1500"})}, send_bytes{"+OK\r\n"},
        expect{cmd({"DEL", "k"})}, send_bytes{":1\r\n"},
        expect{cmd({"DEL", "a", "b"})}, send_bytes{":2\r\n"},
        expect{cmd({"INCR", "n"})}, send_bytes{":3\r\n"},
        expect{cmd({"EXPIRE", "n", "60"})}, send_bytes{":1\r\n"},
        expect{cmd({"EXPIRE", "gone", "60"})}, send_bytes{":0\r\n"},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        EXPECT_EQ(co_await redis::get(c, "k"), std::optional<std::string>{"v"});
        EXPECT_EQ(co_await redis::get(c, "missing"), std::nullopt);
        co_await redis::set(c, "k", "v");
        co_await redis::set(c, "k", "v", 1500ms);
        EXPECT_EQ(co_await redis::del(c, "k"), 1);
        const std::array<std::string_view, 2> keys{"a", "b"};
        EXPECT_EQ(co_await redis::del(c, keys), 2);
        EXPECT_EQ(co_await redis::incr(c, "n"), 3);
        EXPECT_TRUE(co_await redis::expire(c, "n", 60s));
        EXPECT_FALSE(co_await redis::expire(c, "gone", 60s));
    }());
    c.close();
    srv.join();
}

TEST(Commands, EvalAndPublish) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{cmd({"EVAL", "return 1", "2", "a", "b", "x"})}, send_bytes{":1\r\n"},
        expect{cmd({"EVAL", "return 2", "0"})}, send_bytes{":2\r\n"},
        expect{cmd({"PUBLISH", "news", "hi"})}, send_bytes{":2\r\n"},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        const std::vector<std::string> keys{"a", "b"};
        const std::vector<std::string_view> args{"x"};
        const auto one = co_await redis::eval(c, "return 1", keys, args);
        EXPECT_EQ(one.as<int>(), 1);
        const std::vector<std::string_view> none;
        const auto two = co_await redis::try_eval(c, "return 2", none, none);
        EXPECT_TRUE(two.has_value());
        if (two) EXPECT_EQ(two->as<int>(), 2);
        EXPECT_EQ(co_await redis::publish(c, "news", "hi"), 2);
    }());
    c.close();
    srv.join();
}

TEST(Commands, TryTwinsReturnErrorsAndThrowingTwinsThrow) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{cmd({"INCR", "k"})}, send_bytes{"-WRONGTYPE bad\r\n"},
        expect{cmd({"GET", "k"})}, send_bytes{"-ERR nope\r\n"},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await redis::try_incr(c, "k");
        EXPECT_FALSE(r.has_value());
        if (!r) EXPECT_EQ(r.error().prefix(), "WRONGTYPE");
        bool thrown = false;
        try {
            (void)co_await redis::get(c, "k");
        } catch (const redis::error& e) {
            thrown = true;
            EXPECT_EQ(e.kind(), redis::error_kind::command);
        }
        EXPECT_TRUE(thrown);
    }());
    c.close();
    srv.join();
}
