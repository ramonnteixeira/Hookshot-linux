/* (hookshot) Native mouse look (the Insert menu's "Mouse camera", off by default; HOS_MOUSE_NATIVE=1 or 0 overrides it):
   the captured mouse turns the camera itself, its yaw and pitch in degrees, with no decay, no response curve and no top
   speed, in place of the mouse as the right stick (nn_hid.c's mouse_stick).

   Where the angles are (found by snapshotting the heap while the right stick turned the camera, in Kokiri Forest):
   the player camera's controller holds its pitch limits as {-85, 89, -85, 89} (four floats, degrees), its distance
   after them, its pitch at +0x14 from them and its yaw at +0x2c. Yaw falls as the view turns right and pitch rises as
   it tilts down. The output beside it (pitch and yaw side by side) is recomputed by the game every frame: writing it
   does nothing.

   The limits are looked for in the heap on a thread of their own (about a second over the heap's 2 GB in use, which
   took the game's thread that long in the test build), when the mouse first moves and whenever they are no longer
   where they were (a new scene); at most every 3 s, then less often while none is found (up to 30 s). Until one is
   found the motion is given back and the mouse turns the camera as the stick does.

   The sensitivity is the mouse's (the menu's Mouse sensitivity, - and =): 0.1 degrees a count at 1. HOS_MOUSE_INVERT_Y=1
   or the menu's Invert mouse vertical flips up and down. */
#include "hos.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int hos_mouse_take(int32_t *dx, int32_t *dy);                                   /* display.c */
void hos_mouse_give_back(int32_t dx, int32_t dy);                               /* display.c */
extern double hos_mouse_sens;

enum { MAX_T = 8, PITCH_AT = 0x14, YAW_AT = 0x2c, PAGE = 4096 };
static const float limits_sig[4] = {-85.0f, 89.0f, -85.0f, 89.0f};
static uint64_t ctl[MAX_T];                 /* the controllers in use: where their limits are */
static size_t nctl;
static uint64_t found[MAX_T];               /* the search's answer, taken up by the next motion */
static size_t nfound;
static volatile LONG searching, answered, busy;
static int64_t next_search, wait_ns = 3000000000LL;
static long long applied, given_back;

/* Which heap pages the process has touched: Windows says which are in its working set; elsewhere every page is read
   (an untouched page of an anonymous mapping reads as the shared zero page there, and zero pages are left out) */
#ifdef _WIN32
typedef struct { void *va; ULONG_PTR attr; } WsInfo;
typedef BOOL(WINAPI *QwsFn)(HANDLE, void *, DWORD);
#endif

