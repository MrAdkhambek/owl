#include <owl/routing/router.h>

struct App final {};

coro::task<void> bad(owl::ws::Socket, owl::PathView<"id">) { co_return; }

void force() {
    (void)owl::Router<App>::make().ws<"/x/{id}">(bad);
}
