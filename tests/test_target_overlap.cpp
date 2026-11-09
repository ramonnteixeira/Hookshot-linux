#include "../gpu/vk/target_overlap.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using mkwvk::target_overlap::shifted_over;
    // (OoT's 768 x 432 RGBA16F transient targets: 2.6 MB each, made ~1.2 MB apart in a scratch heap)
    const uint64_t size = 768ull * 432 * 8, step = 0x128200;
    const uint64_t a = 0x10005db1600, b = a + step;

    CHECK("a newer target from a shifted start kills it", shifted_over(a, a + size, 100, b, b + size, 101));
    CHECK("and one that starts before it", shifted_over(b, b + size, 100, a, a + size, 101));
    CHECK("not when it was written before the last use", !shifted_over(a, a + size, 100, b, b + size, 100));
    CHECK("not when it was written before the last use (older)", !shifted_over(a, a + size, 100, b, b + size, 40));
    CHECK("the same start is drawn_over's case", !shifted_over(a, a + size, 100, a, a + size / 2, 101));
    CHECK("a view inside it is not a new target", !shifted_over(a, a + size, 100, a + 4096, a + size - 4096, 101));
    CHECK("one covering it whole is", shifted_over(a + 4096, a + size, 100, a, a + 2 * size, 101));
    CHECK("an overlap of an eighth is enough", shifted_over(a, a + 8 * 4096, 100, a + 7 * 4096, a + 20 * 4096, 101));
    CHECK("less than an eighth is not", !shifted_over(a, a + 8 * 4096, 100, a + 7 * 4096 + 1, a + 20 * 4096, 101));
    CHECK("apart is not", !shifted_over(a, a + size, 100, a + size, a + 2 * size, 101));
    CHECK("empty ranges are not", !shifted_over(a, a, 100, a - 1, a + size, 101) && !shifted_over(a, a + size, 100, b, b, 101));
    std::puts("target overlap: all checks passed");
}
