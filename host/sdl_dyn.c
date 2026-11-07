/* (linux port) host/sdl_dyn.h: the system's SDL2, opened at run time. Linux only (Windows has its own window, sound
   and XInput, and pad_sdl.c's SDL2.dll for pads). */
#ifndef _WIN32
#include "hos.h"
#include "sdl_dyn.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>

static SdlDyn sdl;
static int sdl_ok;

static void load(void) {
    static const char *const names[] = {"libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so"};
    void *m = NULL;
    for (size_t i = 0; i < sizeof names / sizeof *names && !m; i++) m = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
    if (!m) {
        hos_log("sdl: no SDL2 on this system (libSDL2-2.0.so.0): no window, no sound and no controllers");
        return;
    }
#define GET(field, name) if (!(*(void **)&sdl.field = dlsym(m, name))) { hos_log("sdl: SDL2 lacks %s", name); return; }
    GET(Init, "SDL_Init");
    GET(InitSubSystem, "SDL_InitSubSystem");
    GET(GetError, "SDL_GetError");
    GET(SetHint, "SDL_SetHint");
    GET(CreateWindow, "SDL_CreateWindow");
    GET(SetWindowTitle, "SDL_SetWindowTitle");
    GET(GetWindowSize, "SDL_GetWindowSize");
    GET(GetWindowFlags, "SDL_GetWindowFlags");
    GET(SetWindowFullscreen, "SDL_SetWindowFullscreen");
    GET(WaitEventTimeout, "SDL_WaitEventTimeout");
    GET(SetRelativeMouseMode, "SDL_SetRelativeMouseMode");
    GET(GetKeyboardState, "SDL_GetKeyboardState");
    GET(GetMouseState, "SDL_GetMouseState");
    GET(GetDesktopDisplayMode, "SDL_GetDesktopDisplayMode");
    GET(Vulkan_GetInstanceExtensions, "SDL_Vulkan_GetInstanceExtensions");
    GET(Vulkan_CreateSurface, "SDL_Vulkan_CreateSurface");
    GET(Vulkan_GetDrawableSize, "SDL_Vulkan_GetDrawableSize");
    GET(OpenAudioDevice, "SDL_OpenAudioDevice");
    GET(PauseAudioDevice, "SDL_PauseAudioDevice");
    GET(QueueAudio, "SDL_QueueAudio");
    GET(GetQueuedAudioSize, "SDL_GetQueuedAudioSize");
#undef GET
    sdl_ok = 1;
}

const SdlDyn *hos_sdl(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load);
    return sdl_ok ? &sdl : NULL;
}
#endif
