// Two placeholders, one argument: the static_assert in try_query fires at
// the call site.
#include <sql/pool.h>
#include <sql/query.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::try_query<"SELECT $1, $2">(pool, 1);
    (void)t;
}
