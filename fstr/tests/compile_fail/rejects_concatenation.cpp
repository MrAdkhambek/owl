#include <string>

#include <fstr/fstr.h>

int main() {
    const std::string extra = "bar";
    constexpr fstr::fstr s = "foo" + extra;
    return static_cast<int>(s.size());
}
