#include "../gpu/vk/dlss_math.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace dm = mkwvk::dlss_math;

static bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }
static void mul(const float* a, const float* b, float* out) {   // (row major 4x4: out = a * b)
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
}
static void apply(const float* m, const float* v, float* out) {
    for (int r = 0; r < 4; r++) out[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3] * v[3];
}
/// A camera as the game's: a perspective projection (w row 0 0 -1 0) after a view of yaw and a position, plus a jitter
static void camera(float yaw, float x, float z, const float jitter[2], float out[16]) {
    const float f = 2.41421f, a = 16.0f / 9.0f, n = 0.1f, fa = 1000.0f;
    float p[16] = {f / a, 0, 0, 0, 0, f, 0, 0, 0, 0, -(fa + n) / (fa - n), -2 * fa * n / (fa - n), 0, 0, -1, 0};
    p[2] += jitter[0] * -1.0f, p[6] += jitter[1] * -1.0f;   // (sead's offset: in the third column, times -w's -1)
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float v[16] = {c, 0, -s, -(c * x - s * z), 0, 1, 0, 0, s, 0, c, -(s * x + c * z), 0, 0, 0, 1};
    mul(p, v, out);
}
/// dlss_prep.comp's motion for a pixel at clip (x, y) and depth d: in render pixels (size w x h)
static void motion(const float reproject[16], float x, float y, float d, const float jitter[2], float w, float h, float out[2]) {
    const float clip[4] = {x, y, d, 1.0f};
    float before[4];
    apply(reproject, clip, before);
    const float then[2] = {before[0] / before[3], before[1] / before[3]};
    const float now[2] = {x - jitter[0], y - jitter[1]};
    out[0] = (then[0] - now[0]) * 0.5f * w, out[1] = (now[1] - then[1]) * 0.5f * h;
}

