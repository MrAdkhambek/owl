#pragma once

// What owl knows about a driver, in one place.
//
// Every driver owl wires needs the same four things said about it: the
// config the builder collects, where that config sits, where the built
// object sits on a worker's Context, and how to build one there. They used
// to be said again in each of four headers, so a driver cost the same
// blocks copied four times and a change of policy -- how a driver is built,
// what an unwired one answers -- had to be made identically in three places
// per file, with nothing to catch a missed one.
//
// Here they are said once per driver, and the policy is written against
// the traits rather than against each driver: wire() below builds every
// wired driver the same way, and one extractor in extract/from_context.h
// serves them all. Adding a driver is a specialization here, a member on
// Context and on DriverConfigs, and a builder setter -- three one-liners
// and this block, rather than eight blocks spread out.
//
// The primary template is left undefined on purpose: driver_type is then
// exactly "something this header describes", which is what the extractor
// constrains on so that it never captures an unrelated handler parameter.

#include <memory>
#include <optional>
#include <string_view>

#ifdef OWL_ENABLE_POSTGRESQL
#include <sql/io.h>
#include <sql/pool.h>
#include <sql/psql/psql.h>
#endif
#ifdef OWL_ENABLE_SQLITE
#include <sql/io.h>
#include <sql/pool.h>
#include <sql/sqlite/sqlite.h>
#endif
#ifdef OWL_ENABLE_REDIS
#include <coro/io/reactor_ref.h>
#include <redis/client.h>
#include <redis/config.h>
#endif

namespace owl::detail {
    template <typename T>
    struct DriverTraits;

    template <typename T>
    concept driver_type = requires { typename DriverTraits<T>::config; };

#ifdef OWL_ENABLE_POSTGRESQL
    template <>
    struct DriverTraits<sql::pool<sql::psql>> {
        using type = sql::pool<sql::psql>;
        using config = sql::psql::config;

        static constexpr std::string_view unwired = "postgres pool is not wired";

        template <typename Configs>
        [[nodiscard]] static const std::optional<config>& asked(const Configs& configs) noexcept {
            return configs.psql;
        }

        template <typename Ctx>
        [[nodiscard]] static auto& slot(Ctx& ctx) noexcept {
            return ctx.psql;
        }

        template <typename Ctx>
        [[nodiscard]] static std::unique_ptr<type> make(Ctx& ctx, const config& cfg) {
            return std::make_unique<type>(cfg, sql::reactor_ref{ctx.reactor});
        }
    };
#endif

#ifdef OWL_ENABLE_SQLITE
    template <>
    struct DriverTraits<sql::pool<sql::sqlite>> {
        using type = sql::pool<sql::sqlite>;
        using config = sql::sqlite::config;

        static constexpr std::string_view unwired = "sqlite pool is not wired";

        template <typename Configs>
        [[nodiscard]] static const std::optional<config>& asked(const Configs& configs) noexcept {
            return configs.sqlite;
        }

        template <typename Ctx>
        [[nodiscard]] static auto& slot(Ctx& ctx) noexcept {
            return ctx.sqlite;
        }

        // The sqlite connections run on their own threads and post their
        // completions back through this worker's scheduler, not its reactor.
        template <typename Ctx>
        [[nodiscard]] static std::unique_ptr<type> make(Ctx& ctx, const config& cfg) {
            return std::make_unique<type>(cfg, sql::scheduler_ref{ctx.loop});
        }
    };
#endif

#ifdef OWL_ENABLE_REDIS
    template <>
    struct DriverTraits<redis::client> {
        using type = redis::client;
        using config = redis::config;

        static constexpr std::string_view unwired = "redis client is not wired";

        template <typename Configs>
        [[nodiscard]] static const std::optional<config>& asked(const Configs& configs) noexcept {
            return configs.redis;
        }

        template <typename Ctx>
        [[nodiscard]] static auto& slot(Ctx& ctx) noexcept {
            return ctx.redis;
        }

        template <typename Ctx>
        [[nodiscard]] static std::unique_ptr<type> make(Ctx& ctx, const config& cfg) {
            return std::make_unique<type>(cfg, coro::reactor_ref{ctx.reactor});
        }
    };
#endif

    // Builds one driver on a worker's Context, if the builder asked for it.
    // Called once per driver at context init; the drivers are built there
    // rather than with the Context because each holds a handle to a member
    // of the Context that has to exist first.
    template <driver_type T, typename Ctx, typename Configs>
    void wire(Ctx& ctx, const Configs& configs) {
        if (const auto& cfg = DriverTraits<T>::asked(configs)) {
            DriverTraits<T>::slot(ctx) = DriverTraits<T>::make(ctx, *cfg);
        }
    }
}
