/* host/hos_motion.h with synthetic SDL readings: no SDL, controller or game. tests/test_motion.py builds and runs it.

   A simulated pad: its true up vector (the world's up in its frame) turns with the true rotation, the accelerometer
   reads it (times G), and the gyroscope reads the true rotation plus an offset (its bias) and noise (a tiny one, a
   real sensor's, so no two readings repeat; or a hand's tremor). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "hos_motion.h"

static int failures;
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (fabs(_a - _b) > (tol)) { \
    fprintf(stderr, "FAIL %s:%d: %s = %.5f, want %.5f\n", __FILE__, __LINE__, #a, _a, _b); failures++; } } while (0)

static float su[3];          /* the true up, in the pad's frame */
static float offset[3];      /* the gyroscope's bias */
static float tremor;         /* the noise's size, rad/s (each axis, uniform) */
static unsigned rng = 1;
static float noise(void) {
    rng = rng * 1664525u + 1013904223u;
    return ((float)(rng >> 8) / 16777216.0f * 2 - 1);
}

static void pose(float x, float y, float z) {
    const float n = sqrtf(x * x + y * y + z * z);
    su[0] = x / n, su[1] = y / n, su[2] = z / n;
}
#define FLAT 0, 1, 0        /* face up on a table: gravity's reaction on SDL's +Y */
#define UPRIGHT 0, 0, -1    /* held upright, face to the player: the top (SDL's -Z) up */

/* the true rotation (gx, gy, gz) rad/s for seconds, read every dt */
static void run_dt(HosMotion *m, float gx, float gy, float gz, float seconds, float dt, float sens_h, float sens_v,
                   int inv_h, int inv_v) {
    const int n = (int)lroundf(seconds / dt);
    for (int i = 0; i < n; i++) {
        const float w[3] = {gx, gy, gz};
        const float a[3] = {su[0] * HOS_MOTION_G + 1e-5f * noise(), su[1] * HOS_MOTION_G + 1e-5f * noise(),
                            su[2] * HOS_MOTION_G + 1e-5f * noise()};
        float g[3];
        for (int k = 0; k < 3; k++) g[k] = w[k] + offset[k] + (tremor > 0 ? tremor : 1e-6f) * noise();
        hos_motion_update(m, a, g, dt, sens_h, sens_v, inv_h, inv_v);
        /* the truth turns: a world vector seen from a frame turning at w turns the other way */
        for (int s = 0; s < 10; s++) {
            const float h = dt / 10;
            const float p[3] = {su[0] + (-w[1] * su[2] + w[2] * su[1]) * h, su[1] + (-w[2] * su[0] + w[0] * su[2]) * h,
                                su[2] + (-w[0] * su[1] + w[1] * su[0]) * h};
            pose(p[0], p[1], p[2]);
        }
    }
}
static void run(HosMotion *m, float gx, float gy, float gz, float seconds, float sens_h, float sens_v, int inv_h,
                int inv_v) {
    run_dt(m, gx, gy, gz, seconds, 0.004f, sens_h, sens_v, inv_h, inv_v);
}
static void start(HosMotion *m, float x, float y, float z) {   /* a fresh filter, the pad at rest, no bias or tremor */
    hos_motion_reset(m);
    pose(x, y, z);
    offset[0] = offset[1] = offset[2] = 0;
    tremor = 0;
}

static void neutral(const HosMotion *m) {   /* what the game folds into its recenter: always at rest */
    NEAR(m->out.accel[0], 0, 1e-6);
    NEAR(m->out.accel[1], 0, 1e-6);
    NEAR(m->out.accel[2], -1, 1e-6);
    NEAR(m->out.gyro[0], 0, 1e-6);
    NEAR(m->out.gyro[2], 0, 1e-6);
    NEAR(m->out.rotation[0] + m->out.rotation[1] + m->out.rotation[2], 0, 1e-6);
    NEAR(m->out.direction[0], 1, 1e-6);   /* (a tilt about x only) */
}

