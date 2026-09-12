#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <owl/core/config.h>

namespace {
    constexpr const char* kVars[] = {
        "OWL_ADDRESS", "OWL_PORT", "OWL_THREADS", "OWL_BACKLOG",
        "OWL_PG", "OWL_SQLITE", "OWL_REDIS",
    };

    struct EnvIsolated : ::testing::Test {
        std::vector<std::pair<std::string, std::optional<std::string>>> saved;

        void SetUp() override {
            for (const char* const k : kVars) {
                if (const char* const v = std::getenv(k)) saved.emplace_back(k, std::string{v});
                else saved.emplace_back(k, std::nullopt);
                unsetenv(k);
            }
        }

        void TearDown() override {
            for (const auto& [k, v] : saved) {
                if (v) setenv(k.c_str(), v->c_str(), 1);
                else unsetenv(k.c_str());
            }
        }
    };

    struct Args final {
        std::vector<std::string> storage;
        std::vector<char*> ptrs;

        explicit Args(std::initializer_list<const char*> rest = {}) {
            storage.emplace_back("owl");
            for (const char* const s : rest) storage.emplace_back(s);
            ptrs.reserve(storage.size());
            for (auto& s : storage) ptrs.push_back(s.data());
        }

        [[nodiscard]] int argc() const { return static_cast<int>(ptrs.size()); }
        [[nodiscard]] char** argv() { return ptrs.data(); }
    };
}

TEST_F(EnvIsolated, DefaultsWhenArgvIsOnlyTheProgram) {
    Args args{};
    const auto c = owl::Config::make(args.argc(), args.argv());
    EXPECT_EQ(c.address, "127.0.0.1");
    EXPECT_EQ(c.port, 8080);
    EXPECT_EQ(c.backlog, 1024);
    EXPECT_EQ(c.threads, 1u);
#ifdef OWL_ENABLE_POSTGRESQL
    EXPECT_FALSE(c.psql);
#endif
#ifdef OWL_ENABLE_SQLITE
    EXPECT_FALSE(c.sqlite);
#endif
#ifdef OWL_ENABLE_REDIS
    EXPECT_FALSE(c.redis);
#endif
}

TEST_F(EnvIsolated, EnvOverridesDefaults) {
    setenv("OWL_ADDRESS", "0.0.0.0", 1);
    setenv("OWL_PORT", "9090", 1);
    setenv("OWL_THREADS", "4", 1);
    setenv("OWL_BACKLOG", "128", 1);
    Args args{};
    const auto c = owl::Config::make(args.argc(), args.argv());
    EXPECT_EQ(c.address, "0.0.0.0");
    EXPECT_EQ(c.port, 9090);
    EXPECT_EQ(c.threads, 4u);
    EXPECT_EQ(c.backlog, 128);
}

TEST_F(EnvIsolated, CliOverridesEnv) {
    setenv("OWL_ADDRESS", "0.0.0.0", 1);
    setenv("OWL_PORT", "9090", 1);
    setenv("OWL_THREADS", "4", 1);
    setenv("OWL_BACKLOG", "128", 1);
    Args args{"-a", "10.0.0.1", "--port", "80", "-t", "2", "--backlog", "16"};
    const auto c = owl::Config::make(args.argc(), args.argv());
    EXPECT_EQ(c.address, "10.0.0.1");
    EXPECT_EQ(c.port, 80);
    EXPECT_EQ(c.threads, 2u);
    EXPECT_EQ(c.backlog, 16);
}

TEST_F(EnvIsolated, LongFlags) {
    Args args{"--address", "::1", "--port", "443", "--threads", "8", "--backlog", "64"};
    const auto c = owl::Config::make(args.argc(), args.argv());
    EXPECT_EQ(c.address, "::1");
    EXPECT_EQ(c.port, 443);
    EXPECT_EQ(c.threads, 8u);
    EXPECT_EQ(c.backlog, 64);
}

