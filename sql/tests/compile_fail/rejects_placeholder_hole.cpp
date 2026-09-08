// $1 and $3 with no $2: the scanner reports the hole and the first
// static_assert in try_query fires.
#include <sql/pool.h>
#include <sql/query.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::try_query<"SELECT $1, $3">(pool, 1, 2);
    (void)t;
}
