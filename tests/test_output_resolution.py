"""Exercise the host and menu's docked output sizes without a game dump."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    host = (ROOT / "host/nn_applet.c").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

using DWORD = unsigned long;
constexpr int MAX_PATH = 260;
DWORD hos_game_path_a(char *path, int size) { return snprintf(path, size, "./hookshot.exe"); }
void hos_log(const char *, ...) {}
volatile uint64_t hos_wide_out = 0;   // (host/hos_main.c: the widescreen picture, 0 = none; nn_applet.c reads it)
struct { int handheld, output_res; } opts;
''' + host[host.index("static int ini_option("):host.index("HLE(_ZN2nn2oe16GetOperationModeEv)")] + menu[
        menu.index("struct Out {"):menu.index("const char* tx_speed(")] + r'''
int main(int argc, char **argv) {
    assert(argc == 5);
    const int mode = atoi(argv[1]);
    const unsigned w = atoi(argv[2]), h = atoi(argv[3]);
    assert(output_mode() == mode);
    assert(hos_output_size(0) == w && hos_output_size(1) == h);
    assert(hos_rt_pool_size(305ull << 20) == ((mode == 2 ? 687ull : 305ull) << 20));
    assert(kLastOutput == 3);
    assert(output_choice(-1) == 0 && output_choice(4) == 0);
    assert(step_output_choice(3, -1) == 3);
    assert(step_output_choice(3, 1) == 0 && step_output_choice(0, -1) == 3);
    assert(step_output_choice(0, 1) == 1 && step_output_choice(1, -1) == 0);
    assert(step_output_choice(1, 1) == 2 && step_output_choice(2, -1) == 1);
    assert(step_output_choice(2, 1) == 2);
    opts.output_res = mode;
    opts.handheld = handheld();
    assert(strcmp(tx_output_res(mode), argv[4]) == 0);
    unsigned scene_w, scene_h;
    game_size(1.0f, scene_w, scene_h);
    assert(scene_w == (opts.handheld ? 1536 : mode ? w : 2048));
    assert(scene_h == (opts.handheld ? 864 : mode ? h : 1152));
    if (mode == 3) assert(strcmp(tx_resolution(2), "75% (1440x810)") == 0);
}
'''
    cases = (
        (0, 0, None, 0, 2560, 1440, "2048x1152 (game, restart)"),
        (1, 0, None, 1, 2560, 1440, "2560x1440 (restart)"),
        (2, 0, None, 2, 3840, 2160, "3840x2160 (restart)"),
        (3, 0, None, 3, 1920, 1080, "1920x1080 (restart)"),
        (1, 0, "3", 3, 1920, 1080, "1920x1080 (restart)"),
        (3, 1, "3", 0, 2560, 1440, "2048x1152 (game, restart)"),
        (4, 0, None, 0, 2560, 1440, "2048x1152 (game, restart)"),
    )
    with tempfile.TemporaryDirectory(prefix="output-resolution-") as tmp:
        work = Path(tmp)
        source, exe = work / "fixture.cpp", work / "fixture.exe"
        source.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", str(source), "-o", str(exe)], check=True)
        for saved, handheld, override, mode, w, h, label in cases:
            (work / "hookshot_options.ini").write_text(f"output_res={saved}\nhandheld_profile={handheld}\n", encoding="ascii")
            env = os.environ.copy()
            for key in ("HOS_OUTPUT_RES", "HOS_HANDHELD", "HOS_RT_POOL_MB"):
                env.pop(key, None)
            if override is not None:
                env["HOS_OUTPUT_RES"] = override
            subprocess.run([str(exe), str(mode), str(w), str(h), label], cwd=work, env=env, check=True)
    print("PASS: output sizes, menu order, 1080p override, handheld and render-target budgets")


if __name__ == "__main__":
    main()
