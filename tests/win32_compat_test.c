/* (linux port) A run-time test of host/plat_posix.c, the Win32 stand-ins: threads, events, timers, SRW locks and
   condition variables, fibers, files and directory listing, virtual memory, vectored exception handlers.

   clang -O1 -D_GNU_SOURCE -Ihost tests/win32_compat_test.c host/plat_posix.c -lpthread -ldl -o /tmp/w32t && /tmp/w32t */
#include "win32_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static SRWLOCK lock = SRWLOCK_INIT;
static CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
static volatile LONG counter, ready;

static DWORD WINAPI worker(void *p) {
    for (int i = 0; i < 100000; i++) {
        AcquireSRWLockExclusive(&lock);
        counter++;
        ReleaseSRWLockExclusive(&lock);
    }
    return (DWORD)(uintptr_t)p;
}
static DWORD WINAPI waker(void *p) {
    (void)p;
    Sleep(20);
    AcquireSRWLockExclusive(&lock);
    ready = 1;
    WakeAllConditionVariable(&cv);
    ReleaseSRWLockExclusive(&lock);
    return 0;
}
static DWORD WINAPI set_later(void *e) { Sleep(10); SetEvent(e); return 0; }

static LPVOID main_fiber, other_fiber;
static int fiber_steps;
static void WINAPI fiber_fn(LPVOID p) {
    fiber_steps += (int)(uintptr_t)p;
    SwitchToFiber(main_fiber);
    fiber_steps += 10;
    SwitchToFiber(main_fiber);
}

