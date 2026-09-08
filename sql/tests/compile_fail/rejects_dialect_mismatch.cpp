#include <sql/sqlite.h>
#include <sql/query.h>

int main() {
    sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};
    // $1 is the postgres style; a sqlite pool takes ?.
    auto work = sql::query<"SELECT $1">(pool, 1);
    return 0;
}