TEST_F(EnvIsolated, RejectsPartialInteger) {
    Args args{"--port", "8080foo"};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, RejectsPartialIntegerInEnv) {
    setenv("OWL_PORT", "8080foo", 1);
    Args args{};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, RejectsUnknownFlag) {
    Args args{"--nope"};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, RejectsLeftoverPositional) {
    Args args{"serve"};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, RejectsMissingValue) {
    Args args{"--port"};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, RejectsEmptyAddress) {
    Args args{"--address", ""};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, RejectsEmptyEnvAddress) {
    setenv("OWL_ADDRESS", "", 1);
    Args args{};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, RejectsZeroThreads) {
    Args args{"-t", "0"};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, RejectsZeroThreadsFromEnv) {
    setenv("OWL_THREADS", "0", 1);
    Args args{};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, RejectsPortOutOfRange) {
    Args args{"-p", "65536"};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, AcceptsPortZero) {
    Args args{"-p", "0"};
    const auto c = owl::Config::make(args.argc(), args.argv());
    EXPECT_EQ(c.port, 0);
}

#ifdef OWL_ENABLE_POSTGRESQL
TEST_F(EnvIsolated, PgFromEnv) {
    setenv("OWL_PG", "postgres://localhost/app", 1);
    Args args{};
    const auto c = owl::Config::make(args.argc(), args.argv());
    ASSERT_TRUE(c.psql);
    EXPECT_EQ(c.psql->dsn, "postgres://localhost/app");
}

TEST_F(EnvIsolated, PgCliOverridesEnv) {
    setenv("OWL_PG", "postgres://localhost/app", 1);
    Args args{"--pg", "postgres://other/db"};
    const auto c = owl::Config::make(args.argc(), args.argv());
    ASSERT_TRUE(c.psql);
    EXPECT_EQ(c.psql->dsn, "postgres://other/db");
}

TEST_F(EnvIsolated, RejectsEmptyPg) {
    Args args{"--pg", ""};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}
#endif

#ifdef OWL_ENABLE_SQLITE
TEST_F(EnvIsolated, SqliteFromEnv) {
    setenv("OWL_SQLITE", "/tmp/app.db", 1);
    Args args{};
    const auto c = owl::Config::make(args.argc(), args.argv());
    ASSERT_TRUE(c.sqlite);
    EXPECT_EQ(c.sqlite->path, "/tmp/app.db");
}

TEST_F(EnvIsolated, SqliteCliOverridesEnv) {
    setenv("OWL_SQLITE", "/tmp/app.db", 1);
    Args args{"--sqlite", "/var/app.db"};
    const auto c = owl::Config::make(args.argc(), args.argv());
    ASSERT_TRUE(c.sqlite);
    EXPECT_EQ(c.sqlite->path, "/var/app.db");
}

TEST_F(EnvIsolated, RejectsEmptySqlite) {
    Args args{"--sqlite", ""};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}
#endif

#ifdef OWL_ENABLE_REDIS
TEST_F(EnvIsolated, RedisHostPortFromEnv) {
    setenv("OWL_REDIS", "10.0.0.1:6380", 1);
    Args args{};
    const auto c = owl::Config::make(args.argc(), args.argv());
    ASSERT_TRUE(c.redis);
    EXPECT_EQ(c.redis->host, "10.0.0.1");
    EXPECT_EQ(c.redis->port, 6380);
}

TEST_F(EnvIsolated, RedisHostOnlyDefaultsPort) {
    setenv("OWL_REDIS", "10.0.0.1", 1);
    Args args{};
    const auto c = owl::Config::make(args.argc(), args.argv());
    ASSERT_TRUE(c.redis);
    EXPECT_EQ(c.redis->host, "10.0.0.1");
    EXPECT_EQ(c.redis->port, 6379);
}

TEST_F(EnvIsolated, RedisCliOverridesEnv) {
    setenv("OWL_REDIS", "10.0.0.1:6380", 1);
    Args args{"--redis", "127.0.0.1:1"};
    const auto c = owl::Config::make(args.argc(), args.argv());
    ASSERT_TRUE(c.redis);
    EXPECT_EQ(c.redis->host, "127.0.0.1");
    EXPECT_EQ(c.redis->port, 1);
}

TEST_F(EnvIsolated, RejectsEmptyRedis) {
    Args args{"--redis", ""};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}

TEST_F(EnvIsolated, RejectsBadRedisPort) {
    Args args{"--redis", "localhost:xyz"};
    EXPECT_THROW((void)owl::Config::make(args.argc(), args.argv()), std::invalid_argument);
}
#endif
