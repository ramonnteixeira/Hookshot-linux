"""The mouse's sensitivity and inversion are the Insert menu's (options 7 and 8), and - and = change the menu's value.

Reads the sources: the menu's rows and their options, the renderer's option_set back to the menu, and the host reading
both options where the mouse is used (host/nn_hid.c's stick, host/mouse_native.c).
python -I -B tests/test_mouse_settings.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def preset_exempt(menu, value):
    """the menu's row for opts.<value> leaves the quality preset alone (Row::preset_exempt, which set_row checks)"""
    start = menu.index("&opts.%s," % value, menu.index("Row rows[] = {"))
    row = menu[start:menu.index("},", start)]
    return row.rstrip().endswith(", true") and "if (!r.preset_exempt)" in menu


def read(*parts):
    return (ROOT.joinpath(*parts)).read_text(encoding="utf-8")


def main():
    menu, replay, api = read("gpu", "vk", "vk_menu.cpp"), read("gpu", "vk", "vk_replay.cpp"), read("gpu", "vk", "vk_api.h")
    hid, native, backend = read("host", "nn_hid.c"), read("host", "mouse_native.c"), read("host", "rx_backend.c")
    for row in ('"Mouse sensitivity", &opts.mouse_sens, 10, 500,', '"Invert mouse vertical", &opts.mouse_invert, 0, 1,'):
        if row not in menu:
            raise SystemExit("the menu lost a mouse row: " + row)
    if "id == 7 ? opts.mouse_sens" not in replay or "id == 8 ? opts.mouse_invert" not in replay:
        raise SystemExit("options 7 and 8 do not reach the host")
    if "t.option_set = menu_option_set" not in replay or "void (*option_set)(int id, int value);" not in api:
        raise SystemExit("the host cannot change the menu's sensitivity")
    if "vk->option_set(id, value)" not in backend:
        raise SystemExit("rx_option_set does not reach the renderer")
    keys = hid[hid.index("static void mouse_sens_keys("):]
    keys = keys[:keys.index("\n}\n")]
    if "rx_option(7, 100)" not in keys or "rx_option_set(7," not in keys:
        raise SystemExit("- and = do not change the menu's sensitivity")
    if "HOS_MOUSE_SENS" not in keys:
        raise SystemExit("HOS_MOUSE_SENS no longer wins over the menu")
    if "rx_option(8, 0)" not in hid or "rx_option(8, 0)" not in native:
        raise SystemExit("the menu's inversion is not read by both mouse paths")
    if not preset_exempt(menu, "mouse_sens"):
        raise SystemExit("a mouse setting turns the quality preset to Custom")
    print("PASS: the mouse's settings are the menu's")


if __name__ == "__main__":
    main()
