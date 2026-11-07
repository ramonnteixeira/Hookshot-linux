/* (linux port) The game's window on Linux, through SDL2 (X11 or Wayland): what display.c does on Windows.
   The renderer (gpu/vk) gets the SDL window as its "hwnd" and makes its Vulkan surface through hos_vk_* below.

   HOS_HEADLESS=1: no window. HOS_WINDOW_SIZE=<w>x<h>: the window's size (default 1280x720). HOS_FULLSCREEN=1 or F11:
   fullscreen. A click captures the mouse (relative mode: hidden, its motion read by nn_hid.c); Esc or leaving the
   window releases it. Closing the window ends the run.

   It also stands in for the few window calls the rest of the host and the renderer make (GetAsyncKeyState,
   GetForegroundWindow, GetClientRect, PostMessageA), answered from SDL's state. */
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32_compat.h"
#include "sdl_dyn.h"
#include "../gpu/render/rx.h"

volatile LONG64 hos_present_count;   /* (nvn_native.c: the game's presents) */
static void *window;
static int renderer;
static const SdlDyn *sdl;

static volatile LONG mouse_dx, mouse_dy, mouse_captured, focused;
static volatile LONG want_fullscreen = -1;   /* (the Insert menu's row, from another thread) */
static int fullscreen;

int hos_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = InterlockedExchange(&mouse_dx, 0);
    *dy = InterlockedExchange(&mouse_dy, 0);
    return mouse_captured;
}
int hos_window_focused(void) { return window && focused; }

static void mouse_capture(int on) {
    if (on == (int)mouse_captured) return;
    mouse_captured = on;
    InterlockedExchange(&mouse_dx, 0);
    InterlockedExchange(&mouse_dy, 0);
    sdl->SetRelativeMouseMode(on);
}

static void set_fullscreen(int on) {
    if (on == fullscreen) return;
    if (sdl->SetWindowFullscreen(window, on ? SDLD_WINDOW_FULLSCREEN_DESKTOP : 0)) {
        hos_log("display: fullscreen failed: %s", sdl->GetError());
        return;
    }
    fullscreen = on;
    hos_log("display: %s", on ? "fullscreen (F11 for a window)" : "windowed");
}

static void resized(void) {
    int w = 0, h = 0;
    sdl->Vulkan_GetDrawableSize(window, &w, &h);
    if (renderer && w > 0 && h > 0) rx_resize(w, h);
}

static HANDLE renderer_up;

