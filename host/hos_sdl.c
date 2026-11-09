/* SDL3 or SDL2 behind one set of calls (hos_sdl.h). Each call is SDL3's or SDL2's, the same work under other names;
   SDL3 returns C's bool where SDL2 returned 0 for success or an SDL_bool, and the events and a few structures are laid
   out differently (both read here at their offsets: SDL_Event is 128 bytes in SDL3, 56 in SDL2). */
#include "hos.h"
#include "hos_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int version;   /* 3, 2, or 0 none */
static struct {
    void *InitSubSystem, *SetHint, *GetError;
    /* window */
    void *CreateWindow, *SetWindowPosition, *SetWindowFullscreen, *SetRelativeMouseMode, *GetPrimaryDisplay,
        *GetDesktopDisplayMode, *Vulkan_CreateSurface, *PixelSize, *GetKeyboardState, *GetMouseState, *WaitEventTimeout,
        *PollEvent, *GetWindowSize, *LoadPNG, *SetWindowIcon, *DestroySurface;
    /* sound */
    void *OpenAudioDevice, *PauseAudioDevice, *OpenAudioDeviceStream, *ResumeAudioStreamDevice, *PutAudioStreamData;
    /* controllers */
    void *PadOpen, *PadClose, *PadConnected, *PadsUpdate, *PadButton, *PadAxis, *PadName, *PadRumble;
    void *GetGamepads, *GetGamepadID, *Free;   /* (SDL3) */
    void *NumJoysticks, *IsGameController, *DeviceInstanceID, *GetJoystick, *JoystickInstanceID;   /* (SDL2) */
} f;
#define CALL(type, fn) ((type)f.fn)

