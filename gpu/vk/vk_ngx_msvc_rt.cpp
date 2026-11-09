// (hookshot) NVIDIA DLSS in the llvm-mingw build of hookshot_vk.dll: what NVIDIA's static NGX loader (nvsdk_ngx_s.lib, built
// with Microsoft's compiler against its static C++ runtime, /MT) asks of that runtime and the MinGW one does not have.
// The C parts it shares with the static FFmpeg libraries (/GS's cookie and handler, _fltused) are host/mingw_msvc_shims.c,
// linked into this DLL too; these are the C++ ones. Its C library calls go to the UCRT (api-ms-win-crt-*), and its C++ frame
// handler is the system's (__CxxFrameHandler3, which the UCRT exports). nx2/build.py compiles this file only for the llvm-mingw
// build with NVIDIA's SDK (HOOKSHOT_NGX_SDK); the MSVC build (HOOKSHOT_MSVC=1) has Microsoft's own runtime and no need of it.
#if defined(__MINGW32__) && defined(HOOKSHOT_NGX)
#include <windows.h>

#include <climits>
#include <cstdio>
#include <cstdlib>

extern "C" {

// (/GS frames with C++ exception handling: the cookie check (skipped, as __GSHandlerCheck's in host/mingw_msvc_shims.c), then
// the C++ frame handler, as Microsoft's runtime does)
EXCEPTION_DISPOSITION __CxxFrameHandler3(struct _EXCEPTION_RECORD* record, void* frame, struct _CONTEXT* context,
                                         void* dispatcher);
EXCEPTION_DISPOSITION __GSHandlerCheck_EH(struct _EXCEPTION_RECORD* record, void* frame, struct _CONTEXT* context,
                                          void* dispatcher) {
    return __CxxFrameHandler3(record, frame, context, dispatcher);
}

// Thread-safe function-local statics, Microsoft's way (its thread_safe_statics.cpp): each static's guard is 0 before, -1
// while one thread initialises it, then the epoch it was done in. A thread whose own epoch is older than a guard takes the
// lock and looks; the others skip it. One lock and one condition variable for all of them.
int _Init_global_epoch = INT_MIN;
thread_local int _Init_thread_epoch = INT_MIN;   // (read by the compiled code itself, through the TLS index)

static SRWLOCK init_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE init_done = CONDITION_VARIABLE_INIT;

void _Init_thread_header(int* once) {
    AcquireSRWLockExclusive(&init_lock);
    if (*once == 0) {
        *once = -1;   // (this thread initialises it)
    } else {
        while (*once == -1) {
            SleepConditionVariableSRW(&init_done, &init_lock, 100, 0);
            if (*once == 0) {   // (the other thread's initialiser threw: this one tries)
                *once = -1;
                ReleaseSRWLockExclusive(&init_lock);
                return;
            }
        }
        _Init_thread_epoch = _Init_global_epoch;
    }
    ReleaseSRWLockExclusive(&init_lock);
}

void _Init_thread_footer(int* once) {
    AcquireSRWLockExclusive(&init_lock);
    ++_Init_global_epoch;
    *once = _Init_global_epoch;
    _Init_thread_epoch = _Init_global_epoch;
    ReleaseSRWLockExclusive(&init_lock);
    WakeAllConditionVariable(&init_done);
}

void _Init_thread_abort(int* once) {   // (an initialiser that threw: the next thread tries again)
    AcquireSRWLockExclusive(&init_lock);
    *once = 0;
    ReleaseSRWLockExclusive(&init_lock);
    WakeAllConditionVariable(&init_done);
}

// Microsoft's operator new and delete (its names: ??2@YAPEAX_K@Z, ??3@YAXPEAX@Z): the C heap. Only NVIDIA's code calls them,
// and frees only what it allocated, so they need not be libc++'s. Out of memory ends the process: an exception of libc++'s
// would not be one Microsoft's handlers catch.
[[noreturn]] static void msvc_fatal(const char* what) {
    fprintf(stderr, "vk: NVIDIA DLSS: %s in NVIDIA's NGX code\n", what);
    fflush(stderr);
    abort();
}

void* msvc_operator_new(size_t size) __asm__("??2@YAPEAX_K@Z");
void* msvc_operator_new(size_t size) {
    if (void* p = malloc(size ? size : 1)) return p;
    msvc_fatal("out of memory");
}

void msvc_operator_delete(void* p) __asm__("??3@YAXPEAX@Z");
void msvc_operator_delete(void* p) { free(p); }

// Microsoft's C++ library's error reports (std::_Xbad_alloc, std::_Xlength_error), which throw in its runtime
[[noreturn]] void msvc_xbad_alloc() __asm__("?_Xbad_alloc@std@@YAXXZ");
void msvc_xbad_alloc() { msvc_fatal("out of memory (std::bad_alloc)"); }

[[noreturn]] void msvc_xlength_error(const char* what) __asm__("?_Xlength_error@std@@YAXPEBD@Z");
void msvc_xlength_error(const char* what) { msvc_fatal(what ? what : "std::length_error"); }

}  // extern "C"
#endif
