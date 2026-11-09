"""The Insert menu's mouse: the hit-test (gpu/vk/vk_menu_hit.h) compiled and run against positions at two window sizes
(tests/test_menu_hit.cpp: the options, the Controls page, the Cheats page and its lists), and the sources checked for the rest of the path: the window lets the mouse go while the
menu is open and takes it back after (WM_APP + 8, host/display.c and host/posix/display_sdl.c), the free mouse reaches
the menu (MkwVkHost.pointer, host/rx_backend.c), and the menu draws with the sizes it hit-tests with.

    python -I -B tests/test_menu_hit.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


def sources():
    menu, api, backend = read("gpu", "vk", "vk_menu.cpp"), read("gpu", "vk", "vk_api.h"), read("host", "rx_backend.c")
    win, sdl = read("host", "display.c"), read("host", "posix", "display_sdl.c")
    if '#include "vk_menu_hit.h"' not in menu or any("menu_hit::%s(" % f not in menu
                                                      for f in ("options", "controls", "cheats", "list_lines")):
        raise SystemExit("the menu does not hit-test with vk_menu_hit.h")
    for size in ("menu_hit::kTabY", "menu_hit::kRowH", "menu_hit::rows_top(s)", "menu_hit::tab_slot_w(", "menu_hit::kCtlRowH",
                 "menu_hit::kCheatTop", "menu_hit::kCheatRowH", "menu_hit::kListTop", "menu_hit::kListRowH"):
        if size not in menu:
            raise SystemExit("the menu draws without %s: its picture and its hit-test can drift apart" % size)
    if "PostMessageA(app_window(), WM_APP + 8" not in menu:
        raise SystemExit("the menu does not tell the window it opened or closed")
    if "int (*pointer)(MkwVkPointer *out);" not in api or "hos_mouse_pointer," not in backend:
        raise SystemExit("the free mouse does not reach the renderer")
    for name, src in (("display.c", win), ("display_sdl.c", sdl)):
        if "int hos_mouse_pointer(MkwVkPointer *out)" not in src:
            raise SystemExit(name + ": no hos_mouse_pointer")
        if "WM_APP + 8" not in src or "captured_before_menu" not in src:
            raise SystemExit(name + ": the menu does not release the mouse and give the capture back")
    # (a click with the menu open is the menu's: it never captures the mouse for the game)
    down = sdl[sdl.index("case HOS_SDL_EV_MOUSE_DOWN:"):]
    down = down[:down.index("break;")]
    if "if (menu_open)" not in down or "else if (renderer && !mouse_captured) mouse_capture(1);" not in down:
        raise SystemExit("display_sdl.c: a click with the menu open captures the mouse")
    down = win[win.index("case WM_LBUTTONDOWN:"):]
    down = down[:down.index("return 0;")]
    if "if (menu_open)" not in down or "else if (renderer && !mouse_captured) mouse_capture(w, 1);" not in down:
        raise SystemExit("display.c: a click with the menu open captures the mouse")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    sources()
    with tempfile.TemporaryDirectory(prefix="menu-hit-") as tmp:
        exe = Path(tmp) / "menu-hit"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_menu_hit.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the menu's mouse")


if __name__ == "__main__":
    main()
