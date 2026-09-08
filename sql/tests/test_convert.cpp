#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sql/convert.h>

namespace {
    const sql::blob bytes{std::byte{0x00}, std::byte{0xab}, std::byte{0xff}};
}

TEST(Bindable, AdmitsTheVocabulary) {
    static_assert(sql::bindable<bool>);
    static_assert(sql::bindable<int>);
    static_assert(sql::bindable<std::int64_t>);
    static_assert(sql::bindable<unsigned>);
    static_assert(sql::bindable<double>);
    static_assert(sql::bindable<float>);
    static_assert(sql::bindable<std::string>);
    static_assert(sql::bindable<std::string_view>);
    static_assert(sql::bindable<const char*>);
    static_assert(sql::bindable<char[6]>);
    static_assert(sql::bindable<const char[6]>);
    static_assert(sql::bindable<sql::blob>);
    static_assert(sql::bindable<sql::blob_view>);
    static_assert(sql::bindable<std::nullopt_t>);
    static_assert(sql::bindable<std::optional<int>>);
    static_assert(sql::bindable<std::optional<std::string>>);
    static_assert(sql::bindable<const int&>);
}

TEST(Bindable, RejectsWhatIsNotSql) {
    static_assert(!sql::bindable<char>);
    static_assert(!sql::bindable<std::vector<int>>);
    static_assert(!sql::bindable<std::optional<std::optional<int>>>);
    static_assert(!sql::bindable<void*>);
}

TEST(ToText, Scalars) {
    EXPECT_EQ(sql::to_text(true), "true");
    EXPECT_EQ(sql::to_text(false), "false");
    EXPECT_EQ(sql::to_text(42), "42");
    EXPECT_EQ(sql::to_text(std::int64_t{-9007199254740993}), "-9007199254740993");
    EXPECT_EQ(sql::to_text(0.5), "0.5");
    EXPECT_EQ(sql::to_text(std::string{"abc"}), "abc");
    EXPECT_EQ(sql::to_text(std::string_view{"abc"}), "abc");
    EXPECT_EQ(sql::to_text("lit"), "lit");
    EXPECT_EQ(sql::to_text(bytes), "\\x00abff");
    EXPECT_EQ(sql::to_text(sql::blob_view{bytes}), "\\x00abff");
}

TEST(ToText, NullSpellings) {
    EXPECT_EQ(sql::to_text(std::nullopt), std::nullopt);
    EXPECT_EQ(sql::to_text(std::optional<int>{}), std::nullopt);
    EXPECT_EQ(sql::to_text(std::optional<int>{7}), "7");
}

TEST(FromText, Numbers) {
    EXPECT_EQ(sql::from_text<int>("42").value(), 42);
    EXPECT_EQ(sql::from_text<std::int64_t>("-9007199254740993").value(), std::int64_t{-9007199254740993});
    EXPECT_DOUBLE_EQ(sql::from_text<double>("0.5").value(), 0.5);
    EXPECT_EQ(sql::from_text<unsigned>("7").value(), 7u);
}

TEST(FromText, WholeTextOrNothing) {
    EXPECT_EQ(sql::from_text<int>("12abc").error().kind(), sql::error_kind::conversion);
    EXPECT_EQ(sql::from_text<int>("").error().kind(), sql::error_kind::conversion);
    EXPECT_EQ(sql::from_text<int>(" 1").error().kind(), sql::error_kind::conversion);
    EXPECT_EQ(sql::from_text<std::int8_t>("300").error().kind(), sql::error_kind::conversion);
}

TEST(FromText, Booleans) {
    for (const auto t : {"t", "true", "1"}) EXPECT_TRUE(sql::from_text<bool>(t).value()) << t;
    for (const auto f : {"f", "false", "0"}) EXPECT_FALSE(sql::from_text<bool>(f).value()) << f;
    EXPECT_EQ(sql::from_text<bool>("yes").error().kind(), sql::error_kind::conversion);
}

TEST(FromText, Strings) {
    const std::string_view text = "hello";
    EXPECT_EQ(sql::from_text<std::string>(text).value(), "hello");
    EXPECT_EQ(sql::from_text<std::string_view>(text).value().data(), text.data());
}

TEST(FromText, ByteaHex) {
    EXPECT_EQ(sql::from_text<sql::blob>("\\x00abff").value(), bytes);
    EXPECT_EQ(sql::from_text<sql::blob>("\\x00ABFF").value(), bytes);
    EXPECT_TRUE(sql::from_text<sql::blob>("\\x").value().empty());
    EXPECT_EQ(sql::from_text<sql::blob>("00ab").error().kind(), sql::error_kind::conversion);
    EXPECT_EQ(sql::from_text<sql::blob>("\\x0").error().kind(), sql::error_kind::conversion);
    EXPECT_EQ(sql::from_text<sql::blob>("\\xzz").error().kind(), sql::error_kind::conversion);
}
