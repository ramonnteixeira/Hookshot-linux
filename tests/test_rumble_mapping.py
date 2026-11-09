"""The game's HD rumble goes to a controller's heavy and light motors by frequency, scaled by the menu's Rumble strength.

host/nn_hid.c rumble_map: each band's strength (rumble_motor) is split between the heavy and light motors by its
frequency (rumble_heavy_share: all heavy up to 160 Hz, all light from 200 Hz, a constant-power crossfade between, on a
log scale). At the Switch's default frequencies and the ones OoT sends, every band lands on the motor it did before, at
the same strength. host/pad_sdl.c rumble_strength: the Insert menu's Rumble strength, in percent, on the motors'
0..65535; above 100% both motors are scaled by the same factor when the stronger would pass the top.

Run with: python -I -B tests/test_rumble_mapping.py
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HID = ROOT / "host" / "nn_hid.c"
PAD = ROOT / "host" / "pad_sdl.c"


def function(path, name):
    text = path.read_text(encoding="utf-8")
    found = re.search(re.escape(name) + r"\([^;{]*\)\s*\{", text)   # (the definition, not a declaration)
    if not found:
        raise RuntimeError("no definition of " + name)
    at = found.start()
    start = found.end() - 1
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == "{"
        depth -= text[i] == "}"
        if depth == 0:
            return text[at:i + 1]
    raise RuntimeError("no end for " + name)


def defines(path, prefix):
    return "\n".join(re.findall(r"^#define %s\w* .*$" % prefix, path.read_text(encoding="utf-8"), re.M))


FIXTURE = r"""
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %%d %%s\n", __LINE__, #x); return 1; } } while (0)
%(defines)s
%(motor)s
%(share)s
%(map)s
%(strength)s
/* (the documented split, worked out again: 1 at 160 Hz, 0 at 200 Hz, log-linear between) */
static float doc_share(float f) {
    return f <= 160.0f ? 1.0f : f >= 200.0f ? 0.0f : 1.0f - logf(f / 160.0f) / logf(200.0f / 160.0f);
}
static uint16_t strength1(unsigned heavy, unsigned light, int percent, int which) {
    uint16_t out[2];
    rumble_strength(heavy, light, percent, out);
    return out[which];
}
/* (the mapping before the frequency split: each band its own motor, (a + a^0.35) / 2 and less above a start frequency) */
static float old_band(float amp, float freq, float start, float width) {
    if (amp <= 0.0f) return 0.0f;
    const float scale = freq > start ? fmaxf(1.0f - (freq - start) / width, 0.3f) : 1.0f;
    return fminf((amp + powf(amp, 0.35f)) * 0.5f * scale, 1.0f);
}
static int near(float a, float b) { return fabsf(a - b) <= 1e-5f; }
static int near_by(float a, float b, float e) { return fabsf(a - b) <= e; }
int main(void) {
    /* 100%%, the defaults and what OoT sends (low about 100-130 Hz, high 250-300 Hz): the same as before, where the low band
       drove the heavy motor and the high band the light one, each with rumble_motor's falloff */
    static const float amps[] = {0.0f, 0.005f, 0.01f, 0.02f, 0.06f, 0.3f, 1.0f};
    static const float lows[] = {40.0f, 102.0f, 116.0f, 126.0f, 140.0f, 160.0f};
    static const float highs[] = {200.0f, 220.0f, 240.0f, 252.0f, 287.0f, 300.0f, 320.0f, 600.0f, 1250.0f};
    for (int a = 0; a < 7; a++)
        for (int b = 0; b < 7; b++)
            for (int l = 0; l < 6; l++)
                for (int h = 0; h < 9; h++) {
                    const float v[4] = {amps[a], lows[l], amps[b], highs[h]};
                    float m[2];
                    rumble_map(v, m);
                    CHECK(near(m[0], old_band(v[0], v[1], 140.0f, 400.0f)));
                    CHECK(near(m[1], old_band(v[2], v[3], 200.0f, 700.0f)));
                    const unsigned lo = (unsigned)(m[0] * 65535), hi = (unsigned)(m[1] * 65535);   /* (hos_sdl_rumble) */
                    CHECK(strength1(lo, hi, 100, 0) == lo && strength1(lo, hi, 100, 1) == hi);
                }
    {   /* the idle value (GetActualVibrationValue) is silence */
        const float v[4] = {0.0f, 160.0f, 0.0f, 320.0f};
        float m[2];
        rumble_map(v, m);
        CHECK(m[0] == 0.0f && m[1] == 0.0f);
    }

    /* the share: all heavy up to 160 Hz, all light from 200 Hz, falling between, half at their geometric middle */
    CHECK(rumble_heavy_share(40.0f) == 1.0f && rumble_heavy_share(160.0f) == 1.0f);
    CHECK(rumble_heavy_share(200.0f) == 0.0f && rumble_heavy_share(240.0f) == 0.0f && rumble_heavy_share(1250.0f) == 0.0f);
    CHECK(rumble_heavy_share(0.0f) == 1.0f && rumble_heavy_share(NAN) == 1.0f);
    CHECK(near(rumble_heavy_share(sqrtf(160.0f * 200.0f)), 0.5f));
    for (float f = 150.0f; f <= 260.0f; f += 0.5f) CHECK(near(rumble_heavy_share(f), doc_share(f)));
    float last = 1.0f;
    for (float f = 160.0f; f <= 200.0f; f += 0.5f) {
        const float s = rumble_heavy_share(f);
        CHECK(s >= 0.0f && s <= 1.0f && s <= last);
        last = s;
    }

    {   /* a high band played low reaches the heavy motor, at its full strength */
        const float v[4] = {0.0f, 160.0f, 0.3f, 100.0f};
        float m[2];
        rumble_map(v, m);
        CHECK(m[0] == rumble_motor(0.3f, 100.0f, 200.0f, 700.0f) && m[0] > 0.0f && m[1] == 0.0f);
    }
    {   /* a low band played high, the light one (with the low band's falloff) */
        const float v[4] = {0.3f, 320.0f, 0.0f, 320.0f};
        float m[2];
        rumble_map(v, m);
        CHECK(m[0] == 0.0f && m[1] == rumble_motor(0.3f, 320.0f, 140.0f, 400.0f) && m[1] > 0.0f);
    }
    for (float f = 170.0f; f < 200.0f; f += 3.0f) {   /* in the crossfade, both motors, at constant power, split as documented */
        for (int band_is_high = 0; band_is_high < 2; band_is_high++) {
            const float v[4] = {band_is_high ? 0.0f : 0.3f, band_is_high ? 160.0f : f, band_is_high ? 0.3f : 0.0f,
                                band_is_high ? f : 320.0f};
            float m[2];
            rumble_map(v, m);
            const float band = band_is_high ? rumble_motor(0.3f, f, 200.0f, 700.0f) : rumble_motor(0.3f, f, 140.0f, 400.0f);
            CHECK(m[0] > 0.0f && m[1] > 0.0f && m[0] <= band && m[1] <= band);
            CHECK(near(m[0] * m[0] + m[1] * m[1], band * band));
            CHECK(near_by(m[0] * m[0] / (band * band), doc_share(f), 1e-4f));
            CHECK(near_by(m[1] * m[1] / (band * band), 1.0f - doc_share(f), 1e-4f));
        }
    }
    {   /* OoT's high band (about 250-300 Hz) is clear of the crossfade, with room: all light */
        for (float f = 200.0f; f <= 320.0f; f += 1.0f) {
            const float v[4] = {0.0f, 160.0f, 0.3f, f};
            float m[2];
            rumble_map(v, m);
            CHECK(m[0] == 0.0f && m[1] == rumble_motor(0.3f, f, 200.0f, 700.0f));
        }
    }

    /* the strength: Off is silence, it grows with the percentage and the motor's value, and stops at the top */
    static const int percents[] = {0, 25, 50, 75, 100, 125, 150};
    static const unsigned values[] = {0, 1, 300, 6880, 20000, 43690, 50000, 65535};
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            const unsigned h = values[i], l = values[j];
            CHECK(strength1(h, l, 0, 0) == 0 && strength1(h, l, 0, 1) == 0 && strength1(h, l, -5, 0) == 0);
            for (int p = 1; p < 7; p++) {
                CHECK(strength1(h, l, percents[p], 0) >= strength1(h, l, percents[p - 1], 0));
                CHECK(strength1(h, l, percents[p], 1) >= strength1(h, l, percents[p - 1], 1));
            }
            for (int p = 0; p < 5; p++) {   /* up to 100%%: each motor on its own, as it was */
                CHECK(strength1(h, l, percents[p], 0) == h * (unsigned)percents[p] / 100u);
                CHECK(strength1(h, l, percents[p], 1) == l * (unsigned)percents[p] / 100u);
            }
            for (int p = 5; p < 7; p++) {   /* above: the balance kept; the stronger at most the top, at it when it would pass */
                const unsigned long long sh = (unsigned long long)h * (unsigned)percents[p] / 100u;
                const unsigned long long sl = (unsigned long long)l * (unsigned)percents[p] / 100u;
                const unsigned oh = strength1(h, l, percents[p], 0), ol = strength1(h, l, percents[p], 1);
                const unsigned top = oh > ol ? oh : ol;
                if (sh > 0xFFFF || sl > 0xFFFF) CHECK(top == 0xFFFF);
                else CHECK(oh == sh && ol == sl);
                if (h && l) CHECK(fabs((double)oh / ol - (double)h / l) <= (double)h / l * 2.0 / (double)(ol < oh ? ol : oh) + 1e-9);
            }
        }
    CHECK(strength1(20000, 10000, 50, 0) == 10000 && strength1(20000, 10000, 150, 0) == 30000 && strength1(20000, 10000, 150, 1) == 15000);
    CHECK(strength1(50000, 30000, 150, 0) == 0xFFFF && strength1(50000, 30000, 150, 1) == 39321);   /* 5:3, not 65535:45000 */
    CHECK(strength1(30000, 50000, 150, 0) == 39321 && strength1(30000, 50000, 150, 1) == 0xFFFF);
    CHECK(strength1(65535, 65535, 150, 0) == 0xFFFF && strength1(65535, 65535, 150, 1) == 0xFFFF);
    CHECK(strength1(43690, 0, 150, 0) == 65535 && strength1(43690, 0, 150, 1) == 0);
    return 0;
}
"""


class RumbleMappingTests(unittest.TestCase):
    def test_the_poll_scales_both_motors_by_the_menu(self):
        poll = re.sub(r"\s+", " ", function(PAD, "static void pads_poll"))
        self.assertRegex(poll, r"\brx_option\(\s*9\s*,\s*100\s*\)")
        call = re.search(r"\brumble_strength\(([^;]*)\);", poll)
        self.assertIsNotNone(call, "the poll does not scale the rumble by the menu's strength")
        self.assertRegex(call.group(1), r"rumble_low\b.*rumble_high\b")   # (heavy first, light second)
        self.assertLess(call.start(), poll.index("rumble_due("))   # (Off is a change: a stop is sent)
        self.assertRegex(re.sub(r"\s+", " ", function(HID, "static void vibration_send")), r"\brumble_map\( ?v ?, ?amp\[dev\] ?\)")

    def test_mapping_and_strength(self):
        fixture = FIXTURE % {
            "defines": defines(HID, "RUMBLE_"),
            "motor": function(HID, "static float rumble_motor"),
            "share": function(HID, "static float rumble_heavy_share"),
            "map": function(HID, "static void rumble_map"),
            "strength": function(PAD, "static void rumble_strength"),
        }
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "rumble.c")
            with open(path, "w", encoding="utf-8") as f:
                f.write(fixture)
            exe = os.path.join(d, "rumble")
            cc = os.environ.get("CC", "cc")
            made = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", path, "-o", exe, "-lm"],
                                  capture_output=True, text=True)
            self.assertEqual(made.returncode, 0, made.stderr)
            ran = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)


if __name__ == "__main__":
    unittest.main()
