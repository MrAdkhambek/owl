#include <sql/placeholders.h>

int main() {
    constexpr auto n = sql::placeholder_count<"SELECT * FROM t WHERE k = ?5">;
    return static_cast<int>(n);
}
