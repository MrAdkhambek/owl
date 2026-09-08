#include <gtest/gtest.h>

#include <stdexcept>

#include <sql/error.h>

TEST(Error, CarriesKindMessageSqlstateAndCode) {
    const sql::error e{sql::error_kind::query, "syntax error", "42601", 7};
    EXPECT_EQ(e.kind(), sql::error_kind::query);
    EXPECT_STREQ(e.what(), "syntax error");
    EXPECT_EQ(e.sqlstate(), "42601");
    EXPECT_EQ(e.code(), 7);
}

TEST(Error, DefaultsForNonDriverFailures) {
    const sql::error e{sql::error_kind::closed, "pool is closed"};
    EXPECT_TRUE(e.sqlstate().empty());
    EXPECT_EQ(e.code(), 0);
}

TEST(Error, IsARuntimeError) {
    EXPECT_THROW(throw sql::error(sql::error_kind::timeout, "late"), std::runtime_error);
}

TEST(Error, KindNames) {
    EXPECT_EQ(sql::to_string(sql::error_kind::connect), "connect");
    EXPECT_EQ(sql::to_string(sql::error_kind::conversion), "conversion");
}
