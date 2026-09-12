#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#ifdef OWL_ENABLE_POSTGRESQL
#include <sql/psql/psql.h>
#endif
#ifdef OWL_ENABLE_SQLITE
#include <sql/sqlite/sqlite.h>
#endif
#ifdef OWL_ENABLE_REDIS
#include <redis/config.h>
#endif

namespace owl {
    // Bind/listen knobs plus optional drivers. Aggregate so
    // Server::builder().config({.port = 0, .sqlite = {{.path = p}}}) stays a
    // designated init; make() is the only parser, not a second type.
    struct Config final {
        std::string address = "127.0.0.1";
        std::uint16_t port = 8080;
        int backlog = 1024;
        unsigned threads = 1;

        // Unset means the extractor kicks 500 rather than opening a
        // default DSN the process never asked for.
#ifdef OWL_ENABLE_POSTGRESQL
        std::optional<sql::psql::config> psql;
#endif
#ifdef OWL_ENABLE_SQLITE
        std::optional<sql::sqlite::config> sqlite;
#endif
#ifdef OWL_ENABLE_REDIS
        std::optional<redis::config> redis;
#endif

        // CLI after env so a flag beats the process environment without
        // the caller merging the two. getopt is rejected: it permutes argv
        // and owns process-global optind.
        [[nodiscard]] static Config make(const int argc, char** argv) {
            Config config{};
            if (const char* const v = std::getenv("OWL_ADDRESS")) set_address(config, v);
            if (const char* const v = std::getenv("OWL_PORT")) set_port(config, v);
            if (const char* const v = std::getenv("OWL_THREADS")) set_threads(config, v);
            if (const char* const v = std::getenv("OWL_BACKLOG")) set_backlog(config, v);
#ifdef OWL_ENABLE_POSTGRESQL
            if (const char* const v = std::getenv("OWL_PG")) set_pg(config, v);
#endif
#ifdef OWL_ENABLE_SQLITE
            if (const char* const v = std::getenv("OWL_SQLITE")) set_sqlite(config, v);
#endif
#ifdef OWL_ENABLE_REDIS
            if (const char* const v = std::getenv("OWL_REDIS")) set_redis(config, v);
#endif
            apply_cli(config, argc, argv);
            return config;
        }

    private:
        // from_chars stops at the first non-digit: without full
        // consumption, "8080foo" would become 8080.
        template <typename T>
        static T parse_int(const std::string_view val, const std::string_view what) {
            if (val.empty()) throw std::invalid_argument(std::format("owl::Config: empty {}", what));
            T out{};
            const auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), out);
            if (ec != std::errc{} || ptr != val.data() + val.size()) {
                throw std::invalid_argument(std::format("owl::Config: invalid {}", what));
            }
            return out;
        }

        static void set_address(Config& config, const std::string_view v) {
            if (v.empty()) throw std::invalid_argument("owl::Config: empty address");
            config.address = std::string{v};
        }

        static void set_port(Config& config, const std::string_view v) {
            config.port = parse_int<std::uint16_t>(v, "port");
        }

        static void set_threads(Config& config, const std::string_view v) {
            const auto n = parse_int<unsigned>(v, "threads");
            if (n == 0) throw std::invalid_argument("owl::Config: threads must be at least 1");
            config.threads = n;
        }

        static void set_backlog(Config& config, const std::string_view v) {
            config.backlog = parse_int<int>(v, "backlog");
        }

#ifdef OWL_ENABLE_POSTGRESQL
        static void set_pg(Config& config, const std::string_view v) {
            if (v.empty()) throw std::invalid_argument("owl::Config: empty pg");
            config.psql = sql::psql::config{.dsn = std::string{v}};
        }
#endif

#ifdef OWL_ENABLE_SQLITE
        static void set_sqlite(Config& config, const std::string_view v) {
            if (v.empty()) throw std::invalid_argument("owl::Config: empty sqlite");
            config.sqlite = sql::sqlite::config{.path = std::string{v}};
        }
#endif

#ifdef OWL_ENABLE_REDIS
        static void set_redis(Config& config, const std::string_view v) {
            if (v.empty()) throw std::invalid_argument("owl::Config: empty redis");
            redis::config r{};
            const auto colon = v.rfind(':');
            if (colon == 0 || colon == v.size() - 1) {
                throw std::invalid_argument("owl::Config: invalid redis");
            }
            r.host = std::string{v.substr(0, colon)};
            if (colon != std::string_view::npos) {
                r.port = parse_int<std::uint16_t>(v.substr(colon + 1), "redis port");
            }
            if (r.host.empty()) throw std::invalid_argument("owl::Config: empty redis host");
            config.redis = std::move(r);
        }
#endif

        static std::string_view require_value(const int argc, char** argv, int& i, const std::string_view flag) {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                throw std::invalid_argument(std::format("owl::Config: missing value for {}", flag));
            }
            return argv[++i];
        }

        static void apply_cli(Config& config, const int argc, char** argv) {
            for (auto i = 1; i < argc; ++i) {
                const std::string_view arg = argv[i] != nullptr ? argv[i] : "";
                if (arg == "-a" || arg == "--address") {
                    set_address(config, require_value(argc, argv, i, arg));
                } else if (arg == "-p" || arg == "--port") {
                    set_port(config, require_value(argc, argv, i, arg));
                } else if (arg == "-t" || arg == "--threads") {
                    set_threads(config, require_value(argc, argv, i, arg));
                } else if (arg == "-b" || arg == "--backlog") {
                    set_backlog(config, require_value(argc, argv, i, arg));
#ifdef OWL_ENABLE_POSTGRESQL
                } else if (arg == "--pg") {
                    set_pg(config, require_value(argc, argv, i, arg));
#endif
#ifdef OWL_ENABLE_SQLITE
                } else if (arg == "--sqlite") {
                    set_sqlite(config, require_value(argc, argv, i, arg));
#endif
#ifdef OWL_ENABLE_REDIS
                } else if (arg == "--redis") {
                    set_redis(config, require_value(argc, argv, i, arg));
#endif
                } else if (arg.starts_with('-')) {
                    throw std::invalid_argument(std::format("owl::Config: unknown flag {}", arg));
                } else {
                    throw std::invalid_argument(std::format("owl::Config: unexpected argument {}", arg));
                }
            }
        }
    };
}
