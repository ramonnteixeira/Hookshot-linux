/* (linux port) The subset of the Win32 API the host layer uses, on POSIX: the Windows build includes <windows.h>
   unchanged, every other build gets these. Implemented in plat_posix.c.

   What maps directly: types, SRW locks (pthread rwlocks), condition variables (a futex sequence), critical sections
   (recursive mutexes), Interlocked* (__atomic builtins), the performance counter (CLOCK_MONOTONIC in ns), threads,
   events and waitable timers (handle objects over pthreads), files (fds; backslashes in paths become slashes),
   FindFirstFile (opendir + fnmatch), modules (dlopen/dladdr; "x.dll" opens "x.so"), virtual memory (mmap, mprotect,
   madvise; VirtualQuery reads /proc/self/maps), fibers (ucontext), vectored exception handlers (SIGSEGV/SIGBUS/SIGILL/
   SIGFPE/SIGTRAP turned into EXCEPTION_POINTERS with an x86-64 CONTEXT).

   What does not: windows, messages, the registry, COM, Media Foundation, waveOut, XInput, toolhelp snapshots and
   suspending other threads. The files that use those have their own Linux code (display_sdl.c, ...) or #ifdef the
   Windows-only parts out. */
#ifndef WIN32_COMPAT_H
#define WIN32_COMPAT_H
/* the host's path separator, for paths that reach C's own file functions (fopen) as well as the ones below */
#ifdef _WIN32
#define HOS_SEP '\\'
#define HOS_SEP_S "\\"
#else
#define HOS_SEP '/'
#define HOS_SEP_S "/"
#endif
#define HOS_IS_SEP(ch) ((ch) == '\\' || (ch) == '/')

#if defined(_WIN32) || defined(__EMSCRIPTEN__)   /* (the web build has a windows.h of its own) */
#include <windows.h>
#else

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <pthread.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- types (LONG is 32 bits, as on Windows: `long` is 64 on Linux) */
typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD, UINT32, ULONG;
typedef int32_t LONG, INT32;
typedef unsigned int UINT;
typedef int64_t LONG64, LONGLONG, INT64;
typedef uint64_t DWORD64, ULONG64, ULONGLONG, UINT64, DWORD_PTR, ULONG_PTR, SIZE_T;
typedef int64_t LONG_PTR, INT_PTR;
typedef wchar_t WCHAR;
typedef const char *LPCSTR;
typedef char *LPSTR, CHAR;
typedef const wchar_t *LPCWSTR, *PCWSTR;
typedef wchar_t *LPWSTR, *PWSTR;
typedef void *LPVOID, *PVOID, *HANDLE, *HMODULE, *HINSTANCE, *HWND;
typedef const void *LPCVOID;
typedef int32_t HRESULT;
typedef DWORD *LPDWORD;
typedef void (*FARPROC)(void);
typedef union { struct { DWORD LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;
typedef union { struct { DWORD LowPart; DWORD HighPart; }; ULONGLONG QuadPart; } ULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;

#define TRUE 1
#define FALSE 0
#define WINAPI
#define CALLBACK
#define APIENTRY
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_FILE_EXISTS 80u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_TIMEOUT 1460u
#define ERROR_NOT_ALL_ASSIGNED 1300u
/* the MSVC C library's names */
#define _strdup strdup
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define __int64 long long
#define _atoi64 atoll
#define _ftelli64 ftello
#define _fseeki64 fseeko
#define _mkgmtime timegm
#define _putenv putenv
static inline int localtime_s(struct tm *out, const time_t *t) { return localtime_r(t, out) ? 0 : 22; }   /* (EINVAL) */
static inline int gmtime_s(struct tm *out, const time_t *t) { return gmtime_r(t, out) ? 0 : 22; }

DWORD GetLastError(void);
void SetLastError(DWORD e);

/* ---- atomics: the result conventions of the Win32 functions (Increment/Decrement/Add: the new value; Exchange,
   ExchangeAdd, CompareExchange: the old one), for any operand width */
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedIncrement64 InterlockedIncrement
#define InterlockedDecrement64 InterlockedDecrement
#define InterlockedAdd(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAdd64 InterlockedAdd
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64 InterlockedExchangeAdd
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange64 InterlockedExchange
#define InterlockedExchangePointer InterlockedExchange
#define InterlockedOr(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAnd(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#ifdef __cplusplus
#define W32_AUTO auto
#else
#define W32_AUTO __auto_type
#endif
#define InterlockedCompareExchange(p, x, cmp) __extension__({ W32_AUTO w32_c_ = (__typeof__(*(p)))(cmp); \
    __atomic_compare_exchange_n((p), &w32_c_, (x), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); w32_c_; })
