#pragma once

// What a driver must provide, and what its result must look like, with
// none of how. pool.h, query.h and transaction.h are written against
// these two concepts alone, so a driver is anything that satisfies them
// -- the scripted fake in the tests included -- and generic handler code
// can constrain on result_like without naming a driver.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <coro/task.h>
#include <fstr/fstr.h>

#include "sql/error.h"

namespace sql {
    namespace detail {
        // The literal the driver concept probes execute<> with. Named
        // rather than spelled inline so the requires-expression stays a
        // plain template-argument list.
        inline constexpr fstr::fstr probe_query{"SELECT 1"};
    }

    // The shape both drivers' results share. Cells convert through as<T>();
    // there is no text() because a non-text sqlite cell has no honest one.
    template <typename R>
    concept result_like = requires(const R& r, const std::size_t i, const std::string_view name) {
        { r.rows() } -> std::convertible_to<std::size_t>;
        { r.columns() } -> std::convertible_to<std::size_t>;
        { r.affected() } -> std::convertible_to<std::uint64_t>;
        { r[i][i].is_null() } -> std::same_as<bool>;
        { r[i][name].is_null() } -> std::same_as<bool>;
        { r[i][i].template as<int>() } -> std::same_as<int>;
        { r[i][i].template as<std::optional<int>>() } -> std::same_as<std::optional<int>>;
        { r[i].template as<int>() } -> std::same_as<int>;
        { r[i].template as<int, std::string>() } -> std::same_as<std::tuple<int, std::string>>;
        { r.begin() };
        { r.end() };
    };

    // A driver: a config with a connection count, the io handle it waits
    // through, a connection that opens asynchronously and runs a literal,
    // and the result that comes back. ok() is false once the link is
    // broken; abandon() forces that, for a transaction whose ROLLBACK
    // failed. Every failure is data: open and execute answer expected.
    template <typename D>
    concept driver = requires {
            typename D::config;
            typename D::io;
            typename D::connection;
            typename D::result;
        } && result_like<typename D::result>
        && std::convertible_to<decltype(std::declval<const typename D::config&>().connections), unsigned>
        && requires(typename D::connection& c, const typename D::config& cfg, const typename D::io& io) {
            {
                D::connection::open(cfg, io)
            }
            -> std::same_as<coro::task<std::expected<std::unique_ptr<typename D::connection>, error>>>;
            { c.ok() } -> std::same_as<bool>;
            { c.abandon() } noexcept;
            {
                c.template execute<detail::probe_query>()
            }
            -> std::same_as<coro::task<std::expected<typename D::result, error>>>;
        };
}
