#include <gtest/gtest.h>
#include <utility>

#include <owl/owl.h>

static_assert(sizeof(owl::Server) >= 1);

TEST(Web, LinksH2o) {
    h2o_globalconf_t conf;
    h2o_config_init(&conf);
    h2o_config_dispose(&conf);
}

TEST(Web, PullsCoroAndFstr) {
    constexpr fstr::fstr hello = "hello";
    EXPECT_EQ(hello.view(), "hello");

    auto t = []() -> coro::task<int> { co_return 1; }();
    EXPECT_EQ(std::move(t).get(), 1);
}
