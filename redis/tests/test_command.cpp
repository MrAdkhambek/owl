#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <coro/io/reactor.h>
#include <coro/io/reactor_ref.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <redis/client.h>
#include <redis/error.h>
#include <redis/reply.h>

#include "support/fake_server.h"
#include "support/fixture.h"
#include "support/resp.h"

using namespace std::chrono_literals;
using namespace redis_test;

TEST(Command, RendersArgumentsAndReturnsTheReply) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{cmd({"SET", "k", "5", "PX", "1500"})}, send_bytes{"+OK\r\n"},
        expect{cmd({"DEL", "a", "b"})}, send_bytes{":2\r\n"},
        expect{cmd({"GET", "missing"})}, send_bytes{"_\r\n"},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        const auto set = co_await c.try_command("SET", "k", 5, "PX", 1500);
        EXPECT_TRUE(set.has_value());
        if (set) EXPECT_EQ(set->as<std::string>(), "OK");

        const std::vector<std::string> keys{"a", "b"};
        const auto deleted = co_await c.command("DEL", keys);
        EXPECT_EQ(deleted.as<int>(), 2);

        const auto missing = co_await c.command("GET", std::string_view{"missing"});
        EXPECT_TRUE(missing.is_null());
    }());
    c.close();
    srv.join();
}

TEST(Command, ThrowingSpellingThrowsTheError) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{cmd({"INCR", "k"})}, send_bytes{"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        bool thrown = false;
        try {
            (void)co_await c.command("INCR", "k");
        } catch (const redis::error& e) {
            thrown = true;
            EXPECT_EQ(e.kind(), redis::error_kind::command);
            EXPECT_EQ(e.prefix(), "WRONGTYPE");
        }
        EXPECT_TRUE(thrown);
        EXPECT_TRUE(c.connected());
    }());
    c.close();
    srv.join();
}

TEST(Command, ConnectFailureIsReturnedNotThrownOnTheTryPath) {
    fixture fx;
    redis::client c{{.port = 1, .connect_timeout = 2000ms}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await c.try_command("PING");
        EXPECT_FALSE(r.has_value());
        if (!r) EXPECT_EQ(r.error().kind(), redis::error_kind::connect);
    }());
}
