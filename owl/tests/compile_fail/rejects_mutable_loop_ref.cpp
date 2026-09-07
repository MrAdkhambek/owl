// A handler parameter taken by reference must be const: extraction sees the
// Context as const, so a mutable owl::loop_scheduler& has nothing to bind.
// The refusal happens when the handler is mounted, not at extract time.
#include <owl/coro/loop_scheduler.h>
#include <owl/routing/router.h>

namespace {
    owl::Response mutate(owl::loop_scheduler& loop) {
        loop.post({});
        return owl::Response::ok("no", 400);
    }
}

int main() {
    auto router = owl::Router<int>::make().route<"/mutate">(owl::get(mutate));
    (void)router;
}