static DWORD WINAPI display_main(void *p) {
    (void)p;
    extern float hos_aspect_ratio;
    int win_w = hos_aspect_ratio > 1.8f ? (int)(720 * hos_aspect_ratio + 0.5f) : 1280, win_h = 720;
    if (getenv("HOS_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (sscanf(getenv("HOS_WINDOW_SIZE"), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) win_w = w, win_h = h;
    }
    window = sdl->CreateWindow(HOS_GAME_TITLE, SDLD_WINDOWPOS_UNDEFINED, SDLD_WINDOWPOS_UNDEFINED, win_w, win_h,
                               SDLD_WINDOW_VULKAN | SDLD_WINDOW_RESIZABLE | SDLD_WINDOW_ALLOW_HIGHDPI | SDLD_WINDOW_SHOWN);
    if (!window) {
        hos_log("display: no window (%s): running headless", sdl->GetError());
        rx_start(NULL);
        SetEvent(renderer_up);
        return 0;
    }
    focused = (sdl->GetWindowFlags(window) & SDLD_WINDOW_INPUT_FOCUS) != 0;
    if (getenv("HOS_FULLSCREEN") && atoi(getenv("HOS_FULLSCREEN"))) set_fullscreen(1);
    renderer = rx_start(window);
    if (!renderer) hos_log("display: no renderer started: the window stays black");
    resized();
    SetEvent(renderer_up);
    for (;;) {
        SdlEvent e;
        const int got = sdl->WaitEventTimeout(&e, 50);
        const LONG fs = InterlockedExchange(&want_fullscreen, -1);
        if (fs >= 0) set_fullscreen((int)fs);
        if (!got) continue;
        switch (e.type) {
        case SDLD_QUIT:
            hos_log("display: window closed, exiting");
            hos_log_flush();
            ExitProcess(0);
        case SDLD_WINDOWEVENT:
            switch (SDLD_EV_U8(&e, 12)) {
            case SDLD_WINDOWEVENT_SIZE_CHANGED: resized(); break;
            case SDLD_WINDOWEVENT_FOCUS_GAINED: focused = 1; break;
            case SDLD_WINDOWEVENT_FOCUS_LOST: focused = 0; mouse_capture(0); break;
            case SDLD_WINDOWEVENT_CLOSE:
                hos_log("display: window closed, exiting");
                hos_log_flush();
                ExitProcess(0);
            }
            break;
        case SDLD_KEYDOWN: {
            const int sc = SDLD_EV_I32(&e, 16), repeat = SDLD_EV_U8(&e, 13);
            if (sc == SDLD_SCANCODE_ESCAPE) mouse_capture(0);
            if (sc == SDLD_SCANCODE_F11 && !repeat) set_fullscreen(!fullscreen);
            break;
        }
        case SDLD_MOUSEBUTTONDOWN:
            if (renderer && !mouse_captured && SDLD_EV_U8(&e, 16) == 1) mouse_capture(1);
            break;
        case SDLD_MOUSEMOTION:
            if (mouse_captured) {
                InterlockedExchangeAdd(&mouse_dx, SDLD_EV_I32(&e, 28));
                InterlockedExchangeAdd(&mouse_dy, SDLD_EV_I32(&e, 32));
            }
            break;
        }
    }
}

void hos_renderer_wait(void) {
    { const char *e = getenv("HOS_NO_RENDERER_WAIT"); if (e && atoi(e) != 0) return; }
    if (renderer_up) WaitForSingleObject(renderer_up, INFINITE);
}

void hos_display_start(void) {
    sdl = getenv("HOS_HEADLESS") ? NULL : hos_sdl();
    if (!sdl || sdl->InitSubSystem(SDLD_INIT_VIDEO)) {
        if (sdl) hos_log("display: SDL video did not start (%s): running headless", sdl->GetError());
        sdl = NULL;
        rx_start(NULL);   /* no window: the renderer still draws, offscreen */
        return;
    }
    struct { uint32_t format; int w, h, refresh; void *driver; } mode;
    if (!sdl->GetDesktopDisplayMode(0, &mode)) w32_screen_w = mode.w, w32_screen_h = mode.h;
    renderer_up = CreateEventA(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, display_main, NULL, 0, NULL);
}

/* ---- for the renderer (gpu/vk/vk_main.cpp, through -rdynamic): the window's Vulkan surface */
unsigned hos_vk_instance_extensions(const char **names, unsigned cap) {
    unsigned n = 0;
    if (!sdl || !window || !sdl->Vulkan_GetInstanceExtensions(window, &n, NULL)) return 0;
    if (!names) return n;
    if (n > cap) n = cap;
    return sdl->Vulkan_GetInstanceExtensions(window, &n, names) ? n : 0;
}
int hos_vk_create_surface(void *hwnd, void *instance, uint64_t *surface) {
    if (!sdl || !hwnd) return 0;
    if (!sdl->Vulkan_CreateSurface(hwnd, instance, surface)) {
        hos_log("display: no Vulkan surface: %s", sdl->GetError());
        return 0;
    }
    return 1;
}

/* ---- the window calls the host and the renderer make (win32_compat.h) */
HWND GetForegroundWindow(void) { return hos_window_focused() ? window : NULL; }
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid) {
    if (pid) *pid = w && w == window ? GetCurrentProcessId() : 0;
    return 0;
}
BOOL GetClientRect(HWND w, RECT *r) {
    int width = 0, height = 0;
    if (sdl && w) sdl->Vulkan_GetDrawableSize(w, &width, &height);
    r->left = r->top = 0, r->right = width, r->bottom = height;
    return w != NULL;
}
BOOL PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    (void)w, (void)lp;
    if (msg == WM_APP + 7) InterlockedExchange(&want_fullscreen, wp ? 1 : 0);   /* (the Insert menu's Fullscreen row) */
    return TRUE;
}
LONG GetWindowLongA(HWND w, int index) {
    return index == GWL_STYLE && w && w == window && !fullscreen ? (LONG)WS_CAPTION : 0;
}
/* Windows virtual-key codes as SDL scancodes */
static int scancode(int vk) {
    if (vk >= 'A' && vk <= 'Z') return 4 + (vk - 'A');
    if (vk >= '1' && vk <= '9') return 30 + (vk - '1');
    if (vk == '0') return 39;
    if (vk >= VK_F1 && vk <= VK_F1 + 11) return 58 + (vk - VK_F1);
    switch (vk) {
    case VK_RETURN: return 40;
    case VK_ESCAPE: return 41;
    case VK_BACK: return 42;
    case VK_TAB: return 43;
    case VK_SPACE: return 44;
    case VK_OEM_MINUS: return 45;
    case VK_OEM_PLUS: return 46;
    case VK_INSERT: return 73;
    case VK_HOME: return 74;
    case VK_PRIOR: return 75;
    case VK_DELETE: return 76;
    case VK_END: return 77;
    case VK_NEXT: return 78;
    case VK_RIGHT: return 79;
    case VK_LEFT: return 80;
    case VK_DOWN: return 81;
    case VK_UP: return 82;
    case VK_SUBTRACT: return 86;
    case VK_ADD: return 87;
    case VK_LCONTROL: return 224;
    case VK_LSHIFT: return 225;
    case VK_LMENU: return 226;
    case VK_RCONTROL: return 228;
    case VK_RSHIFT: return 229;
    case VK_RMENU: return 230;
    default: return -1;
    }
}
SHORT GetAsyncKeyState(int vk) {
    if (!sdl || !window) return 0;
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) {
        const uint32_t b = sdl->GetMouseState(NULL, NULL);
        const uint32_t bit = vk == VK_LBUTTON ? 1u : vk == VK_MBUTTON ? 2u : 4u;
        return (b & bit) ? (SHORT)0x8000 : 0;
    }
    int n = 0;
    const uint8_t *keys = sdl->GetKeyboardState(&n);
    if (vk == VK_CONTROL) return GetAsyncKeyState(VK_LCONTROL) | GetAsyncKeyState(VK_RCONTROL);
    if (vk == VK_SHIFT) return GetAsyncKeyState(VK_LSHIFT) | GetAsyncKeyState(VK_RSHIFT);
    if (vk == VK_MENU) return GetAsyncKeyState(VK_LMENU) | GetAsyncKeyState(VK_RMENU);
    const int sc = scancode(vk);
    return keys && sc >= 0 && sc < n && keys[sc] ? (SHORT)0x8000 : 0;
}
#endif
