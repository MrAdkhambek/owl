#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <vector>

#include <sql/cell.h>
#include <sql/detail/table.h>
#include <sql/dialect.h>
#include <sql/error.h>
#include <sql/null.h>
#include <sql/result.h>

#ifdef OWL_ENABLE_POSTGRESQL
#include <sql/psql.h>
#endif

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

    TEST(Cell, BlobRoundTripSurvivesEmbeddedNul) {
        sql::detail::table t;
        t.add_column("blob");
        const std::string raw{'a', '\0', 'b'};
        t.add_cell(raw, false);
        t.end_row();

        const sql::result r{std::move(t), sql::dialect::sqlite};
        const auto blob = r[0][0].as<std::vector<std::byte>>();
        ASSERT_EQ(blob.size(), 3);
        EXPECT_EQ(blob[1], std::byte{0});
        EXPECT_EQ(r[0][0].bytes().size(), 3);
    }
}

#ifdef OWL_ENABLE_POSTGRESQL
namespace sql_detail_decode {
    TEST(Cell, ByteaHexAndEscapeBothDecode) {
        const auto hex = sql::detail::decode_bytea("\\x48656c6c6f00");
        ASSERT_EQ(hex.size(), 6);
        EXPECT_EQ(hex[0], 'H');
        EXPECT_EQ(hex[5], '\0');

        const auto escaped = sql::detail::decode_bytea("Hello\\000");
        ASSERT_EQ(escaped.size(), 6);
        EXPECT_EQ(escaped[5], '\0');
    }
}
#endif
