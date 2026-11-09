#include "../gpu/vk/skip_code.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using mkwvk::skip_code::matches;
    // (a Kokiri Forest program on the Steam Deck: vertex/task/mesh stage keys as program_code_keys writes them, issue 817)
    const char* codes = " 1:9bcda2a743ed9704 6:2c56dc1006942f74 7:4797e4cd2b870a69";

    CHECK("a whole key", matches(codes, "2c56dc1006942f74"));
    CHECK("a prefix", matches(codes, "2c56dc"));
    CHECK("upper case", matches(codes, "2C56DC1006942F74"));
    CHECK("any stage", matches(codes, "9bcd") && matches(codes, "4797e4cd2b870a69"));
    CHECK("one of a list", matches(codes, "deadbeef,2c56dc1006942f74") && matches(codes, "2c56,deadbeef"));
    CHECK("empty entries are skipped", matches(codes, ",,2c56dc1006942f74,") && !matches(codes, ",,"));
    CHECK("no match", !matches(codes, "deadbeef") && !matches(codes, "56dc1006942f74"));
    CHECK("a prefix only matches at the start of a key", !matches(codes, "1006942f74"));
    CHECK("the stage number is not part of the key", !matches(codes, "6:2c56") && !matches(codes, "1:"));
    CHECK("longer than the key", !matches(codes, "2c56dc1006942f7400"));
    CHECK("an empty list leaves every program in", !matches(codes, ""));
    CHECK("a program without stage keys", !matches("", "2c56dc1006942f74") && !matches(" ", "2c56"));
    std::puts("skip_code: all checks passed");
    return 0;
}