#define InterlockedCompareExchange64 InterlockedCompareExchange
#define InterlockedCompareExchangePointer InterlockedCompareExchange
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _ReadWriteBarrier() __asm__ __volatile__("" ::: "memory")
#define YieldProcessor() _mm_pause()

/* ---- locks */
typedef struct { pthread_rwlock_t rw; } SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT {PTHREAD_RWLOCK_INITIALIZER}
static inline void InitializeSRWLock(PSRWLOCK l) { pthread_rwlock_init(&l->rw, NULL); }
static inline void AcquireSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_wrlock(&l->rw); }
static inline void ReleaseSRWLockExclusive(PSRWLOCK l) { pthread_rwlock_unlock(&l->rw); }
static inline void AcquireSRWLockShared(PSRWLOCK l) { pthread_rwlock_rdlock(&l->rw); }
static inline void ReleaseSRWLockShared(PSRWLOCK l) { pthread_rwlock_unlock(&l->rw); }
static inline BOOL TryAcquireSRWLockExclusive(PSRWLOCK l) { return pthread_rwlock_trywrlock(&l->rw) == 0; }
static inline BOOL TryAcquireSRWLockShared(PSRWLOCK l) { return pthread_rwlock_tryrdlock(&l->rw) == 0; }

typedef struct { uint32_t seq; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {0}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 1u
static inline void InitializeConditionVariable(PCONDITION_VARIABLE cv) { cv->seq = 0; }
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags);
void WakeConditionVariable(PCONDITION_VARIABLE cv);
void WakeAllConditionVariable(PCONDITION_VARIABLE cv);

typedef struct { pthread_mutex_t m; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
static inline void EnterCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_lock(&cs->m); }
static inline void LeaveCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_unlock(&cs->m); }
static inline BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) { return pthread_mutex_trylock(&cs->m) == 0; }
static inline void DeleteCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_destroy(&cs->m); }

/* ---- time */
BOOL QueryPerformanceCounter(LARGE_INTEGER *t);     /* nanoseconds */
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f);   /* 1e9 */
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
void Sleep(DWORD ms);
BOOL SwitchToThread(void);

/* ---- threads, events, timers (HANDLEs) */
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000u
#define CREATE_SUSPENDED 0x00000004u
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define DUPLICATE_SAME_ACCESS 2u
#define THREAD_ALL_ACCESS 0x1FFFFFu
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800u
#define TIMER_ALL_ACCESS 0x1F0003u
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 2u
HANDLE CreateThread(void *attr, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *id);
HANDLE GetCurrentThread(void);    /* a pseudo handle, as on Windows */
HANDLE GetCurrentProcess(void);
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
DWORD GetThreadId(HANDLE h);
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit, DWORD options);
BOOL CloseHandle(HANDLE h);
DWORD ResumeThread(HANDLE h);   /* (a thread made with CREATE_SUSPENDED) */
__attribute__((noreturn)) void ExitThread(DWORD code);
__attribute__((noreturn)) void ExitProcess(UINT code);
BOOL SetThreadPriority(HANDLE h, int prio);
int GetThreadPriority(HANDLE h);
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys);
HRESULT SetThreadDescription(HANDLE h, PCWSTR name);
HRESULT GetThreadDescription(HANDLE h, PWSTR *name);   /* (LocalFree the name) */
HANDLE CreateEventA(void *attr, BOOL manual, BOOL initial, LPCSTR name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
HANDLE CreateWaitableTimerExW(void *attr, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, void *wake, ULONG delay);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
void *LocalFree(void *p);

typedef struct { ULONG_PTR ProcessorMask; int Relationship; BYTE pad[24]; } SYSTEM_LOGICAL_PROCESSOR_INFORMATION;
#define RelationProcessorCore 0
BOOL GetLogicalProcessorInformation(SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info, DWORD *len);   /* (FALSE: unknown) */

/* ---- fibers (ucontext) */
#define FIBER_FLAG_FLOAT_SWITCH 1u
typedef void (WINAPI *LPFIBER_START_ROUTINE)(LPVOID);
LPVOID ConvertThreadToFiberEx(LPVOID param, DWORD flags);
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID param);
void SwitchToFiber(LPVOID fiber);
void DeleteFiber(LPVOID fiber);

