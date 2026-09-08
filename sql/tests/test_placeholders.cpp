#include <gtest/gtest.h>

#include <string_view>

#include <fstr/fstr.h>

#include <sql/detail/placeholders.h>
#include <sql/detail/stmt_key.h>

using sql::detail::scan_placeholders;

TEST(Placeholders, CountsContiguousNumbers) {
    static_assert(scan_placeholders("SELECT 1").count == 0);
    static_assert(scan_placeholders("SELECT 1").error == nullptr);
    static_assert(scan_placeholders("SELECT $1").count == 1);
    static_assert(scan_placeholders("SELECT $1, $2, $3").count == 3);
    static_assert(scan_placeholders("SELECT $2, $1").count == 2);
    static_assert(scan_placeholders("SELECT $1 + $1").count == 1);
    static_assert(scan_placeholders("SELECT $10, $1, $2, $3, $4, $5, $6, $7, $8, $9").count == 10);
    EXPECT_EQ(scan_placeholders("SELECT $1, $2").count, 2u);
}

TEST(Placeholders, QuotedTextIsSkipped) {
    static_assert(scan_placeholders("SELECT '$1'").count == 0);
    static_assert(scan_placeholders("SELECT 'it''s $9', $1").count == 1);
    static_assert(scan_placeholders("SELECT '$2', $1").error == nullptr);
}

TEST(Placeholders, BareDollarIsNotAPlaceholder) {
    static_assert(scan_placeholders("SELECT $$ a $$").count == 0);
    static_assert(scan_placeholders("SELECT $abc").count == 0);
    static_assert(scan_placeholders("SELECT $").count == 0);
}

TEST(Placeholders, HolesAndBadNumbersAreErrors) {
    static_assert(scan_placeholders("SELECT $1, $3").error != nullptr);
    static_assert(scan_placeholders("SELECT $2").error != nullptr);
    static_assert(scan_placeholders("SELECT $0").error != nullptr);
    static_assert(scan_placeholders("SELECT $65").error != nullptr);
    EXPECT_STREQ(scan_placeholders("SELECT $1, $3").error, "placeholders must be contiguous from $1");
    EXPECT_STREQ(scan_placeholders("SELECT $0").error, "placeholder numbering starts at $1");
    EXPECT_STREQ(scan_placeholders("SELECT $65").error, "at most 64 placeholders");
}

TEST(StmtKey, OneIdentityPerLiteral) {
    using a = sql::detail::stmt_key<"SELECT $1">;
    using b = sql::detail::stmt_key<"SELECT $1">;
    using c = sql::detail::stmt_key<"SELECT $2, $1">;
    static_assert(a::id == b::id);
    static_assert(a::id != c::id);
    static_assert(a::text == std::string_view{"SELECT $1"});
    static_assert(a::scan.count == 1);
    static_assert(c::scan.count == 2);
    EXPECT_STREQ(a::c_str, "SELECT $1");
    EXPECT_EQ(a::c_str[a::text.size()], '\0');
}
