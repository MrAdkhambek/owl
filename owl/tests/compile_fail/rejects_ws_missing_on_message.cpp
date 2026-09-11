#include <owl/routing/router.h>

struct App final {};

struct OnlyConnect final {
    void on_connect(owl::ws::Socket) {}
};

void force() {
    (void)owl::Router<App>::make().ws<"/x", OnlyConnect>();
}
