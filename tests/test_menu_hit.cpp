// The Insert menu's hit-test (gpu/vk/vk_menu_hit.h): where a mouse position falls on the menu, at a scale of 1 (a
// 720-line window) and 2 (1440 lines), and through a window whose pixels are not the swapchain's.
#include "../gpu/vk/vk_menu_hit.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

using namespace mkwvk::menu_hit;

int main() {
    // the Graphics tab: 11 rows, the Sharpness row (7) a slider; values from x 380 at scale 1
    int value_l[11];
    bool slider[11] = {};
    for (int& v : value_l) v = 380;
    slider[7] = true;
    const int w1 = kWidth, h1 = 520;
    auto at = [&](int x, int y, int s) {
        int vl[11];
        for (int i = 0; i < 11; i++) vl[i] = value_l[i] * s;
        return options(x, y, s, kWidth * s, h1 * s, 3, 11, vl, slider);
    };
    Hit h;

    // ---- scale 1: the tabs (side 85, slots of 103 from 85) ----
    h = at(100, 60, 1);
    CHECK("tab 0", h.part == kTab && h.index == 0);
    h = at(240, 60, 1);
    CHECK("tab 1", h.part == kTab && h.index == 1);
    h = at(300, 60, 1);
    CHECK("tab 2", h.part == kTab && h.index == 2);
    CHECK("[Q / LB] is the previous tab", at(40, 60, 1).part == kTabPrev);
    CHECK("[E / RB] is the next tab", at(450, 60, 1).part == kTabNext);
    CHECK("the title is no control", at(100, 20, 1).part == kNone);

    // ---- scale 1: the rows (from 84, 28 high) ----
    h = at(30, 84 + 3 * 28 + 10, 1);
    CHECK("row 3 by its name", h.part == kRow && h.index == 3 && h.side == 0);
    h = at(390, 84 + 3 * 28 + 10, 1);
    CHECK("row 3's value, left half", h.part == kRow && h.index == 3 && h.side == -1);
    h = at(450, 84 + 3 * 28 + 10, 1);
    CHECK("row 3's value, right half", h.part == kRow && h.index == 3 && h.side == 1);
    h = at(30, 84, 1);
    CHECK("row 0's top line", h.part == kRow && h.index == 0);
    h = at(30, 84 - 1, 1);
    CHECK("above the rows", h.part == kNone);
    CHECK("the row band's left margin", at(7, 84 + 10, 1).part == kNone);
    CHECK("past the last row", at(30, 84 + 11 * 28 + 2, 1).part == kNone);
    CHECK("a tab with fewer rows", options(30, 84 + 5 * 28 + 2, 1, w1, h1, 3, 3, value_l, nullptr).part == kNone);

    // ---- scale 1: the slider's track, 200 to 330 ----
    const int sy = 84 + 7 * 28 + 14;
    h = at(265, sy, 1);
    CHECK("the track's middle", h.part == kSlider && h.index == 7 && h.permille == 500);
    h = at(196, sy, 1);
    CHECK("just before the track: its start", h.part == kSlider && h.permille == 0);
    h = at(334, sy, 1);
    CHECK("just past the track: its end", h.part == kSlider && h.permille == 1000);
    h = at(340, sy, 1);
    CHECK("past the grab: the row's name", h.part == kRow && h.side == 0);
    h = at(265, 84 + 6 * 28 + 14, 1);
    CHECK("no track on a row without one", h.part == kRow && h.index == 6);
    CHECK("a drag past the track holds to its ends", slider_drag(100, 1) == 0 && slider_drag(500, 1) == 1000);
    CHECK("a drag along it", slider_drag(265, 1) == 500);

    // ---- the slider's values ----
    CHECK("sharpness at half", slider_value(500, 0, 100, 10) == 50);
    CHECK("sharpness ends", slider_value(0, 0, 100, 10) == 0 && slider_value(1000, 0, 100, 10) == 100);
    CHECK("sensitivity at half (nearest step)", slider_value(500, 10, 500, 10) == 260);
    CHECK("sensitivity ends", slider_value(0, 10, 500, 10) == 10 && slider_value(1000, 10, 500, 10) == 500);
    CHECK("knob place", slider_permille(50, 0, 100) == 500 && slider_permille(10, 10, 500) == 0 &&
                        slider_permille(500, 10, 500) == 1000);
    CHECK("a value and its knob agree", slider_value(slider_permille(260, 10, 500), 10, 500, 10) == 260);

    // ---- scale 2 (a 1440-line window): everything twice as far ----
    h = at(480, 120, 2);
    CHECK("scale 2: tab 1", h.part == kTab && h.index == 1);
    CHECK("scale 2: [Q / LB]", at(100, 120, 2).part == kTabPrev);
    h = at(60, 168 + 2 * 56 + 5, 2);
    CHECK("scale 2: row 2", h.part == kRow && h.index == 2 && h.side == 0);
    h = at(900, 168 + 2 * 56 + 5, 2);
    CHECK("scale 2: row 2's value, right half", h.part == kRow && h.side == 1);
    h = at(530, 168 + 7 * 56 + 28, 2);
    CHECK("scale 2: the track's middle", h.part == kSlider && h.permille == 500);

    // ---- off the menu ----
    CHECK("left of the menu", at(-1, 100, 1).part == kOutside);
    CHECK("right of the menu", at(w1, 100, 1).part == kOutside);
    CHECK("below the menu", at(100, h1, 1).part == kOutside);

    // ---- the window's pixels to the menu's: a 1280x720 window, the menu centred ----
    {
        int ix, iy, x0, y0;
        to_image(400 + 300, 100 + 60, 1280, 720, 1280, 720, &ix, &iy);
        origin(1280, 720, w1, h1, &x0, &y0);
        CHECK("centred at 1280x720", x0 == 400 && y0 == 100);
        h = options(ix - x0, iy - y0, 1, w1, h1, 3, 11, value_l, slider);
        CHECK("1280x720: tab 2", h.part == kTab && h.index == 2);
    }
    {   // (the swapchain at 2560x1440 while the window still says 1280x720 pixels: a resize on its way)
        int ix, iy, x0, y0;
        to_image(640, 160, 1280, 720, 2560, 1440, &ix, &iy);
        CHECK("window to image", ix == 1280 && iy == 320);
        origin(2560, 1440, kWidth * 2, h1 * 2, &x0, &y0);
        h = at(ix - x0, iy - y0, 2);
        CHECK("2560x1440: tab 1", h.part == kTab && h.index == 1);
    }
    {   // (a 1920x1080 window: still scale 1, centred)
        int x0, y0;
        origin(1920, 1080, w1, h1, &x0, &y0);
        h = options(960 - x0, (y0 + 84 + 28 + 5) - y0, 1, w1, h1, 3, 11, value_l, slider);
        CHECK("1920x1080: row 1", h.part == kRow && h.index == 1);
    }

    // ---- the Controls page: 24 controls, then Reset; rows 24 high from 64 ----
    const int cw = kCtlWidth, ch = 720;
    h = controls(50, 64 + 2 * 24 + 5, 1, cw, ch, 24);
    CHECK("controls: row 2's name", h.part == kCtlRow && h.index == 2);
    h = controls(250, 64 + 2 * 24 + 5, 1, cw, ch, 24);
    CHECK("controls: row 2's gamepad", h.part == kCtlCell && h.index == 2 && h.side == 0);
    h = controls(400, 64 + 2 * 24 + 5, 1, cw, ch, 24);
    CHECK("controls: row 2's keys", h.part == kCtlCell && h.index == 2 && h.side == 1);
    h = controls(193, 64 + 5, 1, cw, ch, 24);
    CHECK("controls: just before the gamepad's cell", h.part == kCtlRow && h.index == 0);
    h = controls(250, 64 + 24 * 24 + 5, 1, cw, ch, 24);
    CHECK("controls: Reset to defaults is a row", h.part == kCtlRow && h.index == 24);
    CHECK("controls: past Reset", controls(250, 64 + 25 * 24 + 5, 1, cw, ch, 24).part == kNone);
    CHECK("controls: the header", controls(250, 50, 1, cw, ch, 24).part == kNone);
    h = controls(800, 128 + 2 * 48 + 10, 2, cw * 2, ch * 2, 24);
    CHECK("controls, scale 2: row 2's keys", h.part == kCtlCell && h.index == 2 && h.side == 1);

    // ---- the Cheats page: 11 rows 28 high from 44; row 4's value from x 420 (none on row 2: the menu's width) ----
    {
        int vl[11];
        for (int& v : vl) v = 420;
        vl[2] = kWidth;
        const int cheat_h = 136 + 11 * 28;
        h = cheats(30, 44 + 4 * 28 + 10, 1, kWidth, cheat_h, 11, vl);
        CHECK("cheats: row 4's name", h.part == kCheatRow && h.index == 4 && h.side == 0);
        h = cheats(425, 44 + 4 * 28 + 10, 1, kWidth, cheat_h, 11, vl);
        CHECK("cheats: row 4's value, left half", h.part == kCheatRow && h.index == 4 && h.side == -1);
        h = cheats(460, 44 + 4 * 28 + 10, 1, kWidth, cheat_h, 11, vl);
        CHECK("cheats: row 4's value, right half", h.part == kCheatRow && h.index == 4 && h.side == 1);
        h = cheats(460, 44 + 2 * 28 + 10, 1, kWidth, cheat_h, 11, vl);
        CHECK("cheats: a row without a value", h.part == kCheatRow && h.index == 2 && h.side == 0);
        CHECK("cheats: the title", cheats(30, 43, 1, kWidth, cheat_h, 11, vl).part == kNone);
        CHECK("cheats: the help below the rows", cheats(30, 44 + 11 * 28 + 2, 1, kWidth, cheat_h, 11, vl).part == kNone);
        CHECK("cheats: the band's margin", cheats(7, 44 + 10, 1, kWidth, cheat_h, 11, vl).part == kNone);
        CHECK("cheats: off the menu", cheats(30, cheat_h, 1, kWidth, cheat_h, 11, vl).part == kOutside);
        int vl2[11];
        for (int i = 0; i < 11; i++) vl2[i] = vl[i] * 2;
        h = cheats(60, (44 + 10 * 28 + 3) * 2, 2, kWidth * 2, cheat_h * 2, 11, vl2);
        CHECK("cheats, scale 2: the last row", h.part == kCheatRow && h.index == 10 && h.side == 0);
        h = cheats(930, (44 + 4 * 28 + 3) * 2, 2, kWidth * 2, cheat_h * 2, 11, vl2);
        CHECK("cheats, scale 2: row 4's value, right half", h.part == kCheatRow && h.index == 4 && h.side == 1);
    }

    // ---- a list page: 20 lines 24 high from 48 (the line as shown; the menu maps it to its item) ----
    {
        int vl[20];
        for (int& v : vl) v = kWidth;
        vl[3] = 440;
        const int list_h = 100 + 20 * 24;
        h = list_lines(40, 48 + 3 * 24 + 5, 1, kWidth, list_h, 20, vl);
        CHECK("list: line 3's name", h.part == kListLine && h.index == 3 && h.side == 0);
        h = list_lines(445, 48 + 3 * 24 + 5, 1, kWidth, list_h, 20, vl);
        CHECK("list: line 3's amount, left half", h.part == kListLine && h.index == 3 && h.side == -1);
        h = list_lines(460, 48 + 3 * 24 + 5, 1, kWidth, list_h, 20, vl);
        CHECK("list: line 3's amount, right half", h.part == kListLine && h.index == 3 && h.side == 1);
        h = list_lines(460, 48 + 5 * 24 + 5, 1, kWidth, list_h, 20, vl);
        CHECK("list: a line without an amount", h.part == kListLine && h.index == 5 && h.side == 0);
        CHECK("list: the heading line above", list_lines(40, 47, 1, kWidth, list_h, 20, vl).part == kNone);
        CHECK("list: a short list's end", list_lines(40, 48 + 7 * 24 + 5, 1, kWidth, list_h, 7, vl).part == kNone);
        CHECK("list: the hints", list_lines(40, 48 + 20 * 24 + 5, 1, kWidth, list_h, 20, vl).part == kNone);
        h = list_lines(80, (48 + 19 * 24 + 2) * 2, 2, kWidth * 2, list_h * 2, 20, nullptr);
        CHECK("list, scale 2: the last line", h.part == kListLine && h.index == 19 && h.side == 0);
    }

    // ---- the restart question: Later from 16, Restart now from 182, both 150 wide, 120 to 154 high ----
    {
        const int ah = kAskHeight;
        h = ask(20, 130, 1, kWidth, ah);
        CHECK("ask: Later", h.part == kButton && h.index == 0);
        h = ask(331, 153, 1, kWidth, ah);
        CHECK("ask: Restart now", h.part == kButton && h.index == 1);
        CHECK("ask: between the buttons", ask(170, 130, 1, kWidth, ah).part == kNone);
        CHECK("ask: past Restart now", ask(340, 130, 1, kWidth, ah).part == kNone);
        CHECK("ask: above the buttons", ask(20, 119, 1, kWidth, ah).part == kNone);
        CHECK("ask: below the buttons", ask(20, 154, 1, kWidth, ah).part == kNone);
        CHECK("ask: off the page", ask(20, ah, 1, kWidth, ah).part == kOutside);
        h = ask(190 * 2, 125 * 2, 2, kWidth * 2, ah * 2);
        CHECK("ask, scale 2: Restart now", h.part == kButton && h.index == 1);
        h = ask(17 * 2, 153 * 2 + 1, 2, kWidth * 2, ah * 2);
        CHECK("ask, scale 2: Later", h.part == kButton && h.index == 0);
    }

    std::puts("menu hit-test: all checks passed");
}
