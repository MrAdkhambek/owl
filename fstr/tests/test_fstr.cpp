#include <concepts>
#include <format>
#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <unordered_set>

#include <fstr/fstr.h>

namespace {
    constexpr fstr::fstr hello = "hello";
    constexpr fstr::fstr empty = "";
    constexpr fstr::fstr joined = "foo" "bar";
    constexpr fstr::fstr raw = R"(a
b)";
}

static_assert(hello.view() == "hello");
static_assert(hello.size() == 5);
static_assert(!hello.empty());
static_assert(empty.empty());
static_assert(empty.size() == 0);
static_assert(joined.view() == "foobar");
static_assert(raw.view() == "a\nb");

static_assert(std::string_view{hello} == "hello");

static_assert(!std::constructible_from<fstr::fstr<6>, std::string>);
static_assert(!std::constructible_from<fstr::fstr<6>, std::string_view>);
static_assert(!std::constructible_from<fstr::fstr<6>, const char*>);
static_assert(!std::constructible_from<fstr::fstr<6>, char*>);

namespace {
    template <fstr::fstr S>
    struct tag {
        static constexpr std::string_view text = S.view();
    };

    static_assert(std::same_as<tag<"ab">, tag<"ab">>);
    static_assert(!std::same_as<tag<"ab">, tag<"ac">>);
    static_assert(!std::same_as<tag<"ab">, tag<"ab ">>);
}

static_assert(tag<"hello">::text == "hello");
static_assert(tag<hello>::text == "hello");

static_assert(fstr::fstr{"abc"} == fstr::fstr{"abc"});
static_assert(fstr::fstr{"abc"} != fstr::fstr{"abd"});

TEST(Fstr, ViewOmitsTrailingNul) {
    EXPECT_EQ(hello.view(), "hello");
    EXPECT_EQ(hello.size(), 5u);
}

TEST(Fstr, AdjacentAndRawLiterals) {
    EXPECT_EQ(joined.view(), "foobar");
    EXPECT_EQ(raw.view(), "a\nb");
}

TEST(Fstr, ConvertsToStdString) {
    EXPECT_EQ(static_cast<std::string>(hello), "hello");
}

TEST(Fstr, FormatsAsStringNotAsCharRange) {
    EXPECT_EQ(std::format("{}", hello), "hello");
}

TEST(Fstr, FormatsWithStringSpecs) {
    EXPECT_EQ(std::format("{:>8}", fstr::fstr{"hi"}), "      hi");
}

TEST(Fstr, HashMatchesStringView) {
    EXPECT_EQ(std::hash<fstr::fstr<6>>{}(hello), std::hash<std::string_view>{}(hello.view()));
}

TEST(Fstr, EqualValuesHaveEqualHashes) {
    EXPECT_EQ(std::hash<fstr::fstr<6>>{}(hello), std::hash<fstr::fstr<6>>{}(fstr::fstr{"hello"}));
}

TEST(Fstr, WorksAsUnorderedSetKey) {
    const std::unordered_set<fstr::fstr<6>> set{hello};
    EXPECT_TRUE(set.contains(fstr::fstr{"hello"}));
}
