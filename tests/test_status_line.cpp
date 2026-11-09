// tests/test_status_line.py: gpu/vk/status_line.h, the renderer's status line and its video-memory warning
#include "../gpu/vk/status_line.h"

#include <cstdio>
#include <cstring>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace sl = mkwvk::status_line;
    constexpr uint64_t GB = 1ull << 30;
    char b[256];
    sl::vram_text(b, sizeof b, 7 * GB + GB / 10, 8 * GB, true);
    CHECK("used of allowed", !std::strcmp(b, "VRAM 7.1 of 8.0 GB"));
    sl::vram_text(b, sizeof b, 3 * GB, 0, false);
    CHECK("no budget", !std::strcmp(b, "VRAM 3.0 GB in the game's allocations (no budget from the driver)"));
    sl::vram_text(b, sizeof b, 3 * GB, 6 * GB, false);
    CHECK("no budget, VMA's guess not shown", std::strstr(b, "no budget") && !std::strstr(b, "of 6.0"));

    // the warning: once when the use passes the allowance, again only after a reading back under it
    bool over = false;
    CHECK("under", !sl::crossed(over, 7 * GB, 8 * GB) && !over);
    CHECK("at the allowance is not over", !sl::crossed(over, 8 * GB, 8 * GB));
    CHECK("over: told", sl::crossed(over, 8 * GB + 1, 8 * GB) && over);
    CHECK("still over: not again", !sl::crossed(over, 9 * GB, 8 * GB) && over);
    CHECK("back under", !sl::crossed(over, 7 * GB, 8 * GB) && !over);
    CHECK("over again: told again", sl::crossed(over, 9 * GB, 8 * GB));
    bool unknown = false;
    CHECK("nothing known is never over", !sl::crossed(unknown, 9 * GB, 0) && !unknown);

    sl::over_text(b, sizeof b, 8 * GB + 3 * GB / 10, 8 * GB, 2);
    CHECK("DLAA's warning", !std::strcmp(b, "vk: over the driver's video-memory allowance (8.3 of 8.0 GB): DLAA may stutter here; "
                                            "Quality or Balanced uses less"));
    sl::over_text(b, sizeof b, 8 * GB + 3 * GB / 10, 8 * GB, 1);
    CHECK("DLSS's warning", std::strstr(b, ": DLSS may stutter here; a lower Render resolution mode uses less"));
    sl::over_text(b, sizeof b, 8 * GB + 3 * GB / 10, 8 * GB, 0);
    CHECK("the game's own: no advice", !std::strcmp(b, "vk: over the driver's video-memory allowance (8.3 of 8.0 GB)"));
    std::puts("status line: all checks passed");
}
