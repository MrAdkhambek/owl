// The body must return task<expected<T, error>>; task<int> fails the
// transaction_body concept.
#include <coro/task.h>

#include <sql/pool.h>
#include <sql/transaction.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::transaction(pool, [](auto) -> coro::task<int> { co_return 1; });
    (void)t;
}
