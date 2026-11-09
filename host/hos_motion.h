/* (hookshot) A controller's motion sensors as OoT reads its six-axis state (host/pad_sdl.c fills it from SDL,
   host/nn_hid.c hands it to the game's GetSixAxisSensorStates). Header-only, no SDL or Windows, so
   tests/test_motion.py runs it as it is.

   OoT does not read the six-axis state as a plain sensor. What it aims with was found by spluton (issue 112, "Gyro
   aiming from SDL pads", tested on a DualSense) and corrected by Link-22 (the yaw rate's unit, tested on a GameSir T3
   Pro in Switch Pro mode, and the same feel as Eden on that pad):
   - the horizontal aim is the yaw rate, in the angular velocity's y slot, in turns a second (SDL's rad/s over 2 pi);
   - the vertical aim is the direction matrix, as a pure tilt about x (Rx(pitch)), the pitch rate integrated on its own;
   - the game folds the angles, the pitch rate and the acceleration into an aim recenter that cancels the tilt unless
     they are neutral: the angles and the pitch rate go out zero, the acceleration at rest (0, 0, -1).
   The rates are the pad's own, measured about the world's axes: an up vector from a complementary filter (the gyro's
   prediction pulled toward the measured gravity while it reads 0.65 to 1.35 G), the gyro projected onto right
   (pitch), up (yaw) and forward (roll). That makes the way the pad is held irrelevant: flat or upright, a turn toward
   the ceiling is up. The gyroscope's resting rate (its bias) is learned while the pad is still (both sensors steady, not
   merely slow) and taken off, and the vertical aim is pulled gently toward the tilt gravity measures, so it cannot drift.

   Everything is in SDL's gamepad frame (+X right, +Y up out of the face buttons, +Z toward the player; m/s^2 and
   rad/s). The defaults are the two testers': the yaw rate as is (turning left aims left), the pitch rate negated. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#define HOS_MOTION_G 9.80665f
#define HOS_MOTION_TAU 6.28318531f

typedef struct {
    float accel[3];       /* G (at rest, always: the game folds it into its recenter) */
    float gyro[3];        /* turns a second: the yaw rate in y, x and z zero */
    float rotation[3];    /* turns: zero (the game folds them into its recenter) */
    float direction[9];   /* rows: Rx(pitch), the vertical aim */
} HosMotionState;

/* one reading's state as GetSixAxisSensorStates hands it out: n counts the readings, dt_ns is the time since the one
   before (host/pad_sdl.c keeps the last HOS_MOTION_HISTORY) */
typedef struct {
    HosMotionState st;
    int64_t n, dt_ns;
} HosMotionSample;
#define HOS_MOTION_HISTORY 32

typedef struct {
    float bias[3];   /* rad/s, SDL's axes */
    float still_t;   /* seconds the pad has been still (bias learning starts after HOS_MOTION_STILL_S) */
    float learned_t; /* seconds of stillness learned from so far (fast at first, then slowly) */
    float gmean[3], gvar;   /* the gyro's short-window mean (rad/s) and variance (summed over its axes) */
    float amean, avar;      /* the accelerometer's magnitude, the same way (G) */
    int stats_ready;
    float last_accel[3], last_gyro[3];   /* (the last reading: a repeat of it is no new reading) */
    float pending_dt;                    /* (time from repeats, carried into the next new reading) */
    float up[3];     /* the world's up in the pad's frame (unit) */
    int up_ready;
    float pitch;     /* radians, the vertical aim's tilt */
    float tilt0;     /* radians, the pad's tilt about its x axis when the filter started (the aim's level) */
    int tilt_ready;
    float rates[3];  /* (for tests and logs) pitch, yaw, roll about the world's axes, rad/s */
    HosMotionState out;
} HosMotion;

/* stillness: the gyro's spread under HOS_MOTION_STILL_GYRO rad/s and the accelerometer's under HOS_MOTION_STILL_ACCEL
   G, about 1 G, for HOS_MOTION_STILL_S. A hand never holds that steady (its tremor alone is several times more), so a
   slow, steady turn is aim, not bias, however slow; and an offset of any size is learned on a table. */
