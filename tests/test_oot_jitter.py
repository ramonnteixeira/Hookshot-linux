"""nx2/oot_jitter.py, NVIDIA DLSS's camera jitter: its sites are the loads of a view's projection where the game fills the
views' uniform buffers, and the C after each adds the jitter to the loaded registers only (the third lane: P[0][2], P[1][2]),
never to the game's memory, where the shadow cascades' fit and culling would see it too (#841: shadows flickered). The
snippets are built with gcc against a stand-in context and run. No game or GPU needed.

python -I -B tests/test_oot_jitter.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_jitter  # noqa: E402
import oot_wide  # noqa: E402

CC = "gcc"


class Image:
    def __init__(self, words):
        self.words = words

    def word(self, pc):
        return self.words[pc]


class Lifter:
    def __init__(self):
        self.after = {}


def lift(words):
    L = Lifter()
    n = oot_jitter.install(L, Image(words))
    return L, n


class SiteTests(unittest.TestCase):
    def test_the_three_loads(self):
        words = {oot_wide.MAIN + off: word for off, word, _, _ in oot_jitter.SITES}
        L, n = lift(words)
        self.assertEqual(n, 3)
        self.assertEqual(sorted(L.after), sorted(words))
        for text in L.after.values():
            self.assertIn("hos_dlss_jitter_clip(c->x[28] + 0x1a8", text)
            self.assertNotIn("recomp_store", text)   # (the game's memory is left alone)

    def test_another_version_left_out(self):
        words = {oot_wide.MAIN + off: word for off, word, _, _ in oot_jitter.SITES}
        first = oot_wide.MAIN + oot_jitter.SITES[0][0]
        words[first] ^= 1
        L, n = lift(words)
        self.assertEqual(n, 2)
        self.assertNotIn(first, L.after)

    def test_after_what_is_there(self):
        words = {oot_wide.MAIN + off: word for off, word, _, _ in oot_jitter.SITES}
        L = Lifter()
        pc = oot_wide.MAIN + oot_jitter.SITES[1][0]
        L.after[pc] = "/* before */"
        oot_jitter.install(L, Image(words))
        self.assertTrue(L.after[pc].startswith("/* before */\n"))

    def test_registers_get_the_jitter_in_their_third_lane(self):
        """Each snippet, built and run: the named registers' lane 2 (bytes 8..11) moves by the jitter's x or y, the rest of
        every register stays, and with no jitter (hos_dlss_jitter_clip 0) nothing moves"""
        cases = []
        for i, (_, _, xreg, yreg) in enumerate(oot_jitter.SITES):
            cases.append("static void site%d(Ctx *c) { %s }" % (i, oot_jitter._text(xreg, yreg)))
        runs = []
        for i, (_, _, xreg, yreg) in enumerate(oot_jitter.SITES):
            for on in (1, 0):
                runs.append("check(site%d, %d, %d, %d);" % (i, -1 if xreg is None else xreg, -1 if yreg is None else yreg, on))
        src = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
static int jitter_on;
static uint64_t asked;
int hos_dlss_jitter_clip(uint64_t projection, float out[2]) {
    asked = projection;
    if (!jitter_on) return 0;
    out[0] = 0.25f, out[1] = -0.5f;
    return 1;
}
%s
static int check(void (*site)(Ctx *), int xreg, int yreg, int on) {
    Ctx c, before;
    for (int r = 0; r < 32; r++) {
        c.x[r] = 0x1000u * r;
        const float lanes[4] = {r + 0.5f, r + 1.5f, 1.0f, r + 3.5f};
        memcpy(c.vreg[r], lanes, 16);
    }
    before = c;
    jitter_on = on, asked = 0;
    site(&c);
    if (asked != 0x1000u * 28 + 0x1a8) { printf("projection asked at %%llx\n", (unsigned long long)asked); return 1; }
    if (memcmp(c.x, before.x, sizeof c.x)) { puts("x registers changed"); return 1; }
    for (int r = 0; r < 32; r++) {
        float now[4], was[4];
        memcpy(now, c.vreg[r], 16), memcpy(was, before.vreg[r], 16);
        const float want = !on ? 1.0f : r == xreg ? 1.25f : r == yreg ? 0.5f : 1.0f;
        if (now[0] != was[0] || now[1] != was[1] || now[3] != was[3] || now[2] != want) {
            printf("v%%d: %%g %%g %%g %%g (lane 2 wanted %%g)\n", r, now[0], now[1], now[2], now[3], want);
            return 1;
        }
    }
    return 0;
}
int main(void) {
    int bad = 0;
#define check(...) bad |= check(__VA_ARGS__)
    %s
    return bad;
}
""" % ("\n".join(cases), "\n    ".join(runs))
        with tempfile.TemporaryDirectory(prefix="oot-jitter-") as tmp:
            c = os.path.join(tmp, "sites.c")
            exe = os.path.join(tmp, "sites")
            Path(c).write_text(src)
            subprocess.run([CC, "-std=c11", "-Wall", "-Werror", "-Wno-unused-function", c, "-o", exe], check=True)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="gcc")
    args, rest = ap.parse_known_args()
    CC = args.cc
    unittest.main(argv=[sys.argv[0]] + rest)