static HMODULE load_one(const char *dll) {   /* beside the executable, else the system's */
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    snprintf(path + n, sizeof path - n, "%s", dll);
    HMODULE m = LoadLibraryA(path);
    return m ? m : LoadLibraryA(dll);
}
static int get_all(HMODULE m, int three) {
#define GET(field, name) if (!(*(FARPROC *)&f.field = GetProcAddress(m, name))) { hos_log("sdl: SDL%d has no %s", three ? 3 : 2, name); return 0; }
#define GET2(field, n3, n2) GET(field, three ? n3 : n2)
    GET(InitSubSystem, "SDL_InitSubSystem");
    GET(SetHint, "SDL_SetHint");
    GET(GetError, "SDL_GetError");
    GET(CreateWindow, "SDL_CreateWindow");
    GET(SetWindowFullscreen, "SDL_SetWindowFullscreen");
    GET2(SetRelativeMouseMode, "SDL_SetWindowRelativeMouseMode", "SDL_SetRelativeMouseMode");
    GET(GetDesktopDisplayMode, "SDL_GetDesktopDisplayMode");
    GET(Vulkan_CreateSurface, "SDL_Vulkan_CreateSurface");
    GET2(PixelSize, "SDL_GetWindowSizeInPixels", "SDL_Vulkan_GetDrawableSize");
    GET(GetKeyboardState, "SDL_GetKeyboardState");
    GET(GetMouseState, "SDL_GetMouseState");
    GET(GetWindowSize, "SDL_GetWindowSize");
    GET(WaitEventTimeout, "SDL_WaitEventTimeout");
    GET(PollEvent, "SDL_PollEvent");
    GET2(PadOpen, "SDL_OpenGamepad", "SDL_GameControllerOpen");
    GET2(PadClose, "SDL_CloseGamepad", "SDL_GameControllerClose");
    GET2(PadConnected, "SDL_GamepadConnected", "SDL_GameControllerGetAttached");
    GET2(PadsUpdate, "SDL_UpdateGamepads", "SDL_GameControllerUpdate");
    GET2(PadButton, "SDL_GetGamepadButton", "SDL_GameControllerGetButton");
    GET2(PadAxis, "SDL_GetGamepadAxis", "SDL_GameControllerGetAxis");
    GET2(PadName, "SDL_GetGamepadName", "SDL_GameControllerName");
    GET2(PadRumble, "SDL_RumbleGamepad", "SDL_GameControllerRumble");
    if (three) {
        GET(SetWindowPosition, "SDL_SetWindowPosition");
        GET(GetPrimaryDisplay, "SDL_GetPrimaryDisplay");
        GET(OpenAudioDeviceStream, "SDL_OpenAudioDeviceStream");
        GET(ResumeAudioStreamDevice, "SDL_ResumeAudioStreamDevice");
        GET(PutAudioStreamData, "SDL_PutAudioStreamData");
        GET(GetGamepads, "SDL_GetGamepads");
        GET(GetGamepadID, "SDL_GetGamepadID");
        GET(Free, "SDL_free");
        *(FARPROC *)&f.LoadPNG = GetProcAddress(m, "SDL_LoadPNG");   /* (3.4 on: the icon, when there) */
        *(FARPROC *)&f.SetWindowIcon = GetProcAddress(m, "SDL_SetWindowIcon");
        *(FARPROC *)&f.DestroySurface = GetProcAddress(m, "SDL_DestroySurface");
    } else {
        GET(OpenAudioDevice, "SDL_OpenAudioDevice");
        GET(PauseAudioDevice, "SDL_PauseAudioDevice");
        GET(NumJoysticks, "SDL_NumJoysticks");
        GET(IsGameController, "SDL_IsGameController");
        GET(DeviceInstanceID, "SDL_JoystickGetDeviceInstanceID");
        GET(GetJoystick, "SDL_GameControllerGetJoystick");
        GET(JoystickInstanceID, "SDL_JoystickInstanceID");
    }
#undef GET2
#undef GET
    return 1;
}
int hos_sdl_load(void) {
    static volatile LONG state;   /* 0 not tried, 1 loading, 2 done */
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        const char *want = getenv("HOS_SDL_VERSION");
        HMODULE m = want && atoi(want) == 2 ? NULL : load_one("SDL3.dll");
        if (m && get_all(m, 1)) version = 3;
        else if ((m = load_one("SDL2.dll")) && get_all(m, 0)) version = 2;
        hos_log(version ? "sdl: SDL%d" : "sdl: no SDL3 or SDL2 found: no window, sound or SDL controllers", version);
        InterlockedExchange(&state, 2);
    }
    while (state != 2) Sleep(0);
    return version;
}

int hos_sdl_init(uint32_t subsystems) {
    if (!hos_sdl_load()) return 0;
    return version == 3 ? CALL(_Bool (*)(uint32_t), InitSubSystem)(subsystems)
                        : CALL(int (*)(uint32_t), InitSubSystem)(subsystems) == 0;
}
void hos_sdl_hint(const char *name, const char *value) {
    if (!hos_sdl_load()) return;
    if (version == 3) CALL(_Bool (*)(const char *, const char *), SetHint)(name, value);
    else CALL(int (*)(const char *, const char *), SetHint)(name, value);
}
void hos_sdl_identity(const char *name, const char *app_id) {
    if (!hos_sdl_load()) return;
    if (version == 3) {
        static void *set;
        if (!set) {
            HMODULE m = load_one("SDL3.dll");   /* (the one loaded: the same handle back) */
            set = m ? (void *)GetProcAddress(m, "SDL_SetAppMetadata") : NULL;
        }
        if (set) ((_Bool (*)(const char *, const char *, const char *))set)(name, NULL, app_id);
        return;
    }
    hos_sdl_hint("SDL_APP_NAME", name);
#ifndef _WIN32   /* (SDL2 reads the window's class from these, not from hints) */
    setenv("SDL_VIDEO_WAYLAND_WMCLASS", app_id, 0);
    setenv("SDL_VIDEO_X11_WMCLASS", app_id, 0);
#endif
}
const char *hos_sdl_error(void) { return version ? CALL(const char *(*)(void), GetError)() : "no SDL"; }