#define HOS_MOTION_STILL_GYRO 0.01f
#define HOS_MOTION_STILL_ACCEL 0.01f
#define HOS_MOTION_STILL_S 0.5f
#define HOS_MOTION_WINDOW_S 0.2f   /* (the short window's time constant) */
#define HOS_MOTION_DT_MAX 0.1f     /* (a longer gap, a hitch, counts as this long) */
#define HOS_MOTION_TILT_S 1.0f     /* (the vertical aim's pull toward the measured tilt) */
#define HOS_MOTION_TILT_ROLL 0.25f /* (... only while |up[0]| is under this: about 15 degrees of roll) */

static inline void hos_motion_rest(HosMotionState *o, float pitch) {
    memset(o, 0, sizeof *o);
    o->accel[2] = -1;
    const float c = cosf(pitch), s = sinf(pitch);
    const float r[9] = {1, 0, 0, 0, c, -s, 0, s, c};
    memcpy(o->direction, r, sizeof r);
}

static inline void hos_motion_reset(HosMotion *m) {
    memset(m, 0, sizeof *m);
    m->up[1] = 1;
    hos_motion_rest(&m->out, 0);
}

static inline float hos_motion_wrap(float a) {
    while (a > 3.14159265f) a -= HOS_MOTION_TAU;
    while (a < -3.14159265f) a += HOS_MOTION_TAU;
    return a;
}

/* One reading: SDL's accelerometer (m/s^2) and gyroscope (rad/s), dt seconds after the last one. sens_h and sens_v
   scale the horizontal and the vertical aim (1: as the pad turns); inv_h and inv_v turn them the other way. Returns 0
   for a repeat of the last reading (no new reading: its time goes into the next), 1 otherwise. */
