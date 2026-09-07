#include <gtest/gtest.h>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <owl/util/pool_map.h>

TEST(PoolMap, ComputeIfAbsentInsertsOnce) {
    owl::pool_map map;

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

TEST(PoolMap, DistinctKeys) {
    owl::pool_map map;

    EXPECT_EQ(map.compute_if_absent("a", [] { return std::string_view{"1"}; }), "1");
    EXPECT_EQ(map.compute_if_absent("b", [] { return std::string_view{"2"}; }), "2");
    EXPECT_EQ(map.compute_if_absent("a", [] { return std::string_view{"nope"}; }), "1");
}

TEST(PoolMap, RangesToCollectsPairsFirstWins) {
    const std::vector<std::pair<std::string_view, std::string_view>> pairs{
        {"a", "1"},
        {"b", "two"},
        {"a", "3"},
    };

    const auto map = pairs | std::ranges::to<owl::pool_map>();
    EXPECT_EQ(*map.find_value("a"), "1");
    EXPECT_EQ(*map.find_value("b"), "two");
    EXPECT_EQ(map.find_value("missing"), nullptr);
}

TEST(PoolMap, KeysAreCaseSensitive) {
    owl::pool_map map;
    map.compute_if_absent("Key", [] { return std::string_view{"upper"}; });
    map.compute_if_absent("key", [] { return std::string_view{"lower"}; });

    ASSERT_NE(map.find_value("Key"), nullptr);
    ASSERT_NE(map.find_value("key"), nullptr);
    EXPECT_EQ(*map.find_value("Key"), "upper");
    EXPECT_EQ(*map.find_value("key"), "lower");
    EXPECT_EQ(map.find_value("KEY"), nullptr);
    EXPECT_FALSE(map.contains("KEY"));
}