/* ---- the window ---- */
enum { WIN_SHOWN2 = 0x4, WIN_RESIZABLE = 0x20, WIN_HIGH_DENSITY = 0x2000, WIN_VULKAN = 0x10000000, CENTRED = 0x2FFF0000 };
void *hos_sdl_window(const char *title, int width, int height) {
    if (version == 2)
        return CALL(void *(*)(const char *, int, int, int, int, uint32_t), CreateWindow)(
            title, CENTRED, CENTRED, width, height, WIN_SHOWN2 | WIN_RESIZABLE | WIN_HIGH_DENSITY | WIN_VULKAN);
    void *w = CALL(void *(*)(const char *, int, int, uint64_t), CreateWindow)(title, width, height,
                                                                              WIN_RESIZABLE | WIN_HIGH_DENSITY | WIN_VULKAN);
    if (w) CALL(_Bool (*)(void *, int, int), SetWindowPosition)(w, CENTRED, CENTRED);
    return w;
}
void hos_sdl_fullscreen(void *window, int on) {
    if (version == 3) CALL(_Bool (*)(void *, _Bool), SetWindowFullscreen)(window, on != 0);
    else CALL(int (*)(void *, uint32_t), SetWindowFullscreen)(window, on ? 0x1001 /* FULLSCREEN_DESKTOP */ : 0);
}
void hos_sdl_relative_mouse(void *window, int on) {
    if (version == 3) CALL(_Bool (*)(void *, _Bool), SetRelativeMouseMode)(window, on != 0);
    else CALL(int (*)(int), SetRelativeMouseMode)(on);
}
int hos_sdl_desktop_size(int *width, int *height) {
    if (version == 3) {   /* (SDL_DisplayMode: displayID, format, w and h at 8 and 12 in screen units, pixel_density at
                             16: in pixels, as SDL2 gave them) */
        const uint8_t *m = CALL(const uint8_t *(*)(uint32_t), GetDesktopDisplayMode)(CALL(uint32_t (*)(void), GetPrimaryDisplay)());
        if (!m) return 0;
        int w, h;
        float density;
        memcpy(&w, m + 8, 4), memcpy(&h, m + 12, 4), memcpy(&density, m + 16, 4);
        if (!(density > 0)) density = 1;
        *width = (int)(w * density + 0.5f), *height = (int)(h * density + 0.5f);
        return 1;
    }
    struct { uint32_t format; int w, h, refresh_rate; void *driverdata; } m;
    if (CALL(int (*)(int, void *), GetDesktopDisplayMode)(0, &m)) return 0;
    *width = m.w, *height = m.h;
    return 1;
}
int hos_sdl_vulkan_surface(void *window, void *instance, void *surface_out) {
    return version == 3 ? CALL(_Bool (*)(void *, void *, const void *, void *), Vulkan_CreateSurface)(window, instance, NULL, surface_out)
                        : CALL(int (*)(void *, void *, void *), Vulkan_CreateSurface)(window, instance, surface_out) != 0;
}
void hos_sdl_pixel_size(void *window, int *width, int *height) {
    if (version == 3) CALL(_Bool (*)(void *, int *, int *), PixelSize)(window, width, height);
    else CALL(void (*)(void *, int *, int *), PixelSize)(window, width, height);
}
const uint8_t *hos_sdl_keys(int *count) {   /* (SDL3's bool and SDL2's Uint8: a byte each) */
    return CALL(const uint8_t *(*)(int *), GetKeyboardState)(count);
}
void hos_sdl_icon(void *window, const char *png) {
    if (version != 3 || !f.LoadPNG || !f.SetWindowIcon || !f.DestroySurface) return;
    void *icon = CALL(void *(*)(const char *), LoadPNG)(png);
    if (!icon) return;
    CALL(_Bool (*)(void *, void *), SetWindowIcon)(window, icon);
    CALL(void (*)(void *), DestroySurface)(icon);
}
uint32_t hos_sdl_mouse_buttons(void) {
    return version == 3 ? CALL(uint32_t (*)(float *, float *), GetMouseState)(NULL, NULL)
                        : CALL(uint32_t (*)(int *, int *), GetMouseState)(NULL, NULL);
}
void hos_sdl_window_size(void *window, int *width, int *height) {
    if (version == 3) CALL(_Bool (*)(void *, int *, int *), GetWindowSize)(window, width, height);
    else CALL(void (*)(void *, int *, int *), GetWindowSize)(window, width, height);
}

