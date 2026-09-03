// FromContext<State<T>> casts the erased state pointer without checking it, so
// a handler naming a state type the router does not carry must be rejected at
// the point it is routed rather than reinterpreted per request.
#include <owl/routing/router.h>

namespace {
    struct App {
        int n = 0;
    };

    struct Other {
        int n = 0;
    };

    owl::Response read_other(owl::State<Other>) {
        return owl::Response::ok("no", 400);
    }
}

int main() {
    auto router = owl::Router<App>::make().route<"/n">(owl::get(read_other));
    (void)router;
}
