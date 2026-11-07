// (linux port) The part of GDI the Insert menu draws with (vk_menu.cpp): a memory DC over a 32-bit top-down DIB,
// solid rectangles and text. The text is third_party/font8x8 (public domain, 8x8, ASCII) scaled to the font's height,
// bolded by a second pass one pixel over: plainer than Segoe UI, but the menu and the counter read the same.
// Only on Linux; Windows uses GDI itself.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gdi_lite {
#include "../../third_party/font8x8/font8x8_basic.h"
}

typedef uint32_t COLORREF;
#define RGB(r, g, b) ((COLORREF)((uint8_t)(r) | ((uint32_t)(uint8_t)(g) << 8) | ((uint32_t)(uint8_t)(b) << 16)))
struct GdiObj {
    enum Kind { BITMAP = 1, FONT, BRUSH } kind;
    uint32_t w = 0, h = 0;
    uint8_t* bits = nullptr;   // (BGRX rows, top down)
    int px = 0, weight = 0;
    COLORREF colour = 0;
};
struct GdiDc {
    GdiObj* bitmap = nullptr;
    GdiObj* font = nullptr;
    COLORREF text = 0;
    unsigned align = 0;
};
typedef GdiDc* HDC;
typedef GdiObj *HGDIOBJ, *HBITMAP, *HFONT, *HBRUSH;
struct BITMAPINFOHEADER { uint32_t biSize; int32_t biWidth, biHeight; uint16_t biPlanes, biBitCount; uint32_t biCompression; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; };
struct SIZE { LONG cx, cy; };
enum { BI_RGB = 0, DIB_RGB_COLORS = 0, TRANSPARENT = 1, TA_LEFT = 0, TA_TOP = 0, TA_RIGHT = 2, FW_NORMAL = 400,
       FW_SEMIBOLD = 600, FW_BOLD = 700, DEFAULT_CHARSET = 1, OUT_DEFAULT_PRECIS = 0, CLIP_DEFAULT_PRECIS = 0,
       CLEARTYPE_QUALITY = 5, NONANTIALIASED_QUALITY = 3, DEFAULT_PITCH = 0 };

inline HDC CreateCompatibleDC(HDC) { return new GdiDc(); }
inline BOOL DeleteDC(HDC dc) { delete dc; return TRUE; }
inline HBITMAP CreateDIBSection(HDC, const BITMAPINFO* bi, unsigned, void** bits, HANDLE, DWORD) {
    const uint32_t w = (uint32_t)bi->bmiHeader.biWidth, h = (uint32_t)std::abs(bi->bmiHeader.biHeight);
    auto* o = new GdiObj{GdiObj::BITMAP};
    o->w = w, o->h = h, o->bits = (uint8_t*)calloc((size_t)w * h, 4);
    if (!o->bits) { delete o; return nullptr; }
    *bits = o->bits;
    return o;
}
inline HFONT CreateFontA(int height, int, int, int, int weight, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, const char*) {
    auto* o = new GdiObj{GdiObj::FONT};
    o->px = height < 0 ? -height : height, o->weight = weight;
    return o;
}
inline HBRUSH CreateSolidBrush(COLORREF c) { auto* o = new GdiObj{GdiObj::BRUSH}; o->colour = c; return o; }
inline BOOL DeleteObject(HGDIOBJ o) {
    if (!o) return FALSE;
    free(o->bits);
    delete o;
    return TRUE;
}
inline HGDIOBJ SelectObject(HDC dc, HGDIOBJ o) {
    GdiObj*& slot = o->kind == GdiObj::BITMAP ? dc->bitmap : dc->font;
    GdiObj* was = slot;
    slot = o;
    return was;
}
inline BOOL GdiFlush() { return TRUE; }
inline COLORREF SetTextColor(HDC dc, COLORREF c) { COLORREF was = dc->text; dc->text = c; return was; }
inline int SetBkMode(HDC, int) { return TRANSPARENT; }
inline unsigned SetTextAlign(HDC dc, unsigned a) { unsigned was = dc->align; dc->align = a; return was; }

inline void gdi_put(HDC dc, int x, int y, COLORREF c) {
    GdiObj* b = dc->bitmap;
    if (!b || x < 0 || y < 0 || (uint32_t)x >= b->w || (uint32_t)y >= b->h) return;
    uint8_t* p = b->bits + ((size_t)y * b->w + (uint32_t)x) * 4;
    p[0] = (uint8_t)(c >> 16), p[1] = (uint8_t)(c >> 8), p[2] = (uint8_t)c, p[3] = 0;
}
inline int FillRect(HDC dc, const RECT* r, HBRUSH br) {
    for (int y = r->top; y < r->bottom; y++)
        for (int x = r->left; x < r->right; x++) gdi_put(dc, x, y, br->colour);
    return 1;
}
/// the font's pixel size per font8x8 pixel: its height in eighths, at least 1
inline int gdi_scale(HDC dc) { const int px = dc->font ? dc->font->px : 16; return px >= 12 ? (px + 4) / 8 : 1; }
inline BOOL GetTextExtentPoint32A(HDC dc, const char*, int n, SIZE* sz) {
    const int k = gdi_scale(dc);
    sz->cx = n * 8 * k, sz->cy = 8 * k;
    return TRUE;
}
inline BOOL TextOutA(HDC dc, int x, int y, const char* s, int n) {
    const int k = gdi_scale(dc), px = dc->font ? dc->font->px : 8 * k;
    const bool bold = dc->font && dc->font->weight >= FW_SEMIBOLD;
    if (dc->align & TA_RIGHT) x -= n * 8 * k;
    y += (px - 8 * k) / 2 + k / 2;   // (the cell's height, as GDI's: the glyph in its middle)
    for (int i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)s[i];
        const char* g = gdi_lite::font8x8_basic[ch < 128 ? ch : '?'];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if ((unsigned char)g[row] >> col & 1)
                    for (int dy = 0; dy < k; dy++)
                        for (int dx = 0; dx < k + (bold ? (k + 1) / 2 : 0); dx++)
                            gdi_put(dc, x + i * 8 * k + col * k + dx, y + row * k + dy, dc->text);
    }
    return TRUE;
}
