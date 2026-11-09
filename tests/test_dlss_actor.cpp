#include "../gpu/vk/dlss_actor.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace da = dlss_actor;
    // names: the same draw keeps its name, another draw of the same model is the next one, another model another name
    const uint64_t a0 = da::key(0x1000, 0x2000, 9702, 0, 0xabc, 0), a1 = da::key(0x1000, 0x2000, 9702, 0, 0xabc, 1);
    CHECK("the same draw, the same name", a0 == da::key(0x1000, 0x2000, 9702, 0, 0xabc, 0));
    CHECK("the second draw of a model is another", a0 != a1 && da::same_model(a0, a1));
    CHECK("another model", !da::same_model(a0, da::key(0x1000, 0x2000, 9702, 3, 0xabc, 0)) &&
                               !da::same_model(a0, da::key(0x1040, 0x2000, 9702, 0, 0xabc, 0)) &&
                               !da::same_model(a0, da::key(0x1000, 0x2000, 9702, 0, 0xabd, 0)));
    CHECK("many draws of one model", da::same_model(a0, da::key(0x1000, 0x2000, 9702, 0, 0xabc, 900)));

    // where a draw's indices start for the shader, which reads 32-bit words: the address 4-aligned down, the first index in it
    uint64_t at = 0;
    uint32_t first = 0;
    da::index_start(0x10002, 1, &at, &first);
    CHECK("16-bit at +2: the second half of the word", at == 0x10000 && first == 1);
    da::index_start(0x10003, 0, &at, &first);
    CHECK("8-bit at +3: the fourth byte", at == 0x10000 && first == 3);
    da::index_start(0x10008, 2, &at, &first);
    CHECK("32-bit, aligned", at == 0x10008 && first == 0);
    da::index_start(0x10004, 1, &at, &first);
    CHECK("16-bit, aligned", at == 0x10004 && first == 0);
    std::puts("dlss_actor: ok");
    return 0;
}
