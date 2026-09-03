#include <cstddef>
#include <gtest/gtest.h>
#include <string_view>

#include <owl/util/util.h>

namespace {
    owl::pool_map parse(h2o_req_t& req, char* const path, const std::size_t path_len, const std::size_t query_at) {
        req.path = {.base = path, .len = path_len};
        req.query_at = query_at;
        return owl::util::parse_query_req(&req);
    }
}

TEST(ParseQueryReq, EmptyYieldsNoPairs) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/only";
        const auto got = parse(req, path, sizeof(path) - 1, SIZE_MAX);
        EXPECT_FALSE(got.contains("a"));
        EXPECT_EQ(got.find_value("a"), nullptr);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, ParsesAfterQuestionMark) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?a=1";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        ASSERT_NE(got.find_value("a"), nullptr);
        EXPECT_EQ(*got.find_value("a"), "1");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, SplitsOnAmpersand) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?a=1&b=two";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        ASSERT_NE(got.find_value("a"), nullptr);
        ASSERT_NE(got.find_value("b"), nullptr);
        EXPECT_EQ(*got.find_value("a"), "1");
        EXPECT_EQ(*got.find_value("b"), "two");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, KeyWithoutEqualsHasEmptyValue) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?flag";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        ASSERT_NE(got.find_value("flag"), nullptr);
        EXPECT_EQ(*got.find_value("flag"), "");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, EmptyValue) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?a=";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        ASSERT_NE(got.find_value("a"), nullptr);
        EXPECT_EQ(*got.find_value("a"), "");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, ValueMayContainEquals) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?a=b=c";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        ASSERT_NE(got.find_value("a"), nullptr);
        EXPECT_EQ(*got.find_value("a"), "b=c");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, DuplicateKeysFirstWins) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?a=1&a=2";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        ASSERT_NE(got.find_value("a"), nullptr);
        EXPECT_EQ(*got.find_value("a"), "1");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, KeyIsCaseSensitive) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?Key=upper&low=1";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        ASSERT_NE(got.find_value("Key"), nullptr);
        EXPECT_EQ(*got.find_value("Key"), "upper");
        EXPECT_EQ(got.find_value("key"), nullptr);
        EXPECT_EQ(got.find_value("KEY"), nullptr);
        EXPECT_EQ(got.find_value("LOW"), nullptr);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, DistinctKeysDifferingOnlyByCase) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?a=lower&A=upper";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        ASSERT_NE(got.find_value("a"), nullptr);
        ASSERT_NE(got.find_value("A"), nullptr);
        EXPECT_EQ(*got.find_value("a"), "lower");
        EXPECT_EQ(*got.find_value("A"), "upper");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, NullPathYieldsNoPairs) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        const auto got = parse(req, nullptr, 0, 2);
        EXPECT_TRUE(got.empty());
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, QueryAtAtPathEndYieldsNoPairs) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p";
        EXPECT_TRUE(parse(req, path, sizeof(path) - 1, sizeof(path) - 1).empty());
        EXPECT_TRUE(parse(req, path, sizeof(path) - 1, 99).empty());
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, EmptyQueryStringYieldsNoPairs) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        EXPECT_TRUE(got.empty());
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, TrailingAmpersandYieldsNoEmptyKey) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?a=1&";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        EXPECT_EQ(got.size(), 1u);
        EXPECT_EQ(got.find_value(""), nullptr);
        ASSERT_NE(got.find_value("a"), nullptr);
        EXPECT_EQ(*got.find_value("a"), "1");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, LeadingAmpersandYieldsNoEmptyKey) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?&a=1";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        EXPECT_EQ(got.size(), 1u);
        EXPECT_EQ(got.find_value(""), nullptr);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, ConsecutiveAmpersandsYieldNoEmptyKey) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?a=1&&b=2";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        EXPECT_EQ(got.size(), 2u);
        EXPECT_EQ(got.find_value(""), nullptr);
        ASSERT_NE(got.find_value("b"), nullptr);
        EXPECT_EQ(*got.find_value("b"), "2");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, EmptyKeyWithValueIsKept) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?=v";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        ASSERT_NE(got.find_value(""), nullptr);
        EXPECT_EQ(*got.find_value(""), "v");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, ParsesManyPairs) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?a=1&b=2&c=3&d=4&e=5";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        EXPECT_EQ(got.size(), 5u);
        EXPECT_EQ(*got.find_value("a"), "1");
        EXPECT_EQ(*got.find_value("e"), "5");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, ViewsPointIntoPathBufferWithoutCopying) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?abc=xyz";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        const auto* const value = got.find_value("abc");
        ASSERT_NE(value, nullptr);
        EXPECT_EQ(value->data(), path + 7);
        EXPECT_EQ(*value, "xyz");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(ParseQueryReq, PercentEncodingIsPassedThroughUndecoded) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        char path[] = "/p?a=%20b&c=x+y";
        const auto got = parse(req, path, sizeof(path) - 1, 2);
        ASSERT_NE(got.find_value("a"), nullptr);
        ASSERT_NE(got.find_value("c"), nullptr);
        EXPECT_EQ(*got.find_value("a"), "%20b");
        EXPECT_EQ(*got.find_value("c"), "x+y");
    }
    h2o_mem_clear_pool(&req.pool);
}
