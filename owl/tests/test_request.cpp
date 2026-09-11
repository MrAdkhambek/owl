#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <owl/http/request.h>

static_assert(std::is_same_v<decltype(std::declval<const owl::Request&>().raw()), const h2o_req_t*>);

namespace {
    owl::Request* make_request(h2o_req_t& req, const char* const method, char* const path, const std::size_t path_len, const std::size_t query_at) {
        req.method = {.base = const_cast<char*>(method), .len = std::char_traits<char>::length(method)};
        req.path = {.base = path, .len = path_len};
        req.query_at = query_at;
        return owl::Request::make(&req);
    }
}

TEST(Request, FromParsesMethod) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    const char token[] = "POST";
    req.method = {const_cast<char*>(token), sizeof(token) - 1};
    req.query_at = SIZE_MAX;
    {
        const auto r = owl::Request::make(&req);
        EXPECT_EQ(r->method(), owl::Method::Post);
        EXPECT_EQ(r->raw(), &req);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, FromUnknownMethod) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    const char token[] = "get";
    req.method = {const_cast<char*>(token), sizeof(token) - 1};
    req.query_at = SIZE_MAX;
    {
        EXPECT_EQ(owl::Request::make(&req)->method(), owl::Method::Unknown);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, QueryMissingWhenNoQuestionMark) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    char path[] = "/only";
    {
        const auto r = make_request(req, "GET", path, sizeof(path) - 1, SIZE_MAX);
        EXPECT_EQ(r->query("a"), std::nullopt);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, FromWithoutQueryDoesNotParsePathAsQuery) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    char path[] = "/foo=bar&x=1";
    {
        const auto r = make_request(req, "GET", path, sizeof(path) - 1, SIZE_MAX);
        EXPECT_EQ(r->query("foo"), std::nullopt);
        EXPECT_EQ(r->query("x"), std::nullopt);
        EXPECT_EQ(r->method(), owl::Method::Get);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, QueryFindsFirstMatch) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    char path[] = "/p?a=1&b=two&a=3";
    {
        const auto r = make_request(req, "GET", path, sizeof(path) - 1, 2);
        EXPECT_EQ(r->query("a"), "1");
        EXPECT_EQ(r->query("a"), "1");
        EXPECT_EQ(r->query("b"), "two");
        EXPECT_EQ(r->query("c"), std::nullopt);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, QueryKeyWithoutValue) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    char path[] = "/p?flag&x=1";
    {
        const auto r = make_request(req, "GET", path, sizeof(path) - 1, 2);
        EXPECT_EQ(r->query("flag"), "");
        EXPECT_EQ(r->query("x"), "1");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, QueryNameIsCaseSensitive) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    char path[] = "/p?Key=upper&low=1";
    {
        const auto r = make_request(req, "GET", path, sizeof(path) - 1, 2);
        EXPECT_EQ(r->query("Key"), "upper");
        EXPECT_EQ(r->query("key"), std::nullopt);
        EXPECT_EQ(r->query("KEY"), std::nullopt);
        EXPECT_EQ(r->query("low"), "1");
        EXPECT_EQ(r->query("LOW"), std::nullopt);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, HeaderRepeatedLookupsReturnSameValue) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    h2o_add_header_by_str(&req.pool, &req.headers, "host", 4, 1, nullptr, "ex.com", 6);
    req.query_at = SIZE_MAX;
    {
        const auto r = owl::Request::make(&req);
        EXPECT_EQ(r->header("host"), "ex.com");
        EXPECT_EQ(r->header("host"), "ex.com");
        EXPECT_EQ(r->header("missing"), std::nullopt);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, HeaderNameIsCaseInsensitive) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    h2o_add_header_by_str(&req.pool, &req.headers, "host", 4, 1, nullptr, "ex.com", 6);
    char path[] = "/p?";
    {
        const auto r = make_request(req, "GET", path, sizeof(path) - 1, 2);
        EXPECT_EQ(r->header("Host"), "ex.com");
        EXPECT_EQ(r->header("HOST"), "ex.com");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, HeaderNameMatchesNonAsciiBytes) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    const char name[] = "x-caf\xc3\xa9";
    h2o_add_header_by_str(&req.pool, &req.headers, name, sizeof(name) - 1, 1, nullptr, "v", 1);
    req.query_at = SIZE_MAX;
    {
        const auto r = owl::Request::make(&req);
        EXPECT_EQ(r->header(std::string_view{name, sizeof(name) - 1}), "v");
        EXPECT_EQ(r->header("X-CAF\xc3\xa9"), "v");
        EXPECT_EQ(r->header("x-caf\xc3\xa8"), std::nullopt);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, HeaderMissingNameOnEmptyHeaderList) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    {
        const auto r = owl::Request::make(&req);
        EXPECT_EQ(r->header("host"), std::nullopt);
        EXPECT_EQ(r->header("Host"), std::nullopt);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, StopTokenIsLiveWhileRequestExists) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    {
        const auto* const r = owl::Request::make(&req);
        const auto token = r->stop_source().get_token();
        EXPECT_TRUE(token.stop_possible());
        EXPECT_FALSE(token.stop_requested());
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, DisposingRequestCancelsItsStopToken) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    std::stop_token token;
    {
        const auto* const r = owl::Request::make(&req);
        token = r->stop_source().get_token();
        EXPECT_FALSE(token.stop_requested());
    }
    h2o_mem_clear_pool(&req.pool);
    EXPECT_TRUE(token.stop_requested());
}

TEST(Request, AllTokensShareTheRequestCancellation) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    std::stop_token first;
    std::stop_token second;
    {
        const auto* const r = owl::Request::make(&req);
        first = r->stop_source().get_token();
        second = r->stop_source().get_token();
    }
    h2o_mem_clear_pool(&req.pool);
    EXPECT_TRUE(first.stop_requested());
    EXPECT_TRUE(second.stop_requested());
}

TEST(Request, HeaderFindsNonTokenNameCaseInsensitively) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    h2o_add_header_by_str(&req.pool, &req.headers, "x-custom", 8, 1, nullptr, "v", 1);
    req.query_at = SIZE_MAX;
    {
        const auto r = owl::Request::make(&req);
        EXPECT_EQ(r->header("x-custom"), "v");
        EXPECT_EQ(r->header("X-Custom"), "v");
        EXPECT_EQ(r->header("X-CUSTOM"), "v");
        EXPECT_EQ(r->header("x-custo"), std::nullopt);
        EXPECT_EQ(r->header("x-customx"), std::nullopt);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(Request, FormatsAsMethodAndPath) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    const char token[] = "GET";
    req.method = {const_cast<char*>(token), sizeof(token) - 1};
    char path[] = "/users/42";
    req.path_normalized = {.base = path, .len = sizeof(path) - 1};
    req.query_at = SIZE_MAX;
    {
        const auto r = owl::Request::make(&req);
        EXPECT_EQ(std::format("{}", *r), "GET /users/42");
        EXPECT_EQ(std::format("{:>20}", *r), "       GET /users/42");
    }
    h2o_mem_clear_pool(&req.pool);
}
