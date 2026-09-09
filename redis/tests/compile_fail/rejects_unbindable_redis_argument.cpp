#include <coro/task.h>

#include <redis/client.h>
#include <redis/command.h>

struct point final {
    int x;
};

coro::task<> f(const redis::client& c) {
    (void)co_await redis::command(c, "SET", "k", point{1});
}

int main() {
}
