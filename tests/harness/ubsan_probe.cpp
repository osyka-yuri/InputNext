#include <limits>

int main() {
    volatile int limit = std::numeric_limits<int>::max();
    volatile int one = 1;
    [[maybe_unused]] volatile int result = limit + one;
    return 0;
}
