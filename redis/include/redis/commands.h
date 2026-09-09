#pragma once

// Thin sugar over command / try_command for the calls the cache, rate-limit
// and pub/sub use cases make constantly. Each maps one reply shape and
// nothing more; everything else is command<>. The try_ / throwing pair is
// sql's. A try_ function never throws for a server or link failure; it
// still throws error{conversion} if the server answers a shape the helper
// did not promise, because reading a reply is synchronous.

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <coro/task.h>

#include "redis/client.h"
#include "redis/command.h"
#include "redis/detail/args.h"
#include "redis/error.h"
#include "redis/reply.h"

namespace redis {
    [[nodiscard]] inline coro::task<std::expected<std::optional<std::string>, error>>
    try_get(const client& c, const std::string_view key) {
        auto r = co_await try_command(c, "GET", key);
        if (!r) co_return std::unexpected(std::move(r.error()));
        co_return r->as<std::optional<std::string>>();
    }

    [[nodiscard]] inline coro::task<std::optional<std::string>> get(const client& c, const std::string_view key) {
        auto r = co_await try_get(c, key);
        if (!r) throw std::move(r.error());
        co_return std::move(*r);
    }

    [[nodiscard]] inline coro::task<std::expected<void, error>>
    try_set(const client& c, const std::string_view key, const std::string_view value) {
        auto r = co_await try_command(c, "SET", key, value);
        if (!r) co_return std::unexpected(std::move(r.error()));
        co_return std::expected<void, error>{};
    }

    [[nodiscard]] inline coro::task<std::expected<void, error>>
    try_set(const client& c, const std::string_view key, const std::string_view value, const std::chrono::milliseconds ttl) {
        auto r = co_await try_command(c, "SET", key, value, "PX", ttl.count());
        if (!r) co_return std::unexpected(std::move(r.error()));
        co_return std::expected<void, error>{};
    }

    [[nodiscard]] inline coro::task<> set(const client& c, const std::string_view key, const std::string_view value) {
        auto r = co_await try_set(c, key, value);
        if (!r) throw std::move(r.error());
    }

    [[nodiscard]] inline coro::task<>
    set(const client& c, const std::string_view key, const std::string_view value, const std::chrono::milliseconds ttl) {
        auto r = co_await try_set(c, key, value, ttl);
        if (!r) throw std::move(r.error());
    }

    // One key or a range of keys: both are args, and DEL takes a list.
    template <arg Keys>
    [[nodiscard]] coro::task<std::expected<std::int64_t, error>> try_del(const client& c, const Keys& keys) {
        auto r = co_await try_command(c, "DEL", keys);
        if (!r) co_return std::unexpected(std::move(r.error()));
        co_return r->template as<std::int64_t>();
    }

    template <arg Keys>
    [[nodiscard]] coro::task<std::int64_t> del(const client& c, const Keys& keys) {
        auto r = co_await try_del(c, keys);
        if (!r) throw std::move(r.error());
        co_return *r;
    }

    [[nodiscard]] inline coro::task<std::expected<std::int64_t, error>> try_incr(const client& c, const std::string_view key) {
        auto r = co_await try_command(c, "INCR", key);
        if (!r) co_return std::unexpected(std::move(r.error()));
        co_return r->as<std::int64_t>();
    }

    [[nodiscard]] inline coro::task<std::int64_t> incr(const client& c, const std::string_view key) {
        auto r = co_await try_incr(c, key);
        if (!r) throw std::move(r.error());
        co_return *r;
    }

    // true when the key existed and now has the TTL.
    [[nodiscard]] inline coro::task<std::expected<bool, error>>
    try_expire(const client& c, const std::string_view key, const std::chrono::seconds ttl) {
        auto r = co_await try_command(c, "EXPIRE", key, ttl.count());
        if (!r) co_return std::unexpected(std::move(r.error()));
        co_return r->as<bool>();
    }

    [[nodiscard]] inline coro::task<bool> expire(const client& c, const std::string_view key, const std::chrono::seconds ttl) {
        auto r = co_await try_expire(c, key, ttl);
        if (!r) throw std::move(r.error());
        co_return *r;
    }

    // EVAL script numkeys key... arg...; the key count is the range's size,
    // which is why keys must be a range and not a single string.
    template <typename Keys, typename Args>
        requires detail::range_arg<Keys> && detail::range_arg<Args>
    [[nodiscard]] coro::task<std::expected<reply, error>>
    try_eval(const client& c, const std::string_view script, const Keys& keys, const Args& args) {
        co_return co_await try_command(c, "EVAL", script, std::ranges::distance(keys), keys, args);
    }

    template <typename Keys, typename Args>
        requires detail::range_arg<Keys> && detail::range_arg<Args>
    [[nodiscard]] coro::task<reply> eval(const client& c, const std::string_view script, const Keys& keys, const Args& args) {
        auto r = co_await try_eval(c, script, keys, args);
        if (!r) throw std::move(r.error());
        co_return std::move(*r);
    }

    // The number of subscribers the message reached.
    [[nodiscard]] inline coro::task<std::expected<std::int64_t, error>>
    try_publish(const client& c, const std::string_view channel, const std::string_view payload) {
        auto r = co_await try_command(c, "PUBLISH", channel, payload);
        if (!r) co_return std::unexpected(std::move(r.error()));
        co_return r->as<std::int64_t>();
    }

    [[nodiscard]] inline coro::task<std::int64_t>
    publish(const client& c, const std::string_view channel, const std::string_view payload) {
        auto r = co_await try_publish(c, channel, payload);
        if (!r) throw std::move(r.error());
        co_return *r;
    }
}