/* ---- text */
#define CP_UTF8 65001u
#define CP_ACP 0u
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR w, int cap);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int n, LPSTR s, int cap, LPCSTR def, BOOL *used);

/* ---- files */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_READONLY 0x01u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_RANDOM_ACCESS 0x10000000u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
#define FILE_TYPE_UNKNOWN 0u
#define FILE_TYPE_DISK 1u
#define FILE_TYPE_CHAR 2u
#define FILE_TYPE_PIPE 3u
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define MOVEFILE_REPLACE_EXISTING 1u
typedef struct { ULONG_PTR Internal, InternalHigh; union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; };
                 HANDLE hEvent; } OVERLAPPED;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
                 DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; CHAR cFileName[MAX_PATH];
                 CHAR cAlternateFileName[14]; } WIN32_FIND_DATAA;
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, OVERLAPPED *ov);
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, OVERLAPPED *ov);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
DWORD GetFileSize(HANDLE h, DWORD *high);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *now, DWORD method);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
DWORD GetFileType(HANDLE h);
HANDLE GetStdHandle(DWORD which);
BOOL GetConsoleMode(HANDLE h, DWORD *mode);
DWORD GetFileAttributesA(LPCSTR path);
BOOL CreateDirectoryA(LPCSTR path, void *sa);
BOOL RemoveDirectoryA(LPCSTR path);
BOOL DeleteFileA(LPCSTR path);
BOOL MoveFileA(LPCSTR from, LPCSTR to);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL SetCurrentDirectoryA(LPCSTR path);
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
BOOL FindClose(HANDLE h);
/* a Windows path (backslashes) as a POSIX one, in place; returns it */
char *w32_path(char *path);

/* ---- modules */
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2u
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4u
HMODULE LoadLibraryA(LPCSTR name);
HMODULE GetModuleHandleA(LPCSTR name);   /* NULL: the program; a system DLL's name: NULL (not on Linux) */
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name_or_addr, HMODULE *out);
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD cap);
FARPROC GetProcAddress(HMODULE m, LPCSTR name);
BOOL FreeLibrary(HMODULE m);

/* ---- virtual memory */
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_DECOMMIT 0x00004000u
#define MEM_RELEASE 0x00008000u
#define MEM_FREE 0x00010000u
#define MEM_PRIVATE 0x00020000u
#define MEM_MAPPED 0x00040000u
#define MEM_RESET 0x00080000u
#define MEM_WRITE_WATCH 0x00200000u
#define MEM_LARGE_PAGES 0x20000000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_WRITECOPY 0x08u
#define PAGE_GUARD 0x100u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
typedef struct { PVOID BaseAddress, AllocationBase; DWORD AllocationProtect; SIZE_T RegionSize;
                 DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION;
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD *old);
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len);

