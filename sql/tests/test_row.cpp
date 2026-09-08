#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <sql/detail/table.h>
#include <sql/result.h>

namespace {
    sql::detail::table make_users() {
        sql::detail::table t;
        t.add_column("id");
        t.add_column("name");
        t.add_column("nick");
        t.add_cell("42", false);
        t.add_cell("ada", false);
        t.add_cell("", true);
        t.end_row();
        t.add_cell("7", false);
        t.add_cell("", true);
        t.add_cell("grace", false);
        t.end_row();
        return t;
    }

    TEST(Result, IteratesRowsAndColumns) {
        const sql::result r{make_users(), sql::dialect::sqlite};

        ASSERT_EQ(r.size(), 2);
        ASSERT_EQ(r.columns(), 3);
        EXPECT_EQ(r.name(1), "name");
        EXPECT_EQ(r.index("nick"), 2);
        EXPECT_EQ(r[0][0].as<int>(), 42);
        EXPECT_EQ(r[1]["name"].optional<std::string>(), std::nullopt);
        EXPECT_TRUE(r[0][2] == sql::null);
        EXPECT_FALSE(r[0][1] == sql::null);
    }

    TEST(Result, RowTupleConsumesColumnsLeftToRight) {
        const sql::result r{make_users(), sql::dialect::sqlite};

        const auto [id, name, nick] = r[0].as<std::tuple<int, std::string, std::optional<std::string>>>();
        EXPECT_EQ(id, 42);
        EXPECT_EQ(name, "ada");
        EXPECT_EQ(nick, std::nullopt);
    }

    TEST(Result, VectorTakesFirstColumn) {
        const sql::result r{make_users(), sql::dialect::sqlite};

        EXPECT_EQ(r.vector<int>(), (std::vector<int>{42, 7}));
    }

    TEST(Result, EmptyResult) {
        const sql::result r;

        EXPECT_TRUE(r.empty());
        EXPECT_EQ(r.optional<int>(), std::nullopt);
    }

    TEST(Result, MisuseThrows) {
        const sql::result r{make_users(), sql::dialect::sqlite};

        EXPECT_THROW((void)r.at(9), std::out_of_range);
        EXPECT_THROW((void)r[0]["nope"], std::out_of_range);
        EXPECT_THROW((static_cast<void>(r.tuple<std::tuple<int, std::string>>())), std::runtime_error);
    }

    TEST(Result, BoolDecodesPerDialect) {
        sql::detail::table t;
        t.add_column("b");
        t.add_cell("t", false);
        t.end_row();

        EXPECT_TRUE((sql::result{std::move(t), sql::dialect::postgres}[0][0].as<bool>()));
    }
}
