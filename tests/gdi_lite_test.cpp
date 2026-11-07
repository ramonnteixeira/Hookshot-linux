// (linux port) gpu/vk/gdi_lite.h run through the Insert menu's own sequence: a DIB and fonts selected, the DC's first
// objects selected back before the fonts are deleted (a null "old" font crashed the FPS counter), text drawn after.
//   clang -c -O1 -D_GNU_SOURCE -Ihost host/plat_posix.c -o /tmp/plat.o
//   clang++ -std=c++20 -O1 -D_GNU_SOURCE -Ihost -Igpu/vk tests/gdi_lite_test.cpp /tmp/plat.o -o /tmp/gdit -lpthread -ldl
#include "win32_compat.h"
#include "gdi_lite.h"
#include <cstdio>
int main() {
    // the menu's sequence: a DIB selected, fonts selected and the old ones back, then everything deleted
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{}; bi.bmiHeader.biWidth = 200; bi.bmiHeader.biHeight = -40; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldb = SelectObject(dc, bmp);
    HFONT label = CreateFontA(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0, 0, 0, "x"), big = CreateFontA(-24, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, 0, 0, "x");
    HGDIOBJ oldf = SelectObject(dc, label);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, 2, 2, "FPS", 3);
    SelectObject(dc, big);
    SetTextAlign(dc, TA_RIGHT);
    TextOutA(dc, 198, 2, "60", 2);
    SelectObject(dc, oldf);
    DeleteObject(label); DeleteObject(big);
    TextOutA(dc, 0, 20, "ok", 2);   // (with the stock font again)
    int lit = 0;
    for (int i = 0; i < 200 * 40; i++) lit += ((uint8_t*)bits)[i * 4 + 2] != 0;
    SelectObject(dc, oldb); DeleteObject(bmp); DeleteDC(dc);
    printf(lit > 50 ? "all passed (%d pixels)\n" : "FAIL: %d pixels\n", lit);
    return lit > 50 ? 0 : 1;
}