/* ---- structured exceptions: vectored handlers over POSIX signals. The CONTEXT is x86-64's general registers only
   (what the handlers here read: Rip, Rsp, Rbp and the integer registers); writing it back resumes with it. */
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u
#define EXCEPTION_BREAKPOINT 0x80000003u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDu
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD { DWORD ExceptionCode, ExceptionFlags; struct _EXCEPTION_RECORD *ExceptionRecord;
                                   PVOID ExceptionAddress; DWORD NumberParameters;
                                   ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS]; } EXCEPTION_RECORD;
typedef struct { DWORD ContextFlags; DWORD EFlags;
                 DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15, Rip;
                 DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7; } CONTEXT, *PCONTEXT;
#define CONTEXT_CONTROL 1u
#define CONTEXT_INTEGER 2u
#define CONTEXT_DEBUG_REGISTERS 0x10u
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER)
typedef struct { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS;
typedef LONG (CALLBACK *PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);
typedef LONG (WINAPI *LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);
PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);
ULONG RemoveVectoredExceptionHandler(PVOID h);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f);

/* ---- misc */
#define MB_OK 0u
#define MB_ICONERROR 0x10u
#define MB_ICONWARNING 0x30u
#define MB_ICONINFORMATION 0x40u
int MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type);   /* (stderr, and a desktop notice if one can be shown) */
void OutputDebugStringA(LPCSTR s);
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
int GetSystemMetrics(int which);   /* the screen's size once the window system is up (display_sdl.c), 0 before */
extern int w32_screen_w, w32_screen_h;

/* ---- the window (display_sdl.c: the SDL window, its keyboard and mouse) */
typedef int16_t SHORT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
#define WM_APP 0x8000u
SHORT GetAsyncKeyState(int vk);
HWND GetForegroundWindow(void);
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid);
BOOL GetClientRect(HWND w, RECT *r);
BOOL PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp);   /* (WM_APP + 7 only: the menu's fullscreen request) */
#define GWL_STYLE (-16)
#define WS_CAPTION 0x00C00000L
LONG GetWindowLongA(HWND w, int index);   /* (GWL_STYLE: WS_CAPTION unless the window is fullscreen) */
enum {
    VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04, VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D,
    VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21,
    VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_INSERT = 0x2D, VK_DELETE = 0x2E, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D, VK_F1 = 0x70, VK_F2, VK_F3, VK_F4, VK_F5,
    VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12, VK_LSHIFT = 0xA0, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
    VK_LMENU, VK_RMENU, VK_OEM_PLUS = 0xBB, VK_OEM_MINUS = 0xBD,
};

/* ---- XInput's types (no XInput here: nn_hid.c finds no DLL, and SDL's pads come through pad_sdl.c) */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
#define XUSER_MAX_COUNT 4
enum {
    XINPUT_GAMEPAD_DPAD_UP = 0x0001, XINPUT_GAMEPAD_DPAD_DOWN = 0x0002, XINPUT_GAMEPAD_DPAD_LEFT = 0x0004,
    XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008, XINPUT_GAMEPAD_START = 0x0010, XINPUT_GAMEPAD_BACK = 0x0020,
    XINPUT_GAMEPAD_LEFT_THUMB = 0x0040, XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080, XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100,
    XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200, XINPUT_GAMEPAD_A = 0x1000, XINPUT_GAMEPAD_B = 0x2000,
    XINPUT_GAMEPAD_X = 0x4000, XINPUT_GAMEPAD_Y = 0x8000,
};
#define OutputDebugString OutputDebugStringA

/* ---- __try / __except: runs fn(arg); 1 when it returned, 0 when it faulted (SIGSEGV, SIGBUS, SIGILL, SIGFPE) and
   was abandoned there. For calls into code that may fault (a driver's shader compiler, a copy from a page unmapped
   meanwhile); the vectored handlers see the fault first, as on Windows. */
int w32_guard(void (*fn)(void *), void *arg);

#ifdef __cplusplus
}
#endif
#endif /* !_WIN32 && !__EMSCRIPTEN__ */
#endif
