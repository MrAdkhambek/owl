#include <fstr/fstr.h>

int main() {
    fstr::fstr s = "hel\0lo";
    return static_cast<int>(s.size());
}
