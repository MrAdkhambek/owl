#include <sql/placeholders.h>

int main() {
    constexpr auto n = sql::placeholder_count<"SELECT ? AND $1">;
    return static_cast<int>(n);
}
