#include <gtest/gtest.h>

#include <sql/placeholders.h>

namespace {
    using sql::dialect;

    static_assert(sql::placeholder_count<"SELECT 1"> == 0);
    static_assert(!sql::placeholder_style<"SELECT 1">.has_value());

    static_assert(sql::placeholder_count<"SELECT * FROM t WHERE a = ? AND b = ?"> == 2);
    static_assert(sql::placeholder_style<"SELECT * FROM t WHERE a = ?"> == dialect::sqlite);

    static_assert(sql::placeholder_count<"SELECT * FROM t WHERE a = $2 AND b = $1"> == 2);
    static_assert(sql::placeholder_style<"SELECT $1"> == dialect::postgres);

    // Postgres casts are not bind sites.
    static_assert(sql::placeholder_count<"SELECT $1::int"> == 1);
    static_assert(sql::placeholder_count<"SELECT a::text FROM t"> == 0);

    TEST(Placeholders, RuntimeValuesMatchTheConstantFoldedOnes) {
        EXPECT_EQ(sql::placeholder_count<"SELECT ?, ?">, 2);
        EXPECT_EQ(*sql::placeholder_style<"SELECT $1">, dialect::postgres);
    }
}
