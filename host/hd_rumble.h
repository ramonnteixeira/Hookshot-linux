/* HD rumble for Nintendo Switch Pro Controllers and Joy-Cons, through SDL's effect call (host/pad_sdl.c).

   The game's vibration values (nn::hid VibrationValue: {amp_low, freq_low, amp_high, freq_high}, amplitudes 0 to 1,
   frequencies in Hz, one per side of the controller) are what the Switch sends its controllers, so a Switch controller
   can play them, both bands at their own frequency and on their own side, instead of two motor strengths.
   SDL's Switch driver (src/joystick/hidapi/SDL_hidapi_switch.c, SDL 3.2 on: HIDAPI_DriverSwitch_SendJoystickEffect)
   takes a whole rumble output report, 0x10, packet number, then 4 bytes per side, and writes it in place of its own
   rumble. SDL2's own driver and the other SDL drivers (Switch 2 controllers, the kernel's hid-nintendo, joysticks
   SDL combined, such as a Joy-Con pair) refuse it: those pads keep the motor path.

   The encoding is the one documented by the community's reverse engineering (Switchbrew's HID "Rumble data" page;
   dekuNukem's Nintendo_Switch_Reverse_Engineering rumble_data_table.md, written from its formulas and tables):
   - a frequency's code is round(log2(Hz / 10) * 32); the high band takes code - 0x60 times 4 (0x004 to 0x1FC, 9 bits:
     81.75 to 1252.57 Hz), the low band code - 0x40 (0x01 to 0x7F: 40.88 to 626.29 Hz);
   - an amplitude's code, 0 to 100 (0 to 1.003), is round(log2(8.7 a) * 32) from 32 up, round(log2(17 a) * 16) from 16
     to 31, and below that the table's 2^(1/4) steps (a = 2^(code / 4) / 119.6, code 1 = 0.0078); the high band takes
     code * 2, the low band code / 2 + 0x40 with the code's odd bit as its 9th bit (0x8000). Codes over 100 are past what
     the table calls safe for the actuators: 100 is the top, as SDL's own table stops there;
   - the 4 bytes of a side: high frequency's low 8 bits; high amplitude | the high frequency's 9th bit; low frequency |
     the low amplitude's 9th bit (0x80); the low amplitude's low 8 bits. Silence is 00 01 40 40 (both bands at 0, at
     320 and 160 Hz), as SDL sends it.

   Pure (no state, no SDL): tests/test_hd_rumble.py compiles it. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#define HD_RUMBLE_PACKET_SIZE 10          /* (SDL's SwitchCommonOutputPacket_t: type, number, 2 x 4 bytes) */
#define HD_RUMBLE_REPORT 0x10             /* (k_eSwitchOutputReportIDs_Rumble) */
#define HD_RUMBLE_AMP_TOP 100             /* the amplitude code 1.003 has: the top the table calls safe */
#define HD_RUMBLE_GAP_NS 32000000LL       /* the least time between two packets (SDL: 30 ms; faster can turn a
                                             Bluetooth controller off, or the motors miss them over USB) */
#define HD_RUMBLE_REFRESH_NS 40000000LL   /* a rumble that goes on is sent again this often (SDL refreshes its own
                                             every 50 ms: the controller stops one it is not sent again) */

/* What kind of Switch controller SDL says a pad is: 0 none of them (the motor path), else the side(s) it has. */
enum { HD_RUMBLE_NONE, HD_RUMBLE_PRO, HD_RUMBLE_JOYCON_LEFT, HD_RUMBLE_JOYCON_RIGHT, HD_RUMBLE_JOYCON_PAIR };
/* From SDL_GetGamepadType (SDL3: SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO 7, then JOYCON_LEFT, RIGHT, PAIR) or
   SDL_GameControllerGetType (SDL2: SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO 5, JOYCON_LEFT 11, RIGHT 12, PAIR 13). */
static inline int hd_rumble_kind(int sdl_version, int type) {
    if (sdl_version == 3) return type >= 7 && type <= 10 ? HD_RUMBLE_PRO + (type - 7) : HD_RUMBLE_NONE;
    if (sdl_version == 2) return type == 5 ? HD_RUMBLE_PRO : type >= 11 && type <= 13 ? HD_RUMBLE_JOYCON_LEFT + (type - 11) : HD_RUMBLE_NONE;
    return HD_RUMBLE_NONE;
}