int main(void) {
    HosMotion m;

    /* before any reading: a pad lying still */
    hos_motion_reset(&m);
    neutral(&m);
    NEAR(m.out.direction[4] + m.out.direction[8], 2, 1e-6);

    /* lying still with a gyroscope that reads a small rate: learned and taken off, no aim moves */
    start(&m, FLAT);
    offset[0] = 0.012f, offset[1] = -0.008f, offset[2] = 0.010f;
    run(&m, 0, 0, 0, 4, 1, 1, 0, 0);
    NEAR(m.out.gyro[1], 0, 1e-4);
    NEAR(m.pitch, 0, 0.02);
    neutral(&m);

    /* a large offset (0.2 rad/s, past what a rate threshold would call still) is learned too */
    start(&m, FLAT);
    offset[1] = 0.2f;
    run(&m, 0, 0, 0, 4, 1, 1, 0, 0);
    NEAR(m.bias[1], 0.2, 1e-3);

    /* a slow, steady turn in the hand (3 degrees a second, with a hand's tremor) is aim: not learned away */
    start(&m, FLAT);
    run(&m, 0, 0, 0, 1, 1, 1, 0, 0);   /* (on the table first) */
    tremor = 0.03f;
    run(&m, 0, 0.0524f, 0, 2, 1, 1, 0, 0);
    NEAR(m.bias[1], 0, 1e-3);

    /* turning left (about SDL's +Y, the pad flat) at one turn a second: the yaw slot reads 1 turn a second */
    start(&m, FLAT);
    run(&m, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, 0, HOS_MOTION_TAU, 0, 0.1f, 1, 1, 0, 0);
    NEAR(m.out.gyro[1], 1, 0.02);
    neutral(&m);

    /* tilting the front up (about SDL's +X) at 1 rad/s for half a second: the tilt matrix turns -0.5 rad, the rest
       neutral (the pitch rate itself never goes out), and it stays there while held */
    start(&m, FLAT);
    run(&m, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, 1, 0, 0, 0.5f, 1, 1, 0, 0);
    NEAR(m.pitch, -0.5, 0.03);
    NEAR(m.out.direction[4], cosf(m.pitch), 1e-5);
    NEAR(m.out.direction[5], -sinf(m.pitch), 1e-5);
    NEAR(m.out.direction[7], sinf(m.pitch), 1e-5);
    NEAR(m.out.gyro[1], 0, 0.01);
    neutral(&m);
    run(&m, 0, 0, 0, 3, 1, 1, 0, 0);
    NEAR(m.pitch, -0.5, 0.03);

    /* rolled 30 degrees, a tilt of the nose up (about the levelled x) moves the aim by the tilt and it stays there:
       the pull, which measures about the pad's own x, waits until the pad is level again (hckrman101) */
    start(&m, -0.5f, 0.8660254f, 0);
    run(&m, 0, 0, 0, 0.5f, 1, 1, 0, 0);
    {
        float r[3] = {1 - su[0] * su[0], -su[0] * su[1], -su[0] * su[2]};
        const float n = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        run(&m, r[0] / n, r[1] / n, r[2] / n, 0.5f, 1, 1, 0, 0);
    }
    NEAR(m.pitch, -0.5, 0.03);
    run(&m, 0, 0, 0, 3, 1, 1, 0, 0);
    NEAR(m.pitch, -0.5, 0.03);
    /* ... then rolled back to level (about the pad's z, which keeps the nose's height): the pull starts there,
       anchored to the aim as it is, so the aim neither jumps nor drifts back to level. This filter started rolled, the
       case behind Link-22's "wrong after turning Motion controls on while not level" (hckrman101) */
    for (int i = 0; i < 80 && su[0] < 0; i++) run(&m, 0, 0, 0.5236f, 0.02f, 1, 1, 0, 0);
    NEAR(su[0], 0, 0.02);
    {
        const float p0 = m.pitch;
        NEAR(p0, -0.5, 0.08);
        run(&m, 0, 0, 0, 3, 1, 1, 0, 0);
        NEAR(m.pitch, p0, 0.02);
    }

    /* a leftover bias the filter cannot learn (the pad in the hand) does not drift the vertical aim: 0.01 rad/s for a
       minute would be 0.6 rad of tilt summed open loop */
    start(&m, FLAT);
    run(&m, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    offset[0] = 0.01f, tremor = 0.03f;
    run(&m, 0, 0, 0, 60, 1, 1, 0, 0);
    NEAR(m.pitch, 0, 0.03);

    /* held upright the same turn left is about the pad's -Z: the yaw slot still reads +1 (how the pad is held does
       not matter) */
    start(&m, UPRIGHT);
    run(&m, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, 0, 0, -HOS_MOTION_TAU, 0.1f, 1, 1, 0, 0);
    NEAR(m.out.gyro[1], 1, 0.02);

    /* the sensitivities (each its own axis) and the inverts; the tilt's pull keeps the scaled tilt */
    start(&m, FLAT);
    run(&m, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, 0, HOS_MOTION_TAU, 0, 0.1f, 2, 0.5f, 1, 0);
    NEAR(m.out.gyro[1], -2, 0.04);
    NEAR(m.pitch, 0, 0.01);
    start(&m, FLAT);
    run(&m, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, 1, 0, 0, 0.5f, 3, 1, 0, 1);
    NEAR(m.pitch, 0.5, 0.03);
    NEAR(m.out.gyro[1], 0, 0.01);
    start(&m, FLAT);
    run(&m, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, 1, 0, 0, 0.5f, 1, 2, 0, 0);
    NEAR(m.pitch, -1.0, 0.05);
    run(&m, 0, 0, 0, 3, 1, 2, 0, 0);
    NEAR(m.pitch, -1.0, 0.05);

    /* picked up from flat to upright without the gyro seeing it: up settles on the measured gravity */
    start(&m, FLAT);
    run(&m, 0, 0, 0, 0.5f, 1, 1, 0, 0);
    pose(UPRIGHT);
    run(&m, 0, 0, 0, 5, 1, 1, 0, 0);
    NEAR(m.up[2], -1, 0.01);

    /* the real time between readings: polls 1 ms apart (Linux, a 1 kHz pad) and 60 ms apart (a hitch) both add up */
    start(&m, FLAT);
    run_dt(&m, 0, 0, 0, 0.1f, 0.001f, 1, 1, 0, 0);
    run_dt(&m, 1, 0, 0, 0.5f, 0.001f, 1, 1, 0, 0);
    NEAR(m.pitch, -0.5, 0.03);
    start(&m, FLAT);
    run_dt(&m, 0, 0, 0, 0.12f, 0.06f, 1, 1, 0, 0);
    run_dt(&m, 1, 0, 0, 0.48f, 0.06f, 1, 1, 0, 0);
    NEAR(m.pitch, -0.48, 0.03);

    /* a stall (a second between readings) counts as HOS_MOTION_DT_MAX, not a jump (shaken: no tilt pull) */
    start(&m, FLAT);
    run(&m, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    {
        const float a[3] = {0, 2 * HOS_MOTION_G, 0}, g[3] = {1, 0, 0};
        hos_motion_update(&m, a, g, 1.0f, 1, 1, 0, 0);
    }
    NEAR(m.pitch, -HOS_MOTION_DT_MAX, 2e-3);

    /* a repeat of the last reading is no reading: its time goes into the next */
    start(&m, FLAT);
    run(&m, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    {
        const float a[3] = {0, 2 * HOS_MOTION_G, 0}, g0[3] = {0, 0, 0}, g1[3] = {1, 0, 0};
        const float p0 = m.pitch;
        NEAR(hos_motion_update(&m, a, g0, 0.004f, 1, 1, 0, 0), 1, 0);
        NEAR(hos_motion_update(&m, a, g0, 0.004f, 1, 1, 0, 0), 0, 0);
        NEAR(hos_motion_update(&m, a, g1, 0.004f, 1, 1, 0, 0), 1, 0);
        NEAR(m.pitch - p0, -0.008, 2e-4);
    }

    if (failures) return 1;
    puts("motion: ok");
    return 0;
}
