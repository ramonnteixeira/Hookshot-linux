"""Native mouse look turns the camera controller's own angles (host/mouse_native.c).

The file is compiled on its own against a fake heap of 64 pages that holds one camera controller: its pitch limits
{-85, 89, -85, 89}, its pitch at +0x14 and its yaw at +0x2c. The mouse's motion, until the controller is found, is
given back to the stick; then it turns yaw and pitch by 0.1 degrees a count at sensitivity 1, pitch kept within the
limits and yaw wrapped at 180, up and down swapped with the menu's Invert mouse vertical (option 8). A controller no longer there (a new scene) gives the motion back again.
python -I -B tests/test_mouse_native.py   (Linux: host/posix's windows.h)
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HOS_H = r"""
#pragma once
#include <stdint.h>
#include <stddef.h>
extern uint8_t *fake_heap;
#define HOS_HEAP_BASE 0x1000000000ULL
#define GP(a) ((void *)(fake_heap + ((uint64_t)(a) - HOS_HEAP_BASE)))
extern uint64_t hos_heap_size;
void hos_log(const char *fmt, ...);
int64_t hos_now_ns(void);
"""

FIXTURE = r"""
#include "hos.h"
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <windows.h>
uint8_t *fake_heap;
uint64_t hos_heap_size = 64 * 4096;
double hos_mouse_sens = 1.0;
void hos_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc(10, stderr); }
int64_t hos_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static int32_t move_x, move_y, given_x, given_y;
static int captured = 1, menu_invert;
int rx_option(int id, int otherwise) { return id == 8 ? menu_invert : otherwise; }
int hos_mouse_take(int32_t *dx, int32_t *dy) { *dx = move_x, *dy = move_y; move_x = move_y = 0; return captured; }
void hos_mouse_give_back(int32_t dx, int32_t dy) { given_x += dx, given_y += dy; }
typedef struct { LPTHREAD_START_ROUTINE fn; void *arg; } Start;
static void *run(void *p) { Start s = *(Start *)p; free(p); s.fn(s.arg); return NULL; }
HANDLE CreateThread(void *attr, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id) {
    (void)attr, (void)stack, (void)flags, (void)id;
    Start *s = (Start *)malloc(sizeof *s);
    s->fn = fn, s->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, run, s)) return NULL;
    pthread_detach(t);
    return (HANDLE)1;
}
int hos_mouse_native(void);
static const uint64_t CTL = 10 * 4096 + 0x40;
static float getf(uint64_t o) { float f; memcpy(&f, fake_heap + o, 4); return f; }
static void putf(uint64_t o, float f) { memcpy(fake_heap + o, &f, 4); }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (pitch %g yaw %g)\n", __LINE__, #c, getf(CTL + 0x14), getf(CTL + 0x2c)); fails++; } } while (0)
static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }
static int turn(int32_t dx, int32_t dy) { move_x = dx, move_y = dy; return hos_mouse_native(); }
int main(void) {
    fake_heap = (uint8_t *)calloc(64, 4096);
    const float limits[4] = {-85, 89, -85, 89};
    memcpy(fake_heap + 20 * 4096, limits, sizeof limits);   /* (the limits without angles after them: not a camera) */
    putf(20 * 4096 + 0x14, NAN);
    memcpy(fake_heap + CTL, limits, sizeof limits);
    putf(CTL + 0x14, 10), putf(CTL + 0x2c, 30);
    CHECK(turn(5, 3) == -1 && given_x == 5 && given_y == 3);   /* none found yet: the stick takes it */
    for (int i = 0; i < 200 && turn(0, 0) != 1; i++) usleep(10000);   /* (the search's thread) */
    CHECK(turn(10, 4) == 1);
    CHECK(near(getf(CTL + 0x2c), 29.0f) && near(getf(CTL + 0x14), 10.4f));   /* right: yaw falls; down: pitch rises */
    turn(0, 100000);
    CHECK(near(getf(CTL + 0x14), 89.0f));   /* kept within the limits */
    turn(0, -100000);
    CHECK(near(getf(CTL + 0x14), -85.0f));
    putf(CTL + 0x2c, -179.5f);
    turn(10, 0);
    CHECK(near(getf(CTL + 0x2c), 179.5f));   /* wrapped */
    CHECK(near(getf(20 * 4096 + 0x2c), 0.0f));   /* the one without angles left alone */
    putf(CTL + 0x14, 0);
    menu_invert = 1;   /* the menu's Invert mouse vertical: down tilts up */
    turn(0, 10);
    CHECK(near(getf(CTL + 0x14), -1.0f));
    menu_invert = 0;
    captured = 0;
    CHECK(turn(7, 7) == 0);   /* not captured: nothing */
    captured = 1;
    memset(fake_heap + CTL, 0, 16);   /* a new scene: the controller is gone */
    given_x = given_y = 0;
    CHECK(turn(6, 2) == -1 && given_x == 6 && given_y == 2);
    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""


class MouseNativeTests(unittest.TestCase):
    def test_turns_the_controller(self):
        if sys.platform == "win32":
            self.skipTest("host/posix's windows.h stands in for Windows here")
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="hookshot-mouse-native-") as tmp:
            tmp = Path(tmp)
            (tmp / "hos.h").write_text(HOS_H, encoding="utf-8")
            # (beside the stand-in hos.h, so its #include "hos.h" finds that one)
            shutil.copy(ROOT / "host" / "mouse_native.c", tmp / "mouse_native.c")
            (tmp / "t.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / "t"
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-O1", "-g", "-I", str(tmp), "-I", str(ROOT / "host" / "posix"),
                                    str(tmp / "t.c"), str(tmp / "mouse_native.c"), "-o", str(binary), "-lm", "-lpthread"],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
