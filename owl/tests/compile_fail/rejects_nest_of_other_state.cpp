// nest() takes a router over the same state type. A sub-app carrying a
// different state has nowhere to put it: match() hands the request one state,
// the one bound at the root.
#include <owl/routing/router.h>

namespace {
    struct App {
        int n = 0;
    };

    struct Other {
        int n = 0;
    };

    owl::Response ping() {
        return owl::Response::ok("pong");
    }
}

int main() {
    auto inner = owl::Router<Other>::make().route<"/ping">(owl::get(ping));
    auto router = owl::Router<App>::make().nest<"/api">(std::move(inner));
    (void)router;
}
