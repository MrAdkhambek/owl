#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <sql/bind.h>

namespace {
    TEST(Binds, ScalarsOwnTheirValue) {
        const auto binds = sql::detail::make_binds(1, 2.5, true, std::int64_t{-7});

        ASSERT_EQ(binds.size(), 4);
        EXPECT_EQ(std::get<std::int64_t>(binds[0].value), 1);
        EXPECT_EQ(std::get<double>(binds[1].value), 2.5);
        EXPECT_EQ(std::get<std::int64_t>(binds[2].value), 1);
        EXPECT_EQ(std::get<std::int64_t>(binds[3].value), -7);
    }

    TEST(Binds, StringsCopyEagerly) {
        std::string name = "ada";
        const auto binds = sql::detail::make_binds(name, std::string_view{"bob"}, "cat");

        EXPECT_EQ(std::get<std::string>(binds[0].value), "ada");
        EXPECT_EQ(std::get<std::string>(binds[1].value), "bob");
        EXPECT_EQ(std::get<std::string>(binds[2].value), "cat");
    }

    TEST(Binds, NullsAndOptionals) {
        const auto binds = sql::detail::make_binds(nullptr, std::optional<std::string>{}, std::optional<int>{3});

        EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(binds[0].value));
        EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(binds[1].value));
        EXPECT_EQ(std::get<std::int64_t>(binds[2].value), 3);
    }

    TEST(Binds, BlobsRoundTrip) {
        const std::vector<std::byte> blob{std::byte{1}, std::byte{0}, std::byte{2}};

        const auto& stored = std::get<std::vector<std::byte>>(sql::detail::make_binds(blob)[0].value);

        EXPECT_EQ(stored, blob);
    }

    TEST(Binds, NoBindsIsAnEmptyArray) {
        EXPECT_EQ(sql::detail::make_binds().size(), 0);
    }
}