/* A frequency's code, round(log2(Hz / 10) * 32), kept to [lo, hi]; a frequency that is not one (0, NaN) gets
   otherwise. */
static inline int hd_rumble_freq_code(float hz, int lo, int hi, int otherwise) {
    if (!(hz > 0.0f)) return otherwise;   /* (also NaN) */
    const double c = floor(log2((double)hz / 10.0) * 32.0 + 0.5);
    return c < lo ? lo : c > hi ? hi : (int)c;
}
/* An amplitude's code, 0 to HD_RUMBLE_AMP_TOP (see above). */
static inline int hd_rumble_amp_code(float amp) {
    if (!(amp > 0.0f)) return 0;   /* (also NaN) */
    double c;
    if (amp > 0.2275f) c = floor(log2(8.7 * amp) * 32.0 + 0.5);             /* (between codes 31 and 32: 0.225, 0.230) */
    else if (amp > 0.1150f) c = floor(log2(17.0 * amp) * 16.0 + 0.5);       /* (15 and 16: 0.112, 0.117) */
    else if (amp >= 0.0039f) c = floor(4.0 * log2(119.6 * amp) + 0.5);       /* (half of code 1's 0.0078: less is 0) */
    else return 0;
    if (c < 1) c = 1;   /* (code 1, 0.0078, is off the 2^(1/4) steps: the smallest that plays) */
    return c > HD_RUMBLE_AMP_TOP ? HD_RUMBLE_AMP_TOP : (int)c;
}

/* (issue 263, for HD rumble) The game's amplitudes as a Switch controller is to play them: OoT scales its rumble files
   by small gains (romfs RSDB/RumbleCall: 0.015 to 0.5, most effects 0.05 to 0.2), so what it asks for is mostly 0.02 to
   0.1 and played as it is a Pro Controller's actuators hardly move (a heart caught, 0.06 at 55 Hz, is not felt). Each
   band goes through the same curve as the motors' (nn_hid.c's rumble_motor, from Eden's SDL driver): (a + a^0.35) / 2,
   which keeps 0 and 1 and the order of the strengths (0.03 -> 0.16, 0.06 -> 0.22, 0.2 -> 0.38, 0.5 -> 0.64); what is
   below the encoding's smallest step (half of code 1) stays silent. Frequencies are kept. */
static inline float hd_rumble_felt_amp(float amp) {
    if (!(amp >= 0.0039f)) return 0.0f;   /* (also NaN) */
    const float felt = (amp + powf(amp, 0.35f)) * 0.5f;
    return felt > 1.0f ? 1.0f : felt;
}
static inline void hd_rumble_felt(const float v[4], float out[4]) {
    out[0] = hd_rumble_felt_amp(v[0]), out[1] = v[1];
    out[2] = hd_rumble_felt_amp(v[2]), out[3] = v[3];
}

/* (a single Joy-Con) both sides' values as one: each band at the stronger side's amplitude, with that side's frequency
   (the left's when they are equal). The game always sees a full Pro Controller and may send an effect to one side only;
   a lone Joy-Con has one side, and plays what either side asks for, as the motor path's max of the two sides does. */
static inline void hd_rumble_merge(const float left[4], const float right[4], float out[4]) {
    for (int b = 0; b < 4; b += 2) {
        const int r = right[b] > left[b];   /* (NaN: not stronger, the left kept) */
        out[b] = r ? right[b] : left[b], out[b + 1] = r ? right[b + 1] : left[b + 1];
    }
}

/* The silent side, 00 01 40 40. */
static inline void hd_rumble_neutral(uint8_t out[4]) {
    out[0] = 0x00, out[1] = 0x01, out[2] = 0x40, out[3] = 0x40;
}
/* One side's 4 bytes from the game's value {amp_low, freq_low, amp_high, freq_high}, its amplitudes scaled by percent
   (100 the game's own, 0 silence). Over 100, both bands get the same factor, the stronger stopping at 1 (as the motors'
   strength does: the bands keep their balance), and the top stays HD_RUMBLE_AMP_TOP. */
