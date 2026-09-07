#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <utility>

#include <owl/server.h>

namespace {
    struct App final {
        int n = 0;
    };
}

TEST(Builder, MissingRouterThrows) {
    EXPECT_THROW(
        (void)owl::Server<App>::builder().build_with(std::make_shared<App>()),
        std::invalid_argument);
}

TEST(Builder, MissingConfigThrows) {
    EXPECT_THROW(
        (void)owl::Server<App>::builder()
            .router(owl::Router<App>::make())
            .build_with(std::make_shared<App>()),
        std::invalid_argument);
}

TEST(Builder, DoubleRouterThrows) {
    EXPECT_THROW(
        (void)owl::Server<App>::builder().router(owl::Router<App>::make()).router(owl::Router<App>::make()),
        std::invalid_argument);
}

TEST(Builder, DoubleConfigThrows) {
    EXPECT_THROW(
        (void)owl::Server<App>::builder().config({}).config({}),
        std::invalid_argument);
}

TEST(Builder, ThreadThenConfigThrows) {
    EXPECT_THROW(
        (void)owl::Server<App>::builder().thread(4).config({}),
        std::invalid_argument);
}

TEST(Builder, ThreadAfterConfigOk) {
    EXPECT_NO_THROW((void)owl::Server<App>::builder().config({}).thread(4));
}

TEST(Builder, ThreadAloneOk) {
    EXPECT_NO_THROW((void)owl::Server<App>::builder().thread(4));
}

TEST(Builder, BuildWithNullStateThrows) {
    EXPECT_THROW(
        (void)owl::Server<App>::builder()
            .router(owl::Router<App>::make())
            .config({})
            .build_with(nullptr),
        std::invalid_argument);
}
