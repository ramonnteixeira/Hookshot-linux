#include "../gpu/vk/bc_levels.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using namespace mkwvk::bc_levels;

    CHECK("blocks round up", blocks(1) == 1 && blocks(4) == 1 && blocks(5) == 2 && blocks(1365) == 342);
    CHECK("powers of two are even", first_uneven(2048, 2048, 12) == 12 && first_uneven(512, 256, 10) == 10);
    CHECK("1365 goes uneven at level 2", first_uneven(1365, 1365, 11) == 2);
    CHECK("683 goes uneven at level 1", first_uneven(683, 683, 10) == 1);
    CHECK("768 only at its 6-texel level", first_uneven(768, 768, 10) == 7);
    CHECK("1536 only at its 6-texel level", first_uneven(1536, 1536, 11) == 8);
    CHECK("either side can be uneven", first_uneven(1024, 1365, 11) == 2);
    CHECK("1365 -> 1536", even_size(1365, 11) == 1536);
    CHECK("683 -> 768", even_size(683, 10) == 768);
    CHECK("768 and 1536 stay", even_size(768, 10) == 768 && even_size(1536, 11) == 1536);
    CHECK("powers of two stay", even_size(2048, 12) == 2048 && even_size(256, 9) == 256);
    CHECK("small sizes", even_size(171, 8) == 192 && even_size(12, 4) == 12);
    CHECK("an even size is even down to 16 texels", first_uneven(even_size(1365, 11), even_size(1365, 11), 11) >= 8);
    for (uint32_t w = 16; w <= 2048; w++) {
        uint32_t levels = 1;
        while ((w >> levels) > 0) levels++;
        const uint32_t e = even_size(w, levels);
        uint32_t big = 0;
        while (big < levels && (e >> big) >= 16) big++;
        CHECK("every size halves evenly down to 16 texels", e >= w && first_uneven(e, e, levels) >= big);
    }
    std::puts("bc levels: all checks passed");
}