static void bad_read(void *p) { *(volatile int *)p = *(volatile int *)0x10; }
static void good_call(void *p) { *(int *)p = 1; }
static volatile char *guarded;
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const char *at = (const char *)e->ExceptionRecord->ExceptionInformation[1];
    if (at < guarded || at >= guarded + 4096) return EXCEPTION_CONTINUE_SEARCH;
    CHECK(e->ExceptionRecord->ExceptionInformation[0] == 1);   /* (a write) */
    DWORD old;
    VirtualProtect((void *)guarded, 4096, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* threads and SRW locks */
    HANDLE t[4];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 64 << 20, worker, (void *)(uintptr_t)i, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    for (int i = 0; i < 4; i++) { CHECK(WaitForSingleObject(t[i], INFINITE) == WAIT_OBJECT_0); CloseHandle(t[i]); }
    CHECK(counter == 400000);
    CHECK(InterlockedIncrement(&counter) == 400001);
    CHECK(InterlockedExchange(&counter, 5) == 400001);
    CHECK(InterlockedCompareExchange(&counter, 7, 5) == 5 && counter == 7);

    /* condition variable: a timeout, then a wake */
    AcquireSRWLockExclusive(&lock);
    CHECK(!SleepConditionVariableSRW(&cv, &lock, 10, 0) && GetLastError() == ERROR_TIMEOUT);
    CloseHandle(CreateThread(NULL, 0, waker, NULL, 0, NULL));
    while (!ready) SleepConditionVariableSRW(&cv, &lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&lock);
    CHECK(ready);

    /* events: auto reset, a timeout */
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(WaitForSingleObject(ev, 5) == WAIT_TIMEOUT);
    CloseHandle(CreateThread(NULL, 0, set_later, ev, 0, NULL));
    CHECK(WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);   /* (auto reset) */
    CloseHandle(ev);

    /* a waitable timer: 20 ms */
    HANDLE tm = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due = {.QuadPart = -200000}, a, b, f;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    SetWaitableTimerEx(tm, &due, 0, NULL, NULL, NULL, 0);
    WaitForSingleObject(tm, INFINITE);
    QueryPerformanceCounter(&b);
    CHECK((b.QuadPart - a.QuadPart) * 1000 / f.QuadPart >= 19);
    CloseHandle(tm);

    /* fibers */
    main_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    other_fiber = CreateFiberEx(64 << 10, 1 << 20, FIBER_FLAG_FLOAT_SWITCH, fiber_fn, (void *)(uintptr_t)3);
    SwitchToFiber(other_fiber);
    CHECK(fiber_steps == 3);
    SwitchToFiber(other_fiber);
    CHECK(fiber_steps == 13);
    DeleteFiber(other_fiber);

    /* files, with Windows separators */
    char dir[256], path[300], pat[300];
    snprintf(dir, sizeof dir, "/tmp/w32t_%d", (int)GetCurrentProcessId());
    CHECK(CreateDirectoryA(dir, NULL));
    CHECK(!CreateDirectoryA(dir, NULL) && GetLastError() == ERROR_ALREADY_EXISTS);
    snprintf(path, sizeof path, "%s\\file.bin", dir);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL) == INVALID_HANDLE_VALUE &&
          GetLastError() == ERROR_FILE_EXISTS);
    OVERLAPPED ov = {0};
    ov.Offset = 100;
    DWORD put = 0, got = 0;
    CHECK(WriteFile(h, "hello", 5, &put, &ov) && put == 5);
    LARGE_INTEGER size;
    CHECK(GetFileSizeEx(h, &size) && size.QuadPart == 105);
    char buf[8] = {0};
    CHECK(ReadFile(h, buf, 5, &got, &ov) && got == 5 && !memcmp(buf, "hello", 5));
    LARGE_INTEGER to = {.QuadPart = 50};
    CHECK(SetFilePointerEx(h, to, NULL, FILE_BEGIN) && SetEndOfFile(h));
    CHECK(GetFileSizeEx(h, &size) && size.QuadPart == 50);
    CloseHandle(h);
    CHECK(GetFileAttributesA(path) == FILE_ATTRIBUTE_NORMAL);
    CHECK(GetFileAttributesA(dir) == FILE_ATTRIBUTE_DIRECTORY);
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(pat, &fd);
    int files = 0, dots = 0;
    CHECK(fh != INVALID_HANDLE_VALUE);
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) dots++;
        else if (!strcmp(fd.cFileName, "file.bin") && fd.nFileSizeLow == 50) files++;
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    CHECK(files == 1 && dots == 2);
    CHECK(DeleteFileA(path) && RemoveDirectoryA(dir));
    CHECK(GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES);

    /* text */
    wchar_t w[16];
    char back[16];
    CHECK(MultiByteToWideChar(CP_UTF8, 0, "ação", -1, w, 16) == 5 && w[1] == 0xE7);
    CHECK(WideCharToMultiByte(CP_UTF8, 0, w, -1, back, 16, NULL, NULL) == 7 && !strcmp(back, "ação"));

    /* virtual memory and a vectored handler: a write to a read-only page, made writable by the handler */
    char *mem = (char *)VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(mem != NULL);
    MEMORY_BASIC_INFORMATION mbi;
    CHECK(VirtualQuery(mem + 4096, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE);
    DWORD old;
    guarded = mem + 8192;
    CHECK(VirtualProtect((void *)guarded, 4096, PAGE_READONLY, &old) && old == PAGE_READWRITE);
    CHECK(AddVectoredExceptionHandler(1, on_fault) != NULL);
    guarded[5] = 42;   /* (faults once; the handler makes it writable and the store runs again) */
    CHECK(guarded[5] == 42);
    CHECK(VirtualFree(mem, 1 << 20, MEM_DECOMMIT));
    CHECK(mem[100] == 0);
    CHECK(VirtualFree(mem, 0, MEM_RELEASE));
    CHECK(VirtualQuery(mem, &mbi, sizeof mbi) && mbi.State == MEM_FREE);

    /* __try / __except: a fault abandons the call, a clean one returns */
    int out = 0;
    CHECK(w32_guard(bad_read, &out) == 0);
    CHECK(w32_guard(good_call, &out) == 1 && out == 1);
    CHECK(w32_guard(bad_read, &out) == 0);   /* (again: the signal is not left blocked) */

    /* modules */
    char exe[MAX_PATH];
    CHECK(GetModuleFileNameA(NULL, exe, MAX_PATH) > 0 && exe[0] == '/');
    CHECK(GetModuleHandleA("kernel32.dll") == NULL);
    CHECK(GetProcAddress(GetModuleHandleA(NULL), "main") != NULL);

    printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures != 0;
}
