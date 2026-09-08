// transaction takes only a pool; a transaction_source is not one, so
// BEGIN inside BEGIN cannot be spelled.
#include <expected>

#include <coro/task.h>

#include <sql/pool.h>
#include <sql/transaction.h>

#include "support/fake_driver.h"

int main() {
    sql_test::fake::script s;
    const sql::pool<sql_test::fake> pool{{.connections = 1, .script_ = &s}, {}};
    auto t = sql::transaction(pool, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
        auto inner = sql::transaction(tx, [](auto) -> coro::task<std::expected<int, sql::error>> { co_return 1; });
        (void)inner;
        co_return 1;
    });
    (void)t;
}
