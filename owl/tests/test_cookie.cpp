#include <chrono>
#include <format>
#include <gtest/gtest.h>
#include <string>

#include <owl/http/cookie.h>

TEST(Cookie, NameAndValueOnly) {
    EXPECT_EQ(owl::format_cookie({.name = "sid", .value = "abc"}), "sid=abc");
}

TEST(Cookie, EmptyValueIsKept) {
    EXPECT_EQ(owl::format_cookie({.name = "sid", .value = ""}), "sid=");
}

TEST(Cookie, PathIsAppended) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .path = "/x"}), "a=1; Path=/x");
}

TEST(Cookie, EmptyPathIsOmitted) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .path = ""}), "a=1");
}

TEST(Cookie, DomainIsAppended) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .domain = "example.com"}),
              "a=1; Domain=example.com");
}

TEST(Cookie, MaxAgeIsAppended) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .max_age = std::chrono::seconds{60}}),
              "a=1; Max-Age=60");
}

TEST(Cookie, MaxAgeZeroIsAppended) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .max_age = std::chrono::seconds{0}}),
              "a=1; Max-Age=0");
}

TEST(Cookie, NegativeMaxAgeIsAppended) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .max_age = std::chrono::seconds{-1}}),
              "a=1; Max-Age=-1");
}

TEST(Cookie, AbsentMaxAgeIsOmitted) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1"}), "a=1");
}

TEST(Cookie, HttpOnlyIsAppended) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .http_only = true}), "a=1; HttpOnly");
}

TEST(Cookie, SecureIsAppended) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .secure = true}), "a=1; Secure");
}

TEST(Cookie, SameSiteStrict) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .same_site = owl::same_site::strict}),
              "a=1; SameSite=Strict");
}

TEST(Cookie, SameSiteLax) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .same_site = owl::same_site::lax}),
              "a=1; SameSite=Lax");
}

TEST(Cookie, SameSiteNone) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .same_site = owl::same_site::none}),
              "a=1; SameSite=None");
}

TEST(Cookie, SameSiteUnspecifiedIsOmitted) {
    EXPECT_EQ(owl::format_cookie({.name = "a", .value = "1", .same_site = owl::same_site::unspecified}),
              "a=1");
}

TEST(Cookie, DefaultSameSiteIsUnspecified) {
    EXPECT_EQ(owl::cookie{}.same_site, owl::same_site::unspecified);
}

TEST(Cookie, AttributeOrderIsStable) {
    EXPECT_EQ(owl::format_cookie({
                  .name = "sid",
                  .value = "abc",
                  .path = "/",
                  .domain = "example.com",
                  .max_age = std::chrono::seconds{60},
                  .http_only = true,
                  .secure = true,
                  .same_site = owl::same_site::lax,
              }),
              "sid=abc; Path=/; Domain=example.com; Max-Age=60; HttpOnly; Secure; SameSite=Lax");
}

TEST(Cookie, SameSitePolicyNames) {
    EXPECT_EQ(owl::to_string(owl::same_site::strict), "Strict");
    EXPECT_EQ(owl::to_string(owl::same_site::lax), "Lax");
    EXPECT_EQ(owl::to_string(owl::same_site::none), "None");
    EXPECT_TRUE(owl::to_string(owl::same_site::unspecified).empty());
}

TEST(CookieFormat, FormatsViaStdFormat) {
    const owl::cookie c{.name = "sid", .value = "abc", .path = "/", .http_only = true};
    EXPECT_EQ(std::format("{}", c), "sid=abc; Path=/; HttpOnly");
}

TEST(CookieFormat, MatchesFormatCookie) {
    const owl::cookie c{
        .name = "sid",
        .value = "abc",
        .domain = "example.com",
        .max_age = std::chrono::seconds{5},
        .secure = true,
        .same_site = owl::same_site::none,
    };
    EXPECT_EQ(std::format("{}", c), owl::format_cookie(c));
}

TEST(CookieFormat, EmbedsInSurroundingText) {
    const owl::cookie c{.name = "a", .value = "1"};
    EXPECT_EQ(std::format("Set-Cookie: {}\r\n", c), "Set-Cookie: a=1\r\n");
}

TEST(CookieFormat, HonoursWidthAndAlignment) {
    constexpr owl::cookie c{.name = "a", .value = "1"};
    EXPECT_EQ(std::format("[{:>7}]", c), "[    a=1]");
    EXPECT_EQ(std::format("[{:<5}]", c), "[a=1  ]");
}

TEST(CookieFormat, FormatsSameSitePolicy) {
    EXPECT_EQ(std::format("{}", owl::same_site::strict), "Strict");
    EXPECT_EQ(std::format("{}", owl::same_site::lax), "Lax");
    EXPECT_EQ(std::format("{}", owl::same_site::none), "None");
    EXPECT_EQ(std::format("{}", owl::same_site::unspecified), "");
}
