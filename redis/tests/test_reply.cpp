#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <redis/error.h>
#include <redis/reply.h>

#include "support/resp.h"

using redis::reply_type;
using redis_test::parse;

TEST(Reply, EmptyReadsAsNil) {
    const redis::reply r;
    EXPECT_EQ(r.type(), reply_type::nil);
    EXPECT_TRUE(r.is_null());
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(r.raw(), nullptr);
}

TEST(Reply, TypesOfEveryResp3Kind) {
    EXPECT_EQ(parse("_\r\n").type(), reply_type::nil);
    EXPECT_EQ(parse("+OK\r\n").type(), reply_type::status);
    EXPECT_EQ(parse("$2\r\nhi\r\n").type(), reply_type::string);
    EXPECT_EQ(parse("=6\r\ntxt:hi\r\n").type(), reply_type::verbatim);
    EXPECT_EQ(parse(":1\r\n").type(), reply_type::integer);
    EXPECT_EQ(parse(",1.5\r\n").type(), reply_type::real);
    EXPECT_EQ(parse("#t\r\n").type(), reply_type::boolean);
    EXPECT_EQ(parse("-ERR x\r\n").type(), reply_type::error);
    EXPECT_EQ(parse("*0\r\n").type(), reply_type::array);
    EXPECT_EQ(parse("%0\r\n").type(), reply_type::map);
    EXPECT_EQ(parse("~0\r\n").type(), reply_type::set);
    EXPECT_EQ(parse(">1\r\n+x\r\n").type(), reply_type::push);
    EXPECT_EQ(parse("(123456789012345678901234567890\r\n").type(), reply_type::bignum);
    EXPECT_EQ(redis::to_string(reply_type::attribute), "attribute");
}

TEST(Reply, StringsFromEveryTextKind) {
    EXPECT_EQ(parse("+OK\r\n").as<std::string>(), "OK");
    EXPECT_EQ(parse("$5\r\nhello\r\n").as<std::string>(), "hello");
    EXPECT_EQ(parse("=6\r\ntxt:hi\r\n").as<std::string>(), "hi");
    EXPECT_EQ(parse("(12345678901234567890123\r\n").as<std::string>(), "12345678901234567890123");
    EXPECT_EQ(parse(":42\r\n").as<std::string>(), "42");
    EXPECT_EQ(parse(",1.5\r\n").as<std::string>(), "1.5");
    EXPECT_EQ(parse("$5\r\nhello\r\n").as<std::string_view>(), "hello");
    EXPECT_THROW((void)parse(":42\r\n").as<std::string_view>(), redis::error);
}

TEST(Reply, BinarySafeString) {
    const auto r = parse(std::string_view{"$3\r\na\0b\r\n", 9});
    EXPECT_EQ(r.as<std::string>(), (std::string{"a\0b", 3}));
}

TEST(Reply, IntegersNativeAndParsed) {
    EXPECT_EQ(parse(":42\r\n").as<int>(), 42);
    EXPECT_EQ(parse(":-7\r\n").as<std::int64_t>(), -7);
    EXPECT_EQ(parse("$2\r\n42\r\n").as<int>(), 42);
    EXPECT_EQ(parse(":300\r\n").as<std::uint16_t>(), 300);
}

TEST(Reply, IntegerOutOfRangeOrUnparsableThrowsConversion) {
    EXPECT_THROW((void)parse(":300\r\n").as<std::uint8_t>(), redis::error);
    EXPECT_THROW((void)parse(":-1\r\n").as<unsigned>(), redis::error);
    EXPECT_THROW((void)parse("$3\r\nabc\r\n").as<int>(), redis::error);
    EXPECT_THROW((void)parse("$3\r\n1.5\r\n").as<int>(), redis::error);
    try {
        (void)parse("$3\r\nabc\r\n").as<int>();
    } catch (const redis::error& e) {
        EXPECT_EQ(e.kind(), redis::error_kind::conversion);
    }
}