/* ---- events ---- */
static void read_event(const uint8_t *raw, HosSdlEvent *e) {
    uint32_t type;
    memcpy(&type, raw, 4);
    memset(e, 0, sizeof *e);
    int32_t i[2];
    if (version == 3) {
        switch (type) {
        case 0x100: e->type = HOS_SDL_EV_QUIT; break;
        case 0x207: e->type = HOS_SDL_EV_RESIZED; break;   /* (PIXEL_SIZE_CHANGED) */
        case 0x20d: e->type = HOS_SDL_EV_MOUSE_LEAVE; break;
        case 0x20e: e->type = HOS_SDL_EV_FOCUS_GAINED; break;
        case 0x20f: e->type = HOS_SDL_EV_FOCUS_LOST; break;
        case 0x210: e->type = HOS_SDL_EV_CLOSE; break;
        case 0x300:   /* (SDL_KeyboardEvent: scancode at 24, mod at 32, repeat at 37) */
            e->type = HOS_SDL_EV_KEY_DOWN;
            memcpy(i, raw + 24, 4), e->scancode = i[0];
            memcpy(&e->mod, raw + 32, 2), e->repeat = raw[37];
            break;
        case 0x20c: e->type = HOS_SDL_EV_MOUSE_ENTER; break;
        case 0x400:   /* (SDL_MouseMotionEvent: x and y at 28 and 32, xrel and yrel at 36 and 40, floats) */
            e->type = HOS_SDL_EV_MOUSE_MOTION;
            memcpy(&e->x, raw + 28, 4), memcpy(&e->y, raw + 32, 4);
            memcpy(&e->dx, raw + 36, 4), memcpy(&e->dy, raw + 40, 4);
            break;
        case 0x401:
        case 0x402:   /* (SDL_MouseButtonEvent: button at 24, x and y at 28 and 32, floats) */
            e->type = type == 0x401 ? HOS_SDL_EV_MOUSE_DOWN : HOS_SDL_EV_MOUSE_UP;
            e->button = raw[24];
            memcpy(&e->x, raw + 28, 4), memcpy(&e->y, raw + 32, 4);
            break;
        case 0x403: {   /* (SDL_MouseWheelEvent: y at 28, a float; direction at 32, 1 flipped) */
            uint32_t dir;
            e->type = HOS_SDL_EV_MOUSE_WHEEL;
            memcpy(&e->wheel, raw + 28, 4), memcpy(&dir, raw + 32, 4);
            if (dir == 1) e->wheel = -e->wheel;
            break;
        }
        }
        return;
    }
    switch (type) {
    case 0x100: e->type = HOS_SDL_EV_QUIT; break;
    case 0x200:   /* (SDL_WindowEvent: what at 12) */
        e->type = raw[12] == 6 ? HOS_SDL_EV_RESIZED : raw[12] == 10 ? HOS_SDL_EV_MOUSE_ENTER : raw[12] == 11 ? HOS_SDL_EV_MOUSE_LEAVE
                : raw[12] == 12 ? HOS_SDL_EV_FOCUS_GAINED : raw[12] == 13 ? HOS_SDL_EV_FOCUS_LOST
                : raw[12] == 14 ? HOS_SDL_EV_CLOSE : HOS_SDL_EV_OTHER;
        break;
    case 0x300:   /* (SDL_KeyboardEvent: repeat at 13, scancode at 16, mod at 24) */
        e->type = HOS_SDL_EV_KEY_DOWN;
        memcpy(i, raw + 16, 4), e->scancode = i[0];
        memcpy(&e->mod, raw + 24, 2), e->repeat = raw[13];
        break;
    case 0x400:   /* (SDL_MouseMotionEvent: x and y at 20 and 24, xrel and yrel at 28 and 32, whole pixels) */
        e->type = HOS_SDL_EV_MOUSE_MOTION;
        memcpy(i, raw + 20, 8), e->x = (float)i[0], e->y = (float)i[1];
        memcpy(i, raw + 28, 8), e->dx = (float)i[0], e->dy = (float)i[1];
        break;
    case 0x401:
    case 0x402:   /* (SDL_MouseButtonEvent: button at 16, x and y at 20 and 24, whole pixels) */
        e->type = type == 0x401 ? HOS_SDL_EV_MOUSE_DOWN : HOS_SDL_EV_MOUSE_UP;
        e->button = raw[16];
        memcpy(i, raw + 20, 8), e->x = (float)i[0], e->y = (float)i[1];
        break;
    case 0x403: {   /* (SDL_MouseWheelEvent: y at 20, whole notches; direction at 24, 1 flipped) */
        uint32_t dir;
        e->type = HOS_SDL_EV_MOUSE_WHEEL;
        memcpy(i, raw + 20, 4), memcpy(&dir, raw + 24, 4);
        e->wheel = (float)(dir == 1 ? -i[0] : i[0]);
        break;
    }
    }
}
int hos_sdl_wait_event(HosSdlEvent *e, int timeout_ms) {
    _Alignas(8) uint8_t raw[128];
    const int got = version == 3 ? CALL(_Bool (*)(void *, int32_t), WaitEventTimeout)(raw, timeout_ms)
                                 : CALL(int (*)(void *, int), WaitEventTimeout)(raw, timeout_ms) != 0;
    if (got) read_event(raw, e);
    return got;
}
int hos_sdl_poll_event(HosSdlEvent *e) {
    _Alignas(8) uint8_t raw[128];
    const int got = version == 3 ? CALL(_Bool (*)(void *), PollEvent)(raw) : CALL(int (*)(void *), PollEvent)(raw) != 0;
    if (got) read_event(raw, e);
    return got;
}

