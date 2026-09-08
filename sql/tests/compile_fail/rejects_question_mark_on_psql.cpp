#include <sql/psql.h>
#include <sql/query.h>

int main() {
    sql::pool<sql::psql> pool{sql::psql::config{.dsn = "postgres://localhost/app"}};
    // ? is the sqlite style; a postgres pool takes $n.
    auto work = sql::query<"SELECT ?">(pool, 1);
    return 0;
}
