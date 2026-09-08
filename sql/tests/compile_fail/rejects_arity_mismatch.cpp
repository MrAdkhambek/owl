#include <sql/sqlite.h>
#include <sql/query.h>

int main() {
    sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};
    auto work = sql::query<"SELECT ? AND ?">(pool, 1);
    return 0;
}
