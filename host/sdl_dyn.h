/* (linux port) The SDL2 entry points the Linux window (display_sdl.c) and sound output (nn_audio.c) use, loaded at
   run time from the system's libSDL2 (as pad_sdl.c loads SDL2.dll): the build needs no SDL headers or libraries, and
   a machine without SDL2 still starts (headless, silent).

   The few SDL types used are restated here with SDL2's ABI (stable across 2.x): opaque window pointers, the audio
   spec, the event union read by offset. */
#ifndef SDL_DYN_H
#define SDL_DYN_H
#include <stdint.h>

enum {
    SDLD_INIT_AUDIO = 0x10, SDLD_INIT_VIDEO = 0x20,
    SDLD_WINDOW_FULLSCREEN_DESKTOP = 0x1001, SDLD_WINDOW_SHOWN = 0x4, SDLD_WINDOW_RESIZABLE = 0x20,
    SDLD_WINDOW_INPUT_FOCUS = 0x200, SDLD_WINDOW_ALLOW_HIGHDPI = 0x2000, SDLD_WINDOW_VULKAN = 0x10000000,
    SDLD_WINDOWPOS_UNDEFINED = 0x1FFF0000,
    SDLD_AUDIO_S16LSB = 0x8010,
    /* events */
    SDLD_QUIT = 0x100, SDLD_WINDOWEVENT = 0x200, SDLD_KEYDOWN = 0x300, SDLD_KEYUP = 0x301, SDLD_MOUSEMOTION = 0x400,
    SDLD_MOUSEBUTTONDOWN = 0x401,
    SDLD_WINDOWEVENT_SIZE_CHANGED = 6, SDLD_WINDOWEVENT_FOCUS_GAINED = 12, SDLD_WINDOWEVENT_FOCUS_LOST = 13,
    SDLD_WINDOWEVENT_CLOSE = 14,
    SDLD_SCANCODE_ESCAPE = 41, SDLD_SCANCODE_F11 = 68,
};

typedef struct {   /* SDL_AudioSpec */
    int freq;
    uint16_t format;
    uint8_t channels, silence;
    uint16_t samples, padding;
    uint32_t size;
    void (*callback)(void *userdata, uint8_t *stream, int len);
    void *userdata;
} SdlAudioSpec;

/* SDL_Event: 56 bytes; the fields used, by offset */
typedef union { uint32_t type; uint8_t bytes[64]; uint64_t align; } SdlEvent;
#define SDLD_EV_U8(e, off) ((e)->bytes[off])
#define SDLD_EV_I32(e, off) (*(const int32_t *)((e)->bytes + (off)))
/* window event: event at 12, data1 at 16, data2 at 20; key event: repeat at 13, scancode at 16; motion: xrel at 28,
   yrel at 32; button: button at 16 */

typedef struct {
    int (*Init)(uint32_t);
    int (*InitSubSystem)(uint32_t);
    const char *(*GetError)(void);
    int (*SetHint)(const char *, const char *);
    /* video */
    void *(*CreateWindow)(const char *, int, int, int, int, uint32_t);
    void (*SetWindowTitle)(void *, const char *);
    void (*GetWindowSize)(void *, int *, int *);
    uint32_t (*GetWindowFlags)(void *);
    int (*SetWindowFullscreen)(void *, uint32_t);
    int (*WaitEventTimeout)(SdlEvent *, int);
    int (*SetRelativeMouseMode)(int);
    const uint8_t *(*GetKeyboardState)(int *);
    uint32_t (*GetMouseState)(int *, int *);
    int (*GetDesktopDisplayMode)(int, void *);
    int (*Vulkan_GetInstanceExtensions)(void *, unsigned *, const char **);
    int (*Vulkan_CreateSurface)(void *, void *, uint64_t *);
    void (*Vulkan_GetDrawableSize)(void *, int *, int *);
    /* audio */
    uint32_t (*OpenAudioDevice)(const char *, int, const SdlAudioSpec *, SdlAudioSpec *, int);
    void (*PauseAudioDevice)(uint32_t, int);
    int (*QueueAudio)(uint32_t, const void *, uint32_t);
    uint32_t (*GetQueuedAudioSize)(uint32_t);
} SdlDyn;

/* SDL2, loaded once; NULL when the system has none (logged once) */
const SdlDyn *hos_sdl(void);
#endif