/* a byte a heap page: 1 if the process has it (the caller frees it) */
static uint8_t *pages_in_use(uint64_t pages) {
    uint8_t *use = (uint8_t *)calloc((size_t)pages, 1);
    if (!use) return NULL;
#ifdef _WIN32
    QwsFn qws = (QwsFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    if (qws) {
        enum { BATCH = 65536 };
        WsInfo *info = (WsInfo *)malloc(sizeof(WsInfo) * BATCH);
        for (uint64_t i = 0; info && i < pages; i += BATCH) {
            const uint64_t n = pages - i < BATCH ? pages - i : BATCH;
            for (uint64_t k = 0; k < n; k++) info[k].va = GP(HOS_HEAP_BASE + (i + k) * PAGE), info[k].attr = 0;
            if (qws(GetCurrentProcess(), info, (DWORD)(n * sizeof(WsInfo))))
                for (uint64_t k = 0; k < n; k++) use[i + k] = (uint8_t)(info[k].attr & 1);
        }
        free(info);
    } else
#endif
        memset(use, 1, (size_t)pages);
    return use;
}

/* the 4-byte-aligned places in the heap's used pages that hold len bytes of pat (len <= a page), at
   most max of them; the number found */
static size_t heap_find(const void *pat, size_t len, uint64_t *out, size_t max) {
    const uint64_t pages = hos_heap_size / PAGE;
    uint8_t *use = pages_in_use(pages);
    if (!use || len < 4 || len > PAGE) { free(use); return 0; }
    uint32_t first;
    memcpy(&first, pat, 4);
    size_t n = 0;
    for (uint64_t i = 0; i < pages && n < max; i++) {
        if (!use[i]) continue;
        const uint8_t *p = (const uint8_t *)GP(HOS_HEAP_BASE + i * PAGE);
        for (size_t o = 0; o + len <= PAGE && n < max; o += 4)
            if (*(const uint32_t *)(p + o) == first && !memcmp(p + o, pat, len)) out[n++] = HOS_HEAP_BASE + i * PAGE + o;
    }
    free(use);
    return n;
}

static float getf(uint64_t a) { float f; memcpy(&f, GP(a), 4); return f; }
static void putf(uint64_t a, float f) { memcpy(GP(a), &f, 4); }

static int angles(uint64_t at) {
    const float p = getf(at + PITCH_AT), y = getf(at + YAW_AT);
    return isfinite(p) && isfinite(y) && fabsf(p) <= 90.0f && fabsf(y) <= 360.0f;
}

static int still_there(void) {
    return nctl && !memcmp(GP(ctl[0]), limits_sig, sizeof limits_sig) && angles(ctl[0]);
}

static DWORD WINAPI search(void *arg) {
    (void)arg;
    const int64_t t0 = hos_now_ns();
    uint64_t at[MAX_T];
    const size_t n = heap_find(limits_sig, sizeof limits_sig, at, MAX_T);
    nfound = 0;
    for (size_t i = 0; i < n; i++)
        if (angles(at[i])) found[nfound++] = at[i];
    hos_log("mouse native: %zu camera controller(s) found (%zu places with the limits) in %.0f ms%s", nfound, n,
            (hos_now_ns() - t0) / 1e6, nfound ? "" : ": the mouse turns the camera as the stick does until one is");
    answered = 1;
    searching = 0;
    return 0;
}

static void turn(uint64_t at, float dpitch, float dyaw) {
    float y = getf(at + YAW_AT) + dyaw, p = getf(at + PITCH_AT) + dpitch;
    const float lo = getf(at), hi = getf(at + 4);
    if (y > 180.0f) y -= 360.0f;
    if (y < -180.0f) y += 360.0f;
    if (p < lo) p = lo;
    if (p > hi) p = hi;
    putf(at + YAW_AT, y), putf(at + PITCH_AT, p);
}

/* With each read of the controller (nn_hid.c, the game's threads): 0 if the mouse is not captured, 1 if its motion
   turned the camera, -1 if no camera was found and the motion was given back (the stick's way takes it) */
int hos_mouse_native(void) {
    static int env_invert = -1;
    if (env_invert < 0) {
        env_invert = getenv("HOS_MOUSE_INVERT_Y") != NULL;
        hos_log("mouse native: on (the menu's Mouse camera, or HOS_MOUSE_NATIVE)");
    }
    int rx_option(int id, int otherwise);
    const int invert = env_invert || rx_option(8, 0);   /* (the menu's Invert mouse vertical) */
    int32_t dx, dy;
    if (!hos_mouse_take(&dx, &dy)) return 0;
    if (InterlockedExchange(&busy, 1)) {   /* (another thread is at it: the motion waits for the next read) */
        hos_mouse_give_back(dx, dy);
        return 1;
    }
    if (answered) {
        answered = 0;
        memcpy(ctl, found, sizeof ctl);
        nctl = nfound;
        wait_ns = nctl ? 3000000000LL : (wait_ns * 2 < 30000000000LL ? wait_ns * 2 : 30000000000LL);
    }
    const int64_t now = hos_now_ns();
    int ok = still_there();
    if (!ok && (dx || dy) && now >= next_search && !InterlockedExchange(&searching, 1)) {
        next_search = now + wait_ns;
        if (!CreateThread(NULL, 0, search, NULL, 0, NULL)) searching = 0;
    }
    if (!ok) {
        busy = 0;
        if (dx || dy) hos_mouse_give_back(dx, dy), given_back++;
        return -1;
    }
    if (dx || dy) {
        const double k = 0.1 * hos_mouse_sens;
        const float dyaw = (float)(-dx * k), dpitch = (float)((invert ? -dy : dy) * k);
        for (size_t i = 0; i < nctl; i++)
            if (!memcmp(GP(ctl[i]), limits_sig, sizeof limits_sig) && angles(ctl[i])) turn(ctl[i], dpitch, dyaw);
        if ((++applied & 2047) == 1)
            hos_log("mouse native: %lld moves applied (%lld given to the stick while no camera was found); pitch %.2f yaw %.2f",
                    applied, given_back, getf(ctl[0] + PITCH_AT), getf(ctl[0] + YAW_AT));
    }
    busy = 0;
    return 1;
}
