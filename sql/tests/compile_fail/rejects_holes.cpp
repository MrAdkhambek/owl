#include <sql/placeholders.h>

int main() {
    constexpr auto n = sql::placeholder_count<"SELECT * FROM t WHERE a = $1 AND b = $3">;
    return static_cast<int>(n);
}