static inline void hd_rumble_encode(const float v[4], int percent, uint8_t out[4]) {
    float scale = percent > 0 ? (float)percent / 100.0f : 0.0f;
    const float top = v[0] > v[2] ? v[0] : v[2];
    if (scale > 1.0f && top * scale > 1.0f) scale = top > 1.0f ? 1.0f : 1.0f / top;   /* (NaN: not above, kept) */
    const int la = hd_rumble_amp_code(v[0] * scale), ha = hd_rumble_amp_code(v[2] * scale);
    if (!la && !ha) {
        hd_rumble_neutral(out);
        return;
    }
    const int lf = hd_rumble_freq_code(v[1], 0x41, 0xBF, 0x80) - 0x40;          /* (160 Hz when it has none) */
    const int hf = (hd_rumble_freq_code(v[3], 0x61, 0xDF, 0xA0) - 0x60) * 4;    /* (320 Hz) */
    const int ha2 = ha * 2, la9 = ((la & 1) << 15) | (la / 2 + 0x40);
    out[0] = (uint8_t)(hf & 0xFF);
    out[1] = (uint8_t)(ha2 | ((hf >> 8) & 0x01));
    out[2] = (uint8_t)(lf | ((la9 >> 8) & 0x80));
    out[3] = (uint8_t)(la9 & 0xFF);
}

/* The packet SDL's effect call takes, from the game's left and right sides' bytes: a Pro Controller (and a pair)
   plays each side on its own; a single Joy-Con plays its own side (SDL's packet has both: it gets it in both). For a
   single Joy-Con, host/pad_sdl.c passes the two sides merged (hd_rumble_merge) as both. */
static inline void hd_rumble_packet(int kind, const uint8_t left[4], const uint8_t right[4], uint8_t out[HD_RUMBLE_PACKET_SIZE]) {
    out[0] = HD_RUMBLE_REPORT;
    out[1] = 0;   /* (SDL numbers the packets itself) */
    const uint8_t *l = kind == HD_RUMBLE_JOYCON_RIGHT ? right : left, *r = kind == HD_RUMBLE_JOYCON_LEFT ? left : right;
    memcpy(out + 2, l, 4);
    memcpy(out + 6, r, 4);
}
static inline int hd_rumble_silent(const uint8_t packet[HD_RUMBLE_PACKET_SIZE]) {
    static const uint8_t n[4] = {0x00, 0x01, 0x40, 0x40};
    return !memcmp(packet + 2, n, 4) && !memcmp(packet + 6, n, 4);
}

/* Whether a pad's packet goes out now: packet the one the game asks for, sent the last one sent (silence before the
   first), sent_ns when, ever whether there was one. Never sooner than HD_RUMBLE_GAP_NS after the last; a change goes
   at once, then the latest when the gap is over (what the game asks for in between is not sent: a newer value takes its
   place); a rumble that goes on is sent again every HD_RUMBLE_REFRESH_NS; silence that was sent is not sent again. */
static inline int hd_rumble_due(const uint8_t packet[HD_RUMBLE_PACKET_SIZE], const uint8_t sent[HD_RUMBLE_PACKET_SIZE],
                                int ever, long long now, long long sent_ns) {
    if (ever && now - sent_ns < HD_RUMBLE_GAP_NS) return 0;
    if (memcmp(packet + 2, sent + 2, 8)) return 1;
    return ever && !hd_rumble_silent(sent) && now - sent_ns >= HD_RUMBLE_REFRESH_NS;
}

/* A pad's HD rumble state, as the poll moves it: unknown until its first packet; on while SDL takes them; off (the
   motor path, for as long as the pad is open) once SDL refuses one, or when it has no effect call, is no Switch
   controller, or HOS_HD_RUMBLE=0. */
enum { HD_RUMBLE_UNKNOWN, HD_RUMBLE_ON, HD_RUMBLE_OFF };
static inline int hd_rumble_start(int kind, int have_call, int env_off) {
    return kind != HD_RUMBLE_NONE && have_call && !env_off ? HD_RUMBLE_UNKNOWN : HD_RUMBLE_OFF;
}
static inline int hd_rumble_after_send(int ok) { return ok ? HD_RUMBLE_ON : HD_RUMBLE_OFF; }
