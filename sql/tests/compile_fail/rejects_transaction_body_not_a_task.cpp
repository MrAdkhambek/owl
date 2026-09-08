// A body returning a bare expected (no coroutine) fails the concept.
#include <expected>

#include <sql/pool.h>
#include <sql/transaction.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::transaction(pool, [](auto) -> std::expected<int, sql::error> { return 1; });
    (void)t;
}
