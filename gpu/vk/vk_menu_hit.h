// The Insert menu's layout (vk_menu.cpp draws with these sizes, times its scale) and what a point over it is on: the
// mouse's hover and clicks while the menu is open. Plain arithmetic, no Vulkan or GDI: tests/test_menu_hit.cpp checks it.
#pragma once

namespace mkwvk::menu_hit {

// ---- the options page, at a scale of 1 (the window 720 pixels high; 1440 and up: 2, and so on) ----
constexpr int kWidth = 480;
constexpr int kTabY = 44, kTabH = 32;     // the tab strip
constexpr int kTabSide = 85;              // either side of the tab titles: [Q / LB] and [E / RB], the previous and next tab
constexpr int kRowGap = 8, kRowH = 28;    // the rows, from kTabY + kTabH + kRowGap
constexpr int kRowX = 8;                  // a row's band, kRowX in from either side
constexpr int kValueRight = 18;           // a value ends kValueRight from the right
constexpr int kSliderL = 200, kSliderR = 330;   // a slider row's track (Sharpness, Mouse sensitivity)
constexpr int kSliderGrab = 6;            // (a click this far past either end still takes the track's end)
// ---- the Controls page ----
constexpr int kCtlWidth = 600, kCtlTop = 64, kCtlRowH = 24, kCtlPad = 200, kCtlKey = 390, kCtlCellPad = 6;
// ---- the Cheats page (no tab strip), and its lists: the teleport list and the inventory ----
constexpr int kCheatTop = 44, kCheatRowH = 28;   // the cheats' rows, from kCheatTop
constexpr int kListTop = 48, kListRowH = 24;     // a list's lines (a section's heading is one too), from kListTop
constexpr int kListValueRight = 16;              // a cheat's or a list line's value ends kListValueRight from the right

enum Part {
    kNone,      // off the menu's controls (still over the menu: a click does nothing)
    kOutside,   // off the menu
    kTabPrev,   // the tab strip's left end
    kTab,       // a tab: index
    kTabNext,   // its right end
    kRow,       // an option's row: index (the tab's own), side -1 the value's left half, 1 its right half, 0 the name
    kSlider,    // a slider row's track: index, permille 0 to 1000 along it
    kCtlRow,    // the Controls page: a row's name (index; index == the count: Reset to defaults)
    kCtlCell,   // a binding: index the row, side 0 the gamepad's, 1 the keyboard's
    kCheatRow,  // the Cheats page: a row (index), side -1 the value's left half, 1 its right half, 0 the name
    kListLine,  // a list (the teleport list, the inventory): a line as shown (index, 0 the top one), side as kCheatRow
};
struct Hit {
    Part part = kNone;
    int index = -1;
    int side = 0;
    int permille = 0;
};

/// A point in the window's pixels (the cursor's, in a window win_w x win_h pixels) in the window's image (img_w x img_h:
/// the swapchain's; the same size as a rule, but not while a resize is on its way)
inline void to_image(int x, int y, int win_w, int win_h, int img_w, int img_h, int* ix, int* iy) {
    *ix = win_w > 0 ? (int)((long long)x * img_w / win_w) : x;
    *iy = win_h > 0 ? (int)((long long)y * img_h / win_h) : y;
}
/// The menu's top left in the window's image: centred, as menu_record copies it
inline void origin(int img_w, int img_h, int menu_w, int menu_h, int* x0, int* y0) {
    *x0 = (img_w - menu_w) / 2;
    *y0 = (img_h - menu_h) / 2;
}
inline int rows_top(int s) { return (kTabY + kTabH + kRowGap) * s; }
inline int tab_slot_w(int s, int w) { return (w - 2 * kTabSide * s) / 3; }
/// A slider's value for a point along its track (lo to hi in steps of step), nearest step, held to its ends
inline int slider_value(int permille, int lo, int hi, int step) {
    if (step <= 0 || hi <= lo) return lo;
    const int steps = (hi - lo) / step;
    const int k = (permille * steps + 500) / 1000;
    return lo + (k < 0 ? 0 : k > steps ? steps : k) * step;
}
/// Where along the track a value is, in permille (the knob's place)
inline int slider_permille(int v, int lo, int hi) {
    if (hi <= lo) return 0;
    const int p = (int)((long long)(v - lo) * 1000 / (hi - lo));
    return p < 0 ? 0 : p > 1000 ? 1000 : p;
}

/// The options page: (x, y) in the menu's own pixels (0, 0 its top left), w x h its size, s its scale; rows the shown
/// tab's row count; value_l[i] the left edge of row i's value as last drawn; slider[i] whether row i has a track.
/// A drag (dragging) keeps to the track whatever the point's height: slider_drag
inline Hit options(int x, int y, int s, int w, int h, int tabs, int rows, const int* value_l, const bool* slider) {
    Hit hit;
    if (x < 0 || y < 0 || x >= w || y >= h) return hit.part = kOutside, hit;
    if (y >= kTabY * s && y < (kTabY + kTabH) * s) {
        const int side = kTabSide * s, slot = tab_slot_w(s, w);
        if (x < side) hit.part = kTabPrev;
        else if (x >= w - side) hit.part = kTabNext;
        else if (slot > 0) {
            hit.part = kTab;
            hit.index = (x - side) / slot;
            if (hit.index >= tabs) hit.index = tabs - 1;
        }
        return hit;
    }
    const int top = rows_top(s);
    if (y < top || x < kRowX * s || x >= w - kRowX * s) return hit;
    const int i = (y - top) / (kRowH * s);
    if (i >= rows) return hit;
    hit.index = i;
    if (slider && slider[i] && x >= (kSliderL - kSliderGrab) * s && x < (kSliderR + kSliderGrab) * s) {
        hit.part = kSlider;
        const int p = (int)((long long)(x - kSliderL * s) * 1000 / ((kSliderR - kSliderL) * s));
        hit.permille = p < 0 ? 0 : p > 1000 ? 1000 : p;
        return hit;
    }
    hit.part = kRow;
    const int vl = value_l ? value_l[i] : w;
    const int vr = w - kValueRight * s;
    if (x >= vl && vl < vr) hit.side = x < (vl + vr) / 2 ? -1 : 1;
    return hit;
}
/// A drag that started on a slider: the point's place along the track, held to its ends, wherever the cursor went
inline int slider_drag(int x, int s) {
    const int p = (int)((long long)(x - kSliderL * s) * 1000 / ((kSliderR - kSliderL) * s));
    return p < 0 ? 0 : p > 1000 ? 1000 : p;
}

/// The Controls page: (x, y) in its pixels, w x h its size, s its scale; count the controls (then Reset to defaults)
inline Hit controls(int x, int y, int s, int w, int h, int count) {
    Hit hit;
    if (x < 0 || y < 0 || x >= w || y >= h) return hit.part = kOutside, hit;
    const int top = kCtlTop * s;
    if (y < top || x < kRowX * s || x >= w - kRowX * s) return hit;
    const int i = (y - top) / (kCtlRowH * s);
    if (i > count) return hit;
    hit.index = i;
    if (i < count && x >= (kCtlPad - kCtlCellPad) * s) {
        hit.part = kCtlCell;
        hit.side = x >= (kCtlKey - kCtlCellPad) * s ? 1 : 0;
    } else {
        hit.part = kCtlRow;
    }
    return hit;
}

/// Rows top + i * row_h high (times s), in the band kRowX in from either side: part with the row's index and the side of
/// its value it is on (value_l[i] the left edge of row i's value as last drawn, w or more: none; null: no values)
inline Hit row_list(int x, int y, int s, int w, int h, int top, int row_h, int count, const int* value_l, Part part) {
    Hit hit;
    if (x < 0 || y < 0 || x >= w || y >= h) return hit.part = kOutside, hit;
    if (y < top * s || x < kRowX * s || x >= w - kRowX * s) return hit;
    const int i = (y - top * s) / (row_h * s);
    if (i >= count) return hit;
    hit.part = part;
    hit.index = i;
    const int vl = value_l ? value_l[i] : w;
    const int vr = w - kListValueRight * s;
    if (x >= vl && vl < vr) hit.side = x < (vl + vr) / 2 ? -1 : 1;
    return hit;
}
/// The Cheats page: count its rows
inline Hit cheats(int x, int y, int s, int w, int h, int count, const int* value_l) {
    return row_list(x, y, s, w, h, kCheatTop, kCheatRowH, count, value_l, kCheatRow);
}
/// A list page: count the lines shown (headings included); index is the line from the top as shown, not the item
inline Hit list_lines(int x, int y, int s, int w, int h, int count, const int* value_l) {
    return row_list(x, y, s, w, h, kListTop, kListRowH, count, value_l, kListLine);
}

} // namespace mkwvk::menu_hit
