#include <coro/task.h>

#include <redis/client.h>

struct point final {
    int x;
};

coro::task<> f(const redis::client& c) {
    (void)co_await c.command("SET", "k", point{1});
}

int main() {
}