TEST(Reply, Floats) {
    EXPECT_DOUBLE_EQ(parse(",1.5\r\n").as<double>(), 1.5);
    EXPECT_DOUBLE_EQ(parse(":2\r\n").as<double>(), 2.0);
    EXPECT_DOUBLE_EQ(parse("$3\r\n2.5\r\n").as<double>(), 2.5);
    EXPECT_FLOAT_EQ(parse(",0.25\r\n").as<float>(), 0.25f);
    EXPECT_THROW((void)parse("+OK\r\n").as<double>(), redis::error);
}

TEST(Reply, Booleans) {
    EXPECT_TRUE(parse("#t\r\n").as<bool>());
    EXPECT_FALSE(parse("#f\r\n").as<bool>());
    EXPECT_TRUE(parse(":1\r\n").as<bool>());
    EXPECT_FALSE(parse(":0\r\n").as<bool>());
    EXPECT_THROW((void)parse(":2\r\n").as<bool>(), redis::error);
    EXPECT_THROW((void)parse("$1\r\nt\r\n").as<bool>(), redis::error);
}

TEST(Reply, OptionalMapsNil) {
    EXPECT_EQ(parse("_\r\n").as<std::optional<std::string>>(), std::nullopt);
    EXPECT_EQ(parse("$1\r\nx\r\n").as<std::optional<std::string>>(), "x");
    EXPECT_EQ(parse("_\r\n").as<std::optional<int>>(), std::nullopt);
    EXPECT_THROW((void)parse("_\r\n").as<std::string>(), redis::error);
    EXPECT_THROW((void)parse("_\r\n").as<int>(), redis::error);
}

TEST(Reply, VectorsFromArraySetPushAndFlattenedMap) {
    EXPECT_EQ(parse("*2\r\n:1\r\n:2\r\n").as<std::vector<int>>(), (std::vector<int>{1, 2}));
    EXPECT_EQ(parse("~2\r\n$1\r\na\r\n$1\r\nb\r\n").as<std::vector<std::string>>(), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(parse(">2\r\n$7\r\nmessage\r\n$2\r\nch\r\n").as<std::vector<std::string>>(),
              (std::vector<std::string>{"message", "ch"}));
    EXPECT_EQ(parse("%2\r\n$1\r\nk\r\n$1\r\nv\r\n$1\r\nx\r\n:1\r\n").as<std::vector<std::string>>(),
              (std::vector<std::string>{"k", "v", "x", "1"}));
    EXPECT_EQ(parse("*2\r\n$1\r\na\r\n_\r\n").as<std::vector<std::optional<std::string>>>(),
              (std::vector<std::optional<std::string>>{"a", std::nullopt}));
    EXPECT_THROW((void)parse(":1\r\n").as<std::vector<int>>(), redis::error);
}

TEST(Reply, ElementAccess) {
    const auto r = parse("*3\r\n:1\r\n*2\r\n+a\r\n+b\r\n_\r\n");
    EXPECT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].as<int>(), 1);
    EXPECT_EQ(r[1].size(), 2u);
    EXPECT_EQ(r[1][1].as<std::string_view>(), "b");
    EXPECT_TRUE(r[2].is_null());
    EXPECT_THROW((void)r[3], redis::error);
    EXPECT_EQ(r[0].raw(), r.raw()->element[0]);
    EXPECT_EQ(parse(":1\r\n").size(), 0u);
}

TEST(Reply, ErrorElementConvertsToNothing) {
    const auto r = parse("*1\r\n-ERR nested\r\n");
    EXPECT_EQ(r[0].type(), reply_type::error);
    EXPECT_THROW((void)r[0].as<std::string>(), redis::error);
    EXPECT_THROW((void)r[0].as<std::string_view>(), redis::error);
    EXPECT_THROW((void)r[0].as<int>(), redis::error);
}

TEST(Reply, ConversionMessageNamesTheReplyType) {
    try {
        (void)parse("*0\r\n").as<int>();
        ADD_FAILURE() << "expected redis::error";
    } catch (const redis::error& e) {
        EXPECT_EQ(e.kind(), redis::error_kind::conversion);
        EXPECT_NE(std::string_view{e.what()}.find("array"), std::string_view::npos);
    }
}