/* ---- sound ---- */
typedef struct {
    void (*pull)(void *user, uint8_t *out, int len);
    void *user;
} Pull;
static void pull3(void *p, void *stream, int want, int total) {   /* (SDL3: a stream asks for want more bytes) */
    (void)total;
    const Pull *s = (const Pull *)p;
    uint8_t buf[4096];
    for (int n; want > 0; want -= n) {
        n = want < (int)sizeof buf ? want : (int)sizeof buf;
        s->pull(s->user, buf, n);
        CALL(_Bool (*)(void *, const void *, int), PutAudioStreamData)(stream, buf, n);
    }
}
static void pull2(void *p, uint8_t *out, int len) {   /* (SDL2: a device callback fills its buffer) */
    const Pull *s = (const Pull *)p;
    s->pull(s->user, out, len);
}
int hos_sdl_audio_open(int freq, int channels, int s16, void (*pull)(void *user, uint8_t *out, int len), void *user) {
    if (!hos_sdl_load()) return 0;
    const uint16_t format = s16 ? 0x8010 /* S16LE */ : 0x8120 /* F32LE */;   /* (both's numbers) */
    if (version == 3) hos_sdl_hint("SDL_AUDIO_DEVICE_SAMPLE_FRAMES", "512");   /* (SDL2's device buffer below) */
    if (!hos_sdl_init(HOS_SDL_AUDIO)) return 0;
    Pull *p = (Pull *)calloc(1, sizeof *p);
    p->pull = pull, p->user = user;
    if (version == 3) {
        const struct { uint32_t format; int channels, freq; } spec = {format, channels, freq};
        void *stream = CALL(void *(*)(uint32_t, const void *, void (*)(void *, void *, int, int), void *), OpenAudioDeviceStream)(
            0xFFFFFFFFu /* the default output */, &spec, pull3, p);
        if (stream && CALL(_Bool (*)(void *), ResumeAudioStreamDevice)(stream)) return 1;
        free(p);
        return 0;
    }
    struct {
        int freq;
        uint16_t format;
        uint8_t channels, silence;
        uint16_t samples, padding;
        uint32_t size;
        void (*callback)(void *, uint8_t *, int);
        void *userdata;
    } want, have;
    memset(&want, 0, sizeof want);
    want.freq = freq, want.format = format, want.channels = (uint8_t)channels, want.samples = 512;
    want.callback = pull2, want.userdata = p;
    /* (no changes allowed: SDL converts to what the device wants) */
    const uint32_t dev = CALL(uint32_t (*)(const char *, int, const void *, void *, int), OpenAudioDevice)(NULL, 0, &want, &have, 0);
    if (!dev) {
        free(p);
        return 0;
    }
    CALL(void (*)(uint32_t, int), PauseAudioDevice)(dev, 0);
    return 1;
}

