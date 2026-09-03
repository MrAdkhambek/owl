#include <cstdio>

#include <fstr/fstr.h>

int main() {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "hello %d", 1);
    fstr::fstr s = buffer;
    return static_cast<int>(s.size());
}
