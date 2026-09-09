#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <redis/args.h>

using redis::detail::command_args;

namespace {
    std::vector<std::string> as_strings(const command_args& a) {
        std::vector<std::string> out;
        for (const auto v : a.argv()) out.emplace_back(v);
        return out;
    }
}

TEST(Args, TextIsViewedInPlace) {
    const std::string s = "abc";
    const std::string_view v = "de";
    const char* const p = "fg";
    const command_args a{"lit", s, v, p};
    EXPECT_EQ(as_strings(a), (std::vector<std::string>{"lit", "abc", "de", "fg"}));
    EXPECT_EQ(a.argv()[1].data(), s.data());
}

TEST(Args, NumbersAreRendered) {
    const command_args a{42, -7, 3.5, std::uint64_t{18446744073709551615ull}, std::int64_t{-1}};
    EXPECT_EQ(as_strings(a), (std::vector<std::string>{"42", "-7", "3.5", "18446744073709551615", "-1"}));
}

TEST(Args, BytesAreBinarySafe) {
    const std::vector<std::byte> b{std::byte{0}, std::byte{'a'}};
    const std::span<const std::byte> s{b};
    const command_args a{"SET", b, s};
    EXPECT_EQ(a.argv()[1].size(), 2u);
    EXPECT_EQ(a.argv()[1][0], '\0');
    EXPECT_EQ(a.argv()[1][1], 'a');
    EXPECT_EQ(a.argv()[2].size(), 2u);
}

TEST(Args, RangesFlattenInPlace) {
    const std::vector<std::string> keys{"a", "b"};
    const std::array<int, 2> nums{1, 2};
    const std::vector<std::string_view> none;
    const command_args a{"DEL", keys, nums, none, "x"};
    EXPECT_EQ(as_strings(a), (std::vector<std::string>{"DEL", "a", "b", "1", "2", "x"}));
}

TEST(Args, ConceptAdmitsTheDocumentedKindsOnly) {
    static_assert(redis::arg<const char*>);
    static_assert(redis::arg<const char (&)[4]>);
    static_assert(redis::arg<std::string>);
    static_assert(redis::arg<std::string_view>);
    static_assert(redis::arg<int>);
    static_assert(redis::arg<std::uint64_t>);
    static_assert(redis::arg<double>);
    static_assert(redis::arg<std::span<const std::byte>>);
    static_assert(redis::arg<std::vector<std::byte>>);
    static_assert(redis::arg<std::vector<std::string>>);
    static_assert(redis::arg<std::array<std::string_view, 2>>);
    static_assert(redis::arg<std::vector<int>>);
    static_assert(!redis::arg<bool>);
    static_assert(!redis::arg<char>);
    static_assert(!redis::arg<std::nullptr_t>);
    static_assert(!redis::arg<std::vector<std::vector<std::string>>>);
    static_assert(!redis::arg<std::vector<bool>>);
}
