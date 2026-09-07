// A handler naming State<T> only belongs on Router<T>. route() rejects a
// mismatch at the point the handler is mounted rather than at extract time.
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