static inline int hos_motion_update(HosMotion *m, const float accel[3], const float gyro[3], float dt, float sens_h,
                                    float sens_v, int inv_h, int inv_v) {
    if (!(dt >= 0)) dt = 0;
    if (m->stats_ready && !memcmp(accel, m->last_accel, sizeof m->last_accel) &&
        !memcmp(gyro, m->last_gyro, sizeof m->last_gyro)) {
        m->pending_dt += dt;
        return 0;
    }
    memcpy(m->last_accel, accel, sizeof m->last_accel), memcpy(m->last_gyro, gyro, sizeof m->last_gyro);
    dt += m->pending_dt, m->pending_dt = 0;
    if (dt > HOS_MOTION_DT_MAX) dt = HOS_MOTION_DT_MAX;   /* (a hitch: its time, up to a limit) */
    const float alen = sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
    const float ag = alen / HOS_MOTION_G;
    /* the bias: learned from the short window's mean while the pad is still (both sensors steady, about 1 G) */
    if (!m->stats_ready) {
        memcpy(m->gmean, gyro, sizeof m->gmean), m->gvar = 1, m->amean = ag, m->avar = 1;
        m->stats_ready = 1;
    } else if (dt > 0) {
        const float k = dt / (HOS_MOTION_WINDOW_S + dt);
        float gv = 0;
        for (int i = 0; i < 3; i++) {
            const float d = gyro[i] - m->gmean[i];
            m->gmean[i] += d * k;
            gv += d * d;
        }
        m->gvar += (gv - m->gvar) * k;
        const float da = ag - m->amean;
        m->amean += da * k;
        m->avar += (da * da - m->avar) * k;
    }
    const int still = m->gvar < HOS_MOTION_STILL_GYRO * HOS_MOTION_STILL_GYRO &&
                      m->avar < HOS_MOTION_STILL_ACCEL * HOS_MOTION_STILL_ACCEL && m->amean > 0.9f && m->amean < 1.1f;
    m->still_t = still ? m->still_t + dt : 0;
    if (m->still_t >= HOS_MOTION_STILL_S && dt > 0) {
        const float k = dt / ((m->learned_t < 2 ? 0.25f : 3.0f) + dt);   /* (fast for the first 2 s still, then slowly) */
        for (int i = 0; i < 3; i++) m->bias[i] += (m->gmean[i] - m->bias[i]) * k;
        m->learned_t += dt;
    }
    const float w[3] = {gyro[0] - m->bias[0], gyro[1] - m->bias[1], gyro[2] - m->bias[2]};
    /* up: the gyro's prediction (a world vector seen from a frame turning at w turns the other way), pulled toward
       the measured gravity while it is about 1 G */
    const int valid = alen > 0.65f * HOS_MOTION_G && alen < 1.35f * HOS_MOTION_G;
    float *up = m->up;
    if (!m->up_ready) {
        if (valid) {
            for (int i = 0; i < 3; i++) up[i] = accel[i] / alen;
            m->up_ready = 1;
        }
    } else {
        float p[3] = {up[0] + (-w[1] * up[2] + w[2] * up[1]) * dt, up[1] + (-w[2] * up[0] + w[0] * up[2]) * dt,
                      up[2] + (-w[0] * up[1] + w[1] * up[0]) * dt};
        float n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (n > 0.001f) {
            for (int i = 0; i < 3; i++) p[i] /= n;
            if (valid) {
                const float k = dt / (0.5f + dt);   /* (a half-second pull) */
                for (int i = 0; i < 3; i++) p[i] += (accel[i] / alen - p[i]) * k;
                n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                if (n > 0.001f) for (int i = 0; i < 3; i++) p[i] /= n;
            }
            memcpy(up, p, sizeof p);
        }
    }
    /* right: the pad's own x, level (gravity taken out); when the pad points x up, its z instead */
    float right[3] = {1 - up[0] * up[0], -up[0] * up[1], -up[0] * up[2]};
    float rl = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (rl < 0.1f) {
        right[0] = -up[0] * up[2], right[1] = -up[1] * up[2], right[2] = 1 - up[2] * up[2];
        rl = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    }
    for (int i = 0; i < 3; i++) right[i] /= rl;
    const float fwd[3] = {right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2],
                          right[0] * up[1] - right[1] * up[0]};
    m->rates[0] = w[0] * right[0] + w[1] * right[1] + w[2] * right[2];
    m->rates[1] = w[0] * up[0] + w[1] * up[1] + w[2] * up[2];
    m->rates[2] = w[0] * fwd[0] + w[1] * fwd[1] + w[2] * fwd[2];
    const float sh = sens_h * (inv_h ? -1.0f : 1.0f), sv = -sens_v * (inv_v ? -1.0f : 1.0f);
    m->pitch = hos_motion_wrap(m->pitch + m->rates[0] * sv * dt);
    /* the vertical aim's drift: pulled gently toward the tilt gravity measures (the pad's tilt about its x axis since
       the filter started, scaled the same way), so leftover bias or timing error cannot add up. Only while the pad's
       x is about level (rolled under about 15 degrees): rolled, the tilt about the pad's own x moves faster than the
       pitch about the levelled x (by 1 / sqrt(1 - up[0]^2)), and the pull would drag the aim past each move
       (hckrman101); what builds up while rolled is corrected once it is level again. Not while shaken either. */
    if (m->up_ready && valid && fabsf(up[0]) < HOS_MOTION_TILT_ROLL) {
        const float tilt = atan2f(-up[2], up[1]);   /* (it grows at the x rate: d/dt = w[0]) */
        /* (anchored so the aim stays where it is: the first time the pad is level may be after it moved while rolled,
           or held tilted when the filter started; sv is never 0, the menu's least sensitivity is 10%) */
        if (!m->tilt_ready) m->tilt0 = hos_motion_wrap(tilt - m->pitch / sv), m->tilt_ready = 1;
        const float target = hos_motion_wrap(hos_motion_wrap(tilt - m->tilt0) * sv);
        m->pitch = hos_motion_wrap(m->pitch + hos_motion_wrap(target - m->pitch) * (dt / (HOS_MOTION_TILT_S + dt)));
    }
    hos_motion_rest(&m->out, m->pitch);
    m->out.gyro[1] = m->rates[1] * sh / HOS_MOTION_TAU;
    return 1;
}
