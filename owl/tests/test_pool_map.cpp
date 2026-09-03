#include <cstddef>
#include <gtest/gtest.h>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <owl/util/pool_map.h>

TEST(PoolMap, ComputeIfAbsentInsertsOnce) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        owl::pool_map map{&req};

        int n = 0;
        std::string_view& a = map.compute_if_absent("k", [&] {
            ++n;
            return std::string_view{"v"};
        });
        std::string_view& b = map.compute_if_absent("k", [&] {
            ++n;
            return std::string_view{"x"};
        });

        EXPECT_EQ(n, 1);
        EXPECT_EQ(a, "v");
        EXPECT_EQ(b, "v");
        EXPECT_EQ(&a, &b);
        EXPECT_TRUE(map.contains("k"));
        EXPECT_FALSE(map.contains("missing"));
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(PoolMap, DistinctKeys) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        owl::pool_map map{&req};

        EXPECT_EQ(map.compute_if_absent("a", [] { return std::string_view{"1"}; }), "1");
        EXPECT_EQ(map.compute_if_absent("b", [] { return std::string_view{"2"}; }), "2");
        EXPECT_EQ(map.compute_if_absent("a", [] { return std::string_view{"nope"}; }), "1");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(PoolMap, NodesAreAllocatedFromReqPool) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    EXPECT_EQ(req.pool.chunks, nullptr);
    {
        owl::pool_map map{&req};
        map.compute_if_absent("k", [] { return std::string_view{"v"}; });
        EXPECT_NE(req.pool.chunks, nullptr);

        const auto* const value = map.find_value("k");
        ASSERT_NE(value, nullptr);
        const auto* const sentinel = static_cast<char*>(h2o_mem_alloc_pool(&req.pool, char, 1));
        const auto* const node = reinterpret_cast<const char*>(value);
        const auto distance = static_cast<std::size_t>(node > sentinel ? node - sentinel : sentinel - node);
        EXPECT_LT(distance, 8192u);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(PoolMap, RangesToCollectsPairsFirstWins) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    const std::vector<std::pair<std::string_view, std::string_view>> pairs{
        {"a", "1"},
        {"b", "two"},
        {"a", "3"},
    };
    {
        const auto map = pairs | std::ranges::to<owl::pool_map>(&req);
        EXPECT_EQ(*map.find_value("a"), "1");
        EXPECT_EQ(*map.find_value("b"), "two");
        EXPECT_EQ(map.find_value("missing"), nullptr);
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(PoolMap, KeysAreCaseSensitive) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    {
        owl::pool_map map{&req};
        map.compute_if_absent("Key", [] { return std::string_view{"upper"}; });
        map.compute_if_absent("key", [] { return std::string_view{"lower"}; });

        ASSERT_NE(map.find_value("Key"), nullptr);
        ASSERT_NE(map.find_value("key"), nullptr);
        EXPECT_EQ(*map.find_value("Key"), "upper");
        EXPECT_EQ(*map.find_value("key"), "lower");
        EXPECT_EQ(map.find_value("KEY"), nullptr);
        EXPECT_FALSE(map.contains("KEY"));
    }
    h2o_mem_clear_pool(&req.pool);
}
