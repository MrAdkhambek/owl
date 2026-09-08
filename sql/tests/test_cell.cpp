#include <gtest/gtest.h>

#include <string>
#include <type_traits>

#include <sql/cell.h>
#include <sql/dialect.h>
#include <sql/error.h>
#include <sql/null.h>

namespace {
    // The sentinel contract: writers guarantee one past the end is NUL, so a
    // cell holding embedded NULs still reads as a C string up to its length.
    TEST(Cell, BytesCarryLengthAndSentinel) {
        std::string backing{'a', '\0', 'b', '\0'};
        const sql::cell c{.bytes = backing, .from = sql::dialect::sqlite, .is_null = false};

        EXPECT_EQ(c.bytes.size(), 4);
        EXPECT_EQ(c.bytes[1], '\0');
        EXPECT_EQ(c.bytes.data()[4], '\0');
        EXPECT_FALSE(c.is_null);
    }

    TEST(Cell, NullCellHasEmptyBytesAndDialect) {
        const sql::cell c{.bytes = {}, .from = sql::dialect::postgres, .is_null = true};

        EXPECT_TRUE(c.bytes.empty());
        EXPECT_EQ(c.from, sql::dialect::postgres);
        EXPECT_TRUE(c.is_null);
    }

    TEST(Error, FieldsHold) {
        const sql::error e{.message = "UNIQUE constraint failed", .sqlstate = "", .code = 19};

        EXPECT_EQ(e.code, 19);
        EXPECT_TRUE(e.sqlstate.empty());
    }

    TEST(Null, TagIsAConstant) {
        static_assert(std::is_same_v<decltype(sql::null), const sql::null_t>);
    }
}