/* ---- controllers ---- */
int hos_sdl_pads_list(int32_t *ids, int *opens, int max) {   /* (SDL3 opens by id; SDL2 by device index) */
    int n = 0;
    if (version == 3) {
        int count = 0;
        uint32_t *got = CALL(uint32_t *(*)(int *), GetGamepads)(&count);
        for (int i = 0; got && i < count && n < max; i++) ids[n] = (int32_t)got[i], opens[n] = (int)got[i], n++;
        if (got) CALL(void (*)(void *), Free)(got);
        return n;
    }
    const int nj = CALL(int (*)(void), NumJoysticks)();
    for (int j = 0; j < nj && n < max; j++)
        if (CALL(int (*)(int), IsGameController)(j)) ids[n] = CALL(int32_t (*)(int), DeviceInstanceID)(j), opens[n] = j, n++;
    return n;
}
void *hos_sdl_pad_open(int what) {
    return version == 3 ? CALL(void *(*)(uint32_t), PadOpen)((uint32_t)what) : CALL(void *(*)(int), PadOpen)(what);
}
void hos_sdl_pad_close(void *pad) { CALL(void (*)(void *), PadClose)(pad); }
int hos_sdl_pad_connected(void *pad) {
    return version == 3 ? CALL(_Bool (*)(void *), PadConnected)(pad) : CALL(int (*)(void *), PadConnected)(pad) != 0;
}
void hos_sdl_pads_update(void) { CALL(void (*)(void), PadsUpdate)(); }
int hos_sdl_pad_button(void *pad, int button) {
    return version == 3 ? CALL(_Bool (*)(void *, int), PadButton)(pad, button)
                        : CALL(uint8_t (*)(void *, int), PadButton)(pad, button) != 0;
}
int16_t hos_sdl_pad_axis(void *pad, int axis) { return CALL(int16_t (*)(void *, int), PadAxis)(pad, axis); }
const char *hos_sdl_pad_name(void *pad) {
    const char *n = CALL(const char *(*)(void *), PadName)(pad);
    return n ? n : "?";
}
int32_t hos_sdl_pad_id(void *pad) {
    return version == 3 ? (int32_t)CALL(uint32_t (*)(void *), GetGamepadID)(pad)
                        : CALL(int32_t (*)(void *), JoystickInstanceID)(CALL(void *(*)(void *), GetJoystick)(pad));
}
void hos_sdl_pad_rumble(void *pad, uint16_t low, uint16_t high, uint32_t ms) {
    if (version == 3) CALL(_Bool (*)(void *, uint16_t, uint16_t, uint32_t), PadRumble)(pad, low, high, ms);
    else CALL(int (*)(void *, uint16_t, uint16_t, uint32_t), PadRumble)(pad, low, high, ms);
}
#undef CALL