int main() {
    // the Halton sequence
    CHECK("halton base 2", near(dlss_halton(1, 2), 0.5f) && near(dlss_halton(2, 2), 0.25f) && near(dlss_halton(3, 2), 0.75f) &&
                               near(dlss_halton(4, 2), 0.125f));
    CHECK("halton base 3", near(dlss_halton(1, 3), 1 / 3.0f) && near(dlss_halton(2, 3), 2 / 3.0f) && near(dlss_halton(3, 3), 1 / 9.0f));
    CHECK("halton of 0", dlss_halton(0, 2) == 0.0f);
    for (int phases : {8, 16, 32, 64}) {
        float sum[2] = {0, 0};
        for (int i = 0; i < phases; i++) {
            float j[2];
            dlss_jitter_pixels((unsigned)i, phases, j);
            CHECK("jitter within the pixel", j[0] >= -0.5f && j[0] < 0.5f && j[1] >= -0.5f && j[1] < 0.5f);
            for (int k = 0; k < i; k++) {
                float o[2];
                dlss_jitter_pixels((unsigned)k, phases, o);
                CHECK("each phase its own offset", !(near(o[0], j[0]) && near(o[1], j[1])));
            }
            sum[0] += j[0], sum[1] += j[1];
        }
        CHECK("jitter centred on the pixel", std::fabs(sum[0] / phases) < 0.07f && std::fabs(sum[1] / phases) < 0.07f);
        float a[2], b[2];
        dlss_jitter_pixels(3, phases, a), dlss_jitter_pixels(3 + phases, phases, b);
        CHECK("the sequence repeats", a[0] == b[0] && a[1] == b[1]);
    }
    {   // (the same as host/rx_backend.c's sequence before it moved here)
        for (int frame = 0; frame < 40; frame++) {
            const int i = frame % 16 + 1;
            float x = 0.0f, y = 0.0f, f = 0.5f;
            for (int k = i; k > 0; k /= 2, f *= 0.5f) x += f * (float)(k % 2);
            f = 1.0f / 3.0f;
            for (int k = i; k > 0; k /= 3, f /= 3.0f) y += f * (float)(k % 3);
            float j[2];
            dlss_jitter_pixels((unsigned)frame, 16, j);
            CHECK("the sequence as before", near(j[0], x - 0.5f) && near(j[1], y - 0.5f));
        }
    }
    CHECK("no phases is one", [] { float j[2]; dlss_jitter_pixels(5, 0, j); return near(j[0], 0.0f) && near(j[1], 1 / 3.0f - 0.5f); }());

    // the sequence's length: 8 x (output / render) squared, 8 to 64
    CHECK("DLAA: 8", dlss_jitter_phases(2560, 1440, 2560, 1440) == 8);
    CHECK("Quality (2/3): 18", dlss_jitter_phases(1707, 960, 2560, 1440) == 18);
    CHECK("the game's 3/4: 14", dlss_jitter_phases(1536, 864, 2048, 1152) == 14);
    CHECK("Performance (1/2): 32", dlss_jitter_phases(1280, 720, 2560, 1440) == 32);
    CHECK("Ultra Performance (1/3): 64 at most", dlss_jitter_phases(683, 384, 2048, 1152) == 64);
    CHECK("no size: 8", dlss_jitter_phases(0, 0, 2048, 1152) == 8);

    // the reprojection: previous * inverse(current)
    const float none[2] = {0, 0};
    float cur[16], prev[16], r[16];
    camera(0.3f, 1.0f, 2.0f, none, cur);
    CHECK("same camera: identity", dm::reprojection(cur, cur, r));
    for (int i = 0; i < 16; i++) CHECK("identity", near(r[i], i % 5 == 0 ? 1.0f : 0.0f, 1e-4f));
    camera(0.35f, 1.2f, 1.9f, none, prev);
    CHECK("another camera", dm::reprojection(cur, prev, r));
    float back[16];
    mul(r, cur, back);
    for (int i = 0; i < 16; i++) CHECK("reprojection * current = previous", near(back[i], prev[i], 1e-3f));
    const float singular[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK("a singular camera", !dm::reprojection(singular, prev, r));

    // the jitter taken off: a jittered camera unjittered is the camera
    const float jit[2] = {0.0013f, -0.0021f};
    float jittered[16], plain[16];
    camera(0.3f, 1.0f, 2.0f, jit, jittered);
    camera(0.3f, 1.0f, 2.0f, none, plain);
    dm::unjitter(jittered, jit);
    for (int i = 0; i < 16; i++) CHECK("unjitter", near(jittered[i], plain[i], 1e-5f));

    // the motion of a still camera with a different jitter each frame is none, anywhere on the screen
    {
        const float j0[2] = {-0.0009f, 0.0016f}, j1[2] = {0.0012f, -0.0004f};
        float c1[16], c0[16], m[16], mv[2];
        camera(0.3f, 1.0f, 2.0f, j1, c1);
        camera(0.3f, 1.0f, 2.0f, j0, c0);
        dm::unjitter(c0, j0);
        CHECK("still camera", dm::reprojection(c1, c0, m));
        for (float x : {-0.9f, 0.0f, 0.7f})
            for (float y : {-0.5f, 0.3f})
                for (float d : {0.2f, 0.9f}) {
                    motion(m, x, y, d, j1, 1536, 864, mv);
                    CHECK("no motion from the jitter", near(mv[0], 0.0f, 1e-2f) && near(mv[1], 0.0f, 1e-2f));
                }
    }
    // a camera that turns: the motion is the turn's, the same with or without jitter
    {
        const float j0[2] = {-0.0009f, 0.0016f}, j1[2] = {0.0012f, -0.0004f};
        float a1[16], a0[16], b1[16], b0[16], ma[16], mb[16], va[2], vb[2];
        camera(0.32f, 1.0f, 2.0f, j1, a1), camera(0.3f, 1.0f, 2.0f, j0, a0);
        camera(0.32f, 1.0f, 2.0f, none, b1), camera(0.3f, 1.0f, 2.0f, none, b0);
        dm::unjitter(a0, j0);
        CHECK("turning camera", dm::reprojection(a1, a0, ma) && dm::reprojection(b1, b0, mb));
        // (the same point of the scene: on the jittered screen it is at the plain screen's place plus the jitter)
        float p[4] = {0.5f, 0.2f, -3.0f, 1.0f}, cb[4], ca[4];
        apply(b1, p, cb), apply(a1, p, ca);
        const float xb = cb[0] / cb[3], yb = cb[1] / cb[3], db = cb[2] / cb[3], xa = ca[0] / ca[3], ya = ca[1] / ca[3];
        motion(mb, xb, yb, db, none, 1536, 864, vb);
        motion(ma, xa, ya, db, j1, 1536, 864, va);
        CHECK("the turn's motion", std::fabs(vb[0]) > 1.0f && near(va[0], vb[0], 0.05f) && near(va[1], vb[1], 0.05f));
    }

    // the jitter as DLSS takes it: render pixels, y down
    float px[2];
    const float clip[2] = {2.0f / 1536, 2.0f / 864};
    dm::jitter_to_pixels(clip, 1536, 864, px);
    CHECK("jitter in pixels", near(px[0], 1.0f) && near(px[1], -1.0f));

    // which projections get the jitter (nx2/oot_jitter.py: the game's views' uniform buffers): the picture's perspective ones
    {
        const float f = 2.14451f, n = 0.1f;
        float scene[16] = {f / (16.0f / 9.0f), 0, 0, 0, 0, f, 0, 0, 0, 0, -1.00001f, -n, 0, 0, -1, 0};
        float out[2], expect[2];
        dlss_jitter_pixels(5, 8, expect);
        CHECK("the scene's projection", dlss_jitter_clip(scene, 0.0f, 5, 8, 3840, 2160, out) &&
                                            near(out[0], 2 * expect[0] / 3840) && near(out[1], 2 * expect[1] / 2160));
        // (in the jitter's clip units: the picture moves by out * w / 2 pixels, what jitter_to_pixels gives back)
        dm::jitter_to_pixels(out, 3840, 2160, px);
        CHECK("a sub-pixel offset", near(px[0], expect[0]) && near(px[1], -expect[1]));
        CHECK("none while DLSS is not drawing", !dlss_jitter_clip(scene, 0.0f, 5, 8, 0, 0, out));
        const float ortho[16] = {0.0984f, 0, 0, 5.78f, 0, 0.1535f, 0, 0.31f, 0, 0, -0.11f, 10.66f, 0, 0, 0, 1};
        CHECK("not a shadow map's (orthographic)", !dlss_jitter_clip(ortho, 0.0f, 5, 8, 3840, 2160, out));
        float square[16];
        std::memcpy(square, scene, sizeof square);
        square[0] = f;   // (1:1: the per-character shadow projector, a portrait)
        CHECK("not another ratio's", !dlss_jitter_clip(square, 0.0f, 5, 8, 3840, 2160, out));
        float wide[16];
        std::memcpy(wide, scene, sizeof wide);
        wide[0] = f / (21.0f / 9.0f);   // (an ultrawide window's: nx2/oot_wide.py gives the picture's cameras its ratio)
        CHECK("not 21:9 on a 16:9 window", !dlss_jitter_clip(wide, 0.0f, 5, 8, 3840, 2160, out));
        CHECK("21:9 on a 21:9 window", dlss_jitter_clip(wide, 21.0f / 9.0f, 5, 8, 5040, 2160, out));
        CHECK("16:9 still, on a 21:9 window", dlss_jitter_clip(scene, 21.0f / 9.0f, 5, 8, 5040, 2160, out));
    }
    std::puts("dlss math: all checks passed");
}
