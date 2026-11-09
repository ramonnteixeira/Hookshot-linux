#include "../gpu/vk/gpu_choice.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using mkwvk::gpu_choice::menu_device;
    CHECK("Auto stays Auto", menu_device(0, 2) == -1);
    CHECK("the n-th usable GPU", menu_device(1, 2) == 0 && menu_device(2, 2) == 1);
    CHECK("a GPU taken out since is Auto", menu_device(2, 1) == -1);
    CHECK("a negative choice (a hand-edited ini) is Auto", menu_device(-1, 2) == -1);
    CHECK("no usable GPU: Auto", menu_device(1, 0) == -1);
    std::puts("gpu choice: all checks passed");
}
