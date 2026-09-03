#include <format>
#include <gtest/gtest.h>
#include <string_view>

#include <owl/core/method.h>

static_assert(owl::to_string(owl::Method::Get) == "GET");
static_assert(owl::method_from_string("OPTIONS") == owl::Method::Options);
static_assert(owl::method_from_string("get") == owl::Method::Unknown);
static_assert(owl::method_forbids_body(owl::Method::Head));
static_assert(!owl::method_forbids_body(owl::Method::Get));

TEST(Method, ToStringIsTheHttpToken) {
    EXPECT_EQ(owl::to_string(owl::Method::Delete), "DELETE");
    EXPECT_EQ(owl::to_string(owl::Method::Unknown), "UNKNOWN");
}

TEST(Method, FromStringIsCaseSensitive) {
    EXPECT_EQ(owl::method_from_string("PUT"), owl::Method::Put);
    EXPECT_EQ(owl::method_from_string("put"), owl::Method::Unknown);
    EXPECT_EQ(owl::method_from_string(""), owl::Method::Unknown);
    EXPECT_EQ(owl::method_from_string("GOT"), owl::Method::Unknown);
}

TEST(Method, FormatsAsTheHttpToken) {
    EXPECT_EQ(std::format("{}", owl::Method::Post), "POST");
}
