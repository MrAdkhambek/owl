// A std::vector<int> is not in the bindable vocabulary, so the constraint
// on Args fails.
#include <vector>

#include <sql/pool.h>
#include <sql/query.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::try_query<"SELECT $1">(pool, std::vector<int>{1});
    (void)t;
}
