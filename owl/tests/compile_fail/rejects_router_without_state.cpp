// The Server accepts only a Router<void>. A router that still needs its state
// must not reach it -- with_state() is what proves the state was supplied.
#include <owl/server.h>

namespace {
    struct App {
        int n = 0;
    };

    owl::Response ping(owl::State<App>) {
        return owl::Response::ok("pong");
    }
}

int main() {
    owl::Server server = owl::Server::builder()
                         .router(owl::Router<App>::make().route<"/ping">(owl::get(ping)))
                         .build();
    (void)server;
}
