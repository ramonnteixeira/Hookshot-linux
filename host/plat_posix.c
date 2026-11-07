/* (linux port) host/win32_compat.h on POSIX: the Win32 subset the host layer uses. Not built on Windows. */
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "win32_compat.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

/* ---- errors */
static __thread DWORD last_error;
DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD e) { last_error = e; }
static DWORD from_errno(int e) {
    switch (e) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ETIMEDOUT: return ERROR_TIMEOUT;
    default: return 0x20000000u | (DWORD)e;   /* (customer bit: an errno with no Win32 name) */
    }
}
static BOOL fail(void) { last_error = from_errno(errno); return FALSE; }

/* ---- condition variables: a futex on a sequence number. Wakes bump it before waking, so a waiter that read the old
   value before letting the lock go never misses one. */
static long futex(uint32_t *a, int op, uint32_t v, const struct timespec *t) {
    return syscall(SYS_futex, a, op | FUTEX_PRIVATE_FLAG, v, t, NULL, 0);
}
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags) {
    const uint32_t seq = __atomic_load_n(&cv->seq, __ATOMIC_ACQUIRE);
    pthread_rwlock_unlock(&lock->rw);
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    const long r = futex(&cv->seq, FUTEX_WAIT, seq, ms == INFINITE ? NULL : &t);
    const int timed_out = r < 0 && errno == ETIMEDOUT;
    if (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) pthread_rwlock_rdlock(&lock->rw);
    else pthread_rwlock_wrlock(&lock->rw);
    if (timed_out) { last_error = ERROR_TIMEOUT; return FALSE; }
    return TRUE;
}
void WakeConditionVariable(PCONDITION_VARIABLE cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_RELEASE);
    futex(&cv->seq, FUTEX_WAKE, 1, NULL);
}
void WakeAllConditionVariable(PCONDITION_VARIABLE cv) {
    __atomic_add_fetch(&cv->seq, 1, __ATOMIC_RELEASE);
    futex(&cv->seq, FUTEX_WAKE, INT_MAX, NULL);
}
void InitializeCriticalSection(LPCRITICAL_SECTION cs) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &a);
    pthread_mutexattr_destroy(&a);
}

/* ---- time */
static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
BOOL QueryPerformanceCounter(LARGE_INTEGER *t) { t->QuadPart = now_ns(); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return TRUE; }
ULONGLONG GetTickCount64(void) { return (ULONGLONG)(now_ns() / 1000000); }
DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
static void sleep_ns(int64_t ns) {
    struct timespec t = {ns / 1000000000LL, ns % 1000000000LL};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &t, &t) == EINTR) {}
}
void Sleep(DWORD ms) { if (ms) sleep_ns((int64_t)ms * 1000000); else sched_yield(); }
BOOL SwitchToThread(void) { sched_yield(); return TRUE; }

/* ---- handle objects */
enum { H_THREAD = 0x54485244, H_EVENT, H_TIMER, H_FILE, H_FIND };
typedef struct Obj {
    uint32_t kind;
    int refs;                       /* (a thread holds one on itself until it ends) */
    pthread_mutex_t m;
    pthread_cond_t cv;
    /* thread */
    pthread_t thread;
    pid_t tid;
    int started, done, joinable, suspended;
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    DWORD code;
    wchar_t *desc;
    /* event, timer */
    int manual, signaled;
    int64_t due_ns;
    /* file */
    int fd;
    /* find */
    DIR *dir;
    char dir_path[PATH_MAX], pattern[256];
} Obj;
#define PSEUDO_PROCESS ((HANDLE)(intptr_t)-1)
#define PSEUDO_THREAD ((HANDLE)(intptr_t)-2)

static Obj *obj_new(uint32_t kind) {
    Obj *o = (Obj *)calloc(1, sizeof *o);
    if (!o) return NULL;
    o->kind = kind;
    o->refs = 1;
    o->fd = -1;
    pthread_mutex_init(&o->m, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&o->cv, &ca);
    pthread_condattr_destroy(&ca);
    return o;
}
static void obj_unref(Obj *o) {
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (o->kind == H_FILE && o->fd > 2) close(o->fd);
    if (o->kind == H_FIND && o->dir) closedir(o->dir);
    free(o->desc);
    pthread_mutex_destroy(&o->m);
    pthread_cond_destroy(&o->cv);
    o->kind = 0;
    free(o);
}
static Obj *as(HANDLE h, uint32_t kind) {
    Obj *o = (Obj *)h;
    if (!o || h == INVALID_HANDLE_VALUE || h == PSEUDO_THREAD || o->kind != kind) return NULL;
    return o;
}

/* ---- threads */
static __thread Obj *self_obj;   /* this thread's object, when CreateThread or DuplicateHandle made one */
static pid_t gettid_(void) { return (pid_t)syscall(SYS_gettid); }

static void *thread_start(void *p) {
    Obj *o = (Obj *)p;
    self_obj = o;
    pthread_mutex_lock(&o->m);
    o->tid = gettid_();
    o->started = 1;
    pthread_cond_broadcast(&o->cv);
    while (o->suspended) pthread_cond_wait(&o->cv, &o->m);
    pthread_mutex_unlock(&o->m);
    if (o->desc) {
        char name[16];
        WideCharToMultiByte(CP_UTF8, 0, o->desc, -1, name, sizeof name, NULL, NULL);
        pthread_setname_np(pthread_self(), name);
    }
    const DWORD code = o->fn(o->arg);
    ExitThread(code);
}
static void thread_finish(DWORD code) {
    Obj *o = self_obj;
    if (!o) return;
    self_obj = NULL;
    pthread_mutex_lock(&o->m);
    o->code = code;
    o->done = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    obj_unref(o);
}
HANDLE CreateThread(void *attr, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *id) {
    (void)attr;
    Obj *o = obj_new(H_THREAD);
    if (!o) { last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    o->fn = fn, o->arg = arg, o->refs = 2, o->suspended = (flags & CREATE_SUSPENDED) != 0;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   /* (waits go through the object, not pthread_join) */
    /* (Windows' default is 1 MB; the game's threads ask for 64 MB reserved. glibc maps the whole size, untouched
       pages cost nothing) */
    pthread_attr_setstacksize(&a, stack ? (stack + 0xFFFF) & ~(SIZE_T)0xFFFF : 8u << 20);
    const int r = pthread_create(&o->thread, &a, thread_start, o);
    pthread_attr_destroy(&a);
    if (r) { errno = r; fail(); free(o); return NULL; }
    pthread_mutex_lock(&o->m);
    while (!o->started) pthread_cond_wait(&o->cv, &o->m);
    pthread_mutex_unlock(&o->m);
    if (id) *id = (DWORD)o->tid;
    return o;
}
DWORD ResumeThread(HANDLE h) {
    Obj *o = as(h, H_THREAD);
    if (!o) return (DWORD)-1;
    pthread_mutex_lock(&o->m);
    const DWORD was = (DWORD)o->suspended;
    o->suspended = 0;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    return was;
}
void ExitThread(DWORD code) {
    thread_finish(code);
    pthread_exit(NULL);
}
void ExitProcess(UINT code) { fflush(NULL); _exit((int)code); }
HANDLE GetCurrentThread(void) { return PSEUDO_THREAD; }
HANDLE GetCurrentProcess(void) { return PSEUDO_PROCESS; }
DWORD GetCurrentThreadId(void) { return (DWORD)gettid_(); }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
static pid_t tid_of(HANDLE h) {
    if (h == PSEUDO_THREAD) return gettid_();
    Obj *o = as(h, H_THREAD);
    return o ? o->tid : 0;
}
DWORD GetThreadId(HANDLE h) { return (DWORD)tid_of(h); }
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit, DWORD options) {
    (void)sp, (void)tp, (void)access, (void)inherit, (void)options;
    if (src == PSEUDO_THREAD) {   /* the calling thread, as a real handle (it never ends as far as waits can tell) */
        Obj *o = self_obj;
        if (!o) {
            o = obj_new(H_THREAD);
            if (!o) return FALSE;
            o->thread = pthread_self(), o->tid = gettid_(), o->started = 1;
            o->refs = 1;   /* (kept for the thread's lifetime: self_obj points at it) */
            self_obj = o;
        }
        __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
        *out = o;
        return TRUE;
    }
    Obj *o = (Obj *)src;
    if (!o || src == INVALID_HANDLE_VALUE) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    *out = o;
    return TRUE;
}
BOOL CloseHandle(HANDLE h) {
    if (h == PSEUDO_THREAD || h == PSEUDO_PROCESS) return TRUE;
    Obj *o = (Obj *)h;
    if (!o || h == INVALID_HANDLE_VALUE || (o->kind < H_THREAD || o->kind > H_FIND)) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    obj_unref(o);
    return TRUE;
}
/* Windows thread priorities as nice values, per thread (a raise needs CAP_SYS_NICE or RLIMIT_NICE: refused quietly
   otherwise, the thread keeps its priority) */
static __thread int my_prio;
BOOL SetThreadPriority(HANDLE h, int prio) {
    const pid_t tid = tid_of(h);
    if (!tid) return FALSE;
    const int nice = prio >= THREAD_PRIORITY_TIME_CRITICAL ? -10 : prio <= THREAD_PRIORITY_IDLE ? 19 : -prio * 2;
    setpriority(PRIO_PROCESS, (id_t)tid, nice);
    if (h == PSEUDO_THREAD) my_prio = prio;
    return TRUE;
}
int GetThreadPriority(HANDLE h) { return h == PSEUDO_THREAD ? my_prio : THREAD_PRIORITY_NORMAL; }
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
    const pid_t tid = tid_of(h);
    cpu_set_t old, set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++) if (mask >> i & 1) CPU_SET(i, &set);
    if (!tid || sched_getaffinity(tid, sizeof old, &old) || sched_setaffinity(tid, sizeof set, &set)) return 0;
    DWORD_PTR was = 0;
    for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &old)) was |= (DWORD_PTR)1 << i;
    return was ? was : 1;
}
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys) {
    (void)p;
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set)) return fail();
    DWORD_PTR m = 0;
    for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &set)) m |= (DWORD_PTR)1 << i;
    *proc = m;
    const long n = sysconf(_SC_NPROCESSORS_CONF);
    *sys = n >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << n) - 1);
    return TRUE;
}
BOOL GetLogicalProcessorInformation(SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info, DWORD *len) {
    (void)info, (void)len;
    last_error = 50;   /* ERROR_NOT_SUPPORTED: callers fall back to every logical core */
    return FALSE;
}
HRESULT SetThreadDescription(HANDLE h, PCWSTR name) {
    char n[16];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, n, sizeof n, NULL, NULL);
    n[15] = 0;
    Obj *o = h == PSEUDO_THREAD ? self_obj : as(h, H_THREAD);
    if (o) {
        free(o->desc);
        o->desc = wcsdup(name);
    }
    if (h == PSEUDO_THREAD) return pthread_setname_np(pthread_self(), n) ? E_FAIL : S_OK;
    if (o && o->started) return pthread_setname_np(o->thread, n) ? E_FAIL : S_OK;
    return o ? S_OK : E_FAIL;
}
HRESULT GetThreadDescription(HANDLE h, PWSTR *name) {
    Obj *o = h == PSEUDO_THREAD ? self_obj : as(h, H_THREAD);
    *name = wcsdup(o && o->desc ? o->desc : L"");
    return *name ? S_OK : E_FAIL;
}
void *LocalFree(void *p) { free(p); return NULL; }

/* ---- events and waitable timers */
HANDLE CreateEventA(void *attr, BOOL manual, BOOL initial, LPCSTR name) {
    (void)attr, (void)name;
    Obj *o = obj_new(H_EVENT);
    if (o) o->manual = manual, o->signaled = initial;
    return o;
}
BOOL SetEvent(HANDLE h) {
    Obj *o = as(h, H_EVENT);
    if (!o) return FALSE;
    pthread_mutex_lock(&o->m);
    o->signaled = 1;
    if (o->manual) pthread_cond_broadcast(&o->cv);
    else pthread_cond_signal(&o->cv);
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
BOOL ResetEvent(HANDLE h) {
    Obj *o = as(h, H_EVENT);
    if (!o) return FALSE;
    pthread_mutex_lock(&o->m);
    o->signaled = 0;
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
HANDLE CreateWaitableTimerExW(void *attr, LPCWSTR name, DWORD flags, DWORD access) {
    (void)attr, (void)name, (void)flags, (void)access;
    return obj_new(H_TIMER);
}
BOOL SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, void *wake, ULONG delay) {
    (void)period, (void)fn, (void)arg, (void)wake, (void)delay;
    Obj *o = as(h, H_TIMER);
    if (!o) return FALSE;
    /* (relative times only, in 100 ns units: the only form used here; an absolute one counts as due now) */
    pthread_mutex_lock(&o->m);
    o->due_ns = now_ns() + (due->QuadPart < 0 ? -due->QuadPart * 100 : 0);
    o->signaled = 0;
    pthread_mutex_unlock(&o->m);
    return TRUE;
}
static int wait_cv(Obj *o, const struct timespec *deadline) {
    return deadline ? pthread_cond_timedwait(&o->cv, &o->m, deadline) : pthread_cond_wait(&o->cv, &o->m);
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    Obj *o = (Obj *)h;
    if (!o || h == INVALID_HANDLE_VALUE || h == PSEUDO_THREAD) { last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
    if (o->kind == H_TIMER) {
        const int64_t left = o->due_ns - now_ns();
        if (ms != INFINITE && left > (int64_t)ms * 1000000) { sleep_ns((int64_t)ms * 1000000); return WAIT_TIMEOUT; }
        if (left > 0) {
            struct timespec t = {o->due_ns / 1000000000LL, o->due_ns % 1000000000LL};
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
        }
        return WAIT_OBJECT_0;
    }
    struct timespec deadline, *dl = NULL;
    if (ms != INFINITE) {
        const int64_t at = now_ns() + (int64_t)ms * 1000000;
        deadline.tv_sec = at / 1000000000LL, deadline.tv_nsec = at % 1000000000LL;
        dl = &deadline;
    }
    DWORD r = WAIT_OBJECT_0;
    pthread_mutex_lock(&o->m);
    if (o->kind == H_THREAD) {
        while (!o->done) if (wait_cv(o, dl) == ETIMEDOUT) { r = WAIT_TIMEOUT; break; }
    } else if (o->kind == H_EVENT) {
        while (!o->signaled) if (wait_cv(o, dl) == ETIMEDOUT) { r = WAIT_TIMEOUT; break; }
        if (r == WAIT_OBJECT_0 && !o->manual) o->signaled = 0;
    } else {
        r = WAIT_FAILED;
        last_error = ERROR_INVALID_HANDLE;
    }
    pthread_mutex_unlock(&o->m);
    return r;
}

/* ---- fibers: a ucontext and its stack */
typedef struct Fiber {
    ucontext_t uc;
    void *stack;
    size_t stack_size;
    LPFIBER_START_ROUTINE fn;
    void *param;
} Fiber;
static __thread Fiber *cur_fiber;
LPVOID ConvertThreadToFiberEx(LPVOID param, DWORD flags) {
    (void)flags;
    if (cur_fiber) return cur_fiber;
    Fiber *f = (Fiber *)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->param = param;
    cur_fiber = f;
    return f;
}
static void fiber_entry(unsigned lo, unsigned hi) {
    Fiber *f = (Fiber *)(uintptr_t)((uint64_t)hi << 32 | lo);
    f->fn(f->param);
    /* (a Windows fiber that returns ends its thread) */
    ExitThread(0);
}
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID param) {
    (void)flags;
    SIZE_T size = reserve > commit ? reserve : commit;
    if (size < (64u << 10)) size = 1u << 20;
    size = (size + 0xFFFF) & ~(SIZE_T)0xFFFF;
    Fiber *f = (Fiber *)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->stack = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (f->stack == MAP_FAILED) { free(f); last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    mprotect(f->stack, 4096, PROT_NONE);   /* (a guard page at the bottom) */
    f->stack_size = size, f->fn = fn, f->param = param;
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = f->stack;
    f->uc.uc_stack.ss_size = size;
    f->uc.uc_link = NULL;
    const uint64_t p = (uint64_t)(uintptr_t)f;
    makecontext(&f->uc, (void (*)(void))fiber_entry, 2, (unsigned)p, (unsigned)(p >> 32));
    return f;
}
void SwitchToFiber(LPVOID fiber) {
    Fiber *from = cur_fiber, *to = (Fiber *)fiber;
    if (!from || from == to) return;
    cur_fiber = to;
    swapcontext(&from->uc, &to->uc);
}
void DeleteFiber(LPVOID fiber) {
    Fiber *f = (Fiber *)fiber;
    if (!f) return;
    if (f == cur_fiber) cur_fiber = NULL;
    if (f->stack) munmap(f->stack, f->stack_size);
    free(f);
}

/* ---- text: UTF-8 <-> wchar_t (UTF-32 on Linux) */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s, int n, LPWSTR w, int cap) {
    (void)cp, (void)flags;
    const unsigned char *p = (const unsigned char *)s, *end = n < 0 ? NULL : p + n;
    int out = 0;
    for (;;) {
        if (end ? p >= end : 0) break;
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3F >> more;
        while (more-- && (!end || p < end) && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 0x3F);
        if (w) {
            if (out >= cap) { last_error = 122; return 0; }   /* ERROR_INSUFFICIENT_BUFFER */
            w[out] = (wchar_t)c;
        }
        out++;
        if (!end && !c) break;
    }
    return out;
}
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR w, int n, LPSTR s, int cap, LPCSTR def, BOOL *used) {
    (void)cp, (void)flags, (void)def;
    if (used) *used = FALSE;
    int out = 0;
    for (int i = 0; n < 0 || i < n; i++) {
        const uint32_t c = (uint32_t)w[i];
        char b[4];
        int k;
        if (c < 0x80) b[0] = (char)c, k = 1;
        else if (c < 0x800) b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3F)), k = 2;
        else if (c < 0x10000) b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 0x3F)), b[2] = (char)(0x80 | (c & 0x3F)), k = 3;
        else b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 0x3F)), b[2] = (char)(0x80 | (c >> 6 & 0x3F)),
             b[3] = (char)(0x80 | (c & 0x3F)), k = 4;
        if (s) {
            if (out + k > cap) {   /* (a truncated name still ends in a NUL when the caller's buffer is full) */
                if (cap > 0) s[cap - 1] = 0;
                last_error = 122;
                return 0;
            }
            memcpy(s + out, b, (size_t)k);
        }
        out += k;
        if (n < 0 && !c) break;
    }
    return out;
}

/* ---- files */
char *w32_path(char *path) {
    for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
    return path;
}
static const char *fix(LPCSTR in, char *buf) {
    snprintf(buf, PATH_MAX, "%s", in);
    return w32_path(buf);
}
static Obj std_files[3];
static int std_ready;
HANDLE GetStdHandle(DWORD which) {
    if (!__atomic_load_n(&std_ready, __ATOMIC_ACQUIRE)) {
        for (int i = 0; i < 3; i++) std_files[i].kind = H_FILE, std_files[i].fd = i, std_files[i].refs = 1 << 30;
        __atomic_store_n(&std_ready, 1, __ATOMIC_RELEASE);
    }
    const int fd = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : which == STD_ERROR_HANDLE ? 2 : -1;
    if (fd < 0 || fcntl(fd, F_GETFD) < 0) return INVALID_HANDLE_VALUE;
    return &std_files[fd];
}
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    (void)share, (void)sa, (void)tmpl;
    char buf[PATH_MAX];
    const char *p = fix(path, buf);
    int of = O_CLOEXEC;
    of |= (access & GENERIC_READ) && (access & GENERIC_WRITE) ? O_RDWR : (access & GENERIC_WRITE) ? O_WRONLY : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: of |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: of |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: of |= O_CREAT; break;
    case TRUNCATE_EXISTING: of |= O_TRUNC; break;
    default: break;
    }
    const int fd = open(p, of, (flags & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
    if (fd < 0) {
        last_error = errno == EEXIST ? ERROR_FILE_EXISTS : from_errno(errno);
        return INVALID_HANDLE_VALUE;
    }
    if (flags & FILE_FLAG_SEQUENTIAL_SCAN) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    if (flags & FILE_FLAG_RANDOM_ACCESS) posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
    Obj *o = obj_new(H_FILE);
    if (!o) { close(fd); last_error = ERROR_NOT_ENOUGH_MEMORY; return INVALID_HANDLE_VALUE; }
    o->fd = fd;
    if (disposition == OPEN_ALWAYS || disposition == CREATE_ALWAYS) last_error = 0;
    return o;
}
static int fd_of(HANDLE h) {
    Obj *o = as(h, H_FILE);
    if (!o) { last_error = ERROR_INVALID_HANDLE; return -1; }
    return o->fd;
}
static off_t ov_offset(const OVERLAPPED *ov) { return (off_t)((uint64_t)ov->OffsetHigh << 32 | ov->Offset); }
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, OVERLAPPED *ov) {
    const int fd = fd_of(h);
    if (got) *got = 0;
    if (fd < 0) return FALSE;
    ssize_t r;
    do r = ov ? pread(fd, buf, n, ov_offset(ov)) : read(fd, buf, n); while (r < 0 && errno == EINTR);
    if (r < 0) return fail();
    if (got) *got = (DWORD)r;
    return TRUE;
}
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, OVERLAPPED *ov) {
    const int fd = fd_of(h);
    if (put) *put = 0;
    if (fd < 0) return FALSE;
    DWORD done = 0;
    while (done < n) {
        const ssize_t r = ov ? pwrite(fd, (const char *)buf + done, n - done, ov_offset(ov) + done)
                             : write(fd, (const char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (put) *put = done; return fail(); }
        done += (DWORD)r;
    }
    if (put) *put = done;
    return TRUE;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) {
    struct stat st;
    const int fd = fd_of(h);
    if (fd < 0) return FALSE;
    if (fstat(fd, &st)) return fail();
    size->QuadPart = st.st_size;
    return TRUE;
}
DWORD GetFileSize(HANDLE h, DWORD *high) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(h, &s)) return 0xFFFFFFFFu;
    if (high) *high = (DWORD)((uint64_t)s.QuadPart >> 32);
    return (DWORD)s.QuadPart;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *now, DWORD method) {
    const int fd = fd_of(h);
    if (fd < 0) return FALSE;
    const off_t r = lseek(fd, to.QuadPart, method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) return fail();
    if (now) now->QuadPart = r;
    return TRUE;
}
BOOL SetEndOfFile(HANDLE h) {
    const int fd = fd_of(h);
    if (fd < 0) return FALSE;
    const off_t at = lseek(fd, 0, SEEK_CUR);
    return at >= 0 && !ftruncate(fd, at) ? TRUE : fail();
}
BOOL FlushFileBuffers(HANDLE h) {
    const int fd = fd_of(h);
    return fd >= 0 && !fsync(fd) ? TRUE : FALSE;
}
DWORD GetFileType(HANDLE h) {
    struct stat st;
    const int fd = fd_of(h);
    if (fd < 0 || fstat(fd, &st)) return FILE_TYPE_UNKNOWN;
    if (S_ISCHR(st.st_mode)) return FILE_TYPE_CHAR;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return FILE_TYPE_PIPE;
    return FILE_TYPE_DISK;
}
BOOL GetConsoleMode(HANDLE h, DWORD *mode) {
    const int fd = fd_of(h);
    if (fd < 0 || !isatty(fd)) return FALSE;
    if (mode) *mode = 3;
    return TRUE;
}
DWORD GetFileAttributesA(LPCSTR path) {
    char buf[PATH_MAX];
    struct stat st;
    if (stat(fix(path, buf), &st)) { fail(); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL CreateDirectoryA(LPCSTR path, void *sa) {
    (void)sa;
    char buf[PATH_MAX];
    return mkdir(fix(path, buf), 0755) ? fail() : TRUE;
}
BOOL RemoveDirectoryA(LPCSTR path) { char buf[PATH_MAX]; return rmdir(fix(path, buf)) ? fail() : TRUE; }
BOOL DeleteFileA(LPCSTR path) { char buf[PATH_MAX]; return unlink(fix(path, buf)) ? fail() : TRUE; }
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    char a[PATH_MAX], b[PATH_MAX];
    fix(from, a), fix(to, b);
    if (!(flags & MOVEFILE_REPLACE_EXISTING)) {   /* (Windows refuses to replace without the flag) */
        struct stat st;
        if (!lstat(b, &st)) { last_error = ERROR_ALREADY_EXISTS; return FALSE; }
    }
    return rename(a, b) ? fail() : TRUE;
}
BOOL MoveFileA(LPCSTR from, LPCSTR to) { return MoveFileExA(from, to, 0); }
BOOL SetCurrentDirectoryA(LPCSTR path) { char buf[PATH_MAX]; return chdir(fix(path, buf)) ? fail() : TRUE; }

static BOOL find_next(Obj *o, WIN32_FIND_DATAA *fd) {
    struct dirent *e;
    while ((e = readdir(o->dir))) {
        if (fnmatch(o->pattern, e->d_name, 0)) continue;
        char full[PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", o->dir_path, e->d_name);
        struct stat st;
        memset(fd, 0, sizeof *fd);
        if (!stat(full, &st)) {
            fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            const uint64_t size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
            fd->nFileSizeHigh = (DWORD)(size >> 32), fd->nFileSizeLow = (DWORD)size;
            /* (FILETIME: 100 ns units since 1601) */
            const uint64_t ft = ((uint64_t)st.st_mtim.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)st.st_mtim.tv_nsec / 100;
            fd->ftLastWriteTime.dwLowDateTime = (DWORD)ft, fd->ftLastWriteTime.dwHighDateTime = (DWORD)(ft >> 32);
        } else {
            fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        }
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        return TRUE;
    }
    last_error = 18;   /* ERROR_NO_MORE_FILES */
    return FALSE;
}
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd) {
    char buf[PATH_MAX];
    fix(pattern, buf);
    Obj *o = obj_new(H_FIND);
    if (!o) return INVALID_HANDLE_VALUE;
    char *slash = strrchr(buf, '/');
    if (slash) {
        *slash = 0;
        snprintf(o->dir_path, sizeof o->dir_path, "%s", buf[0] ? buf : "/");
        snprintf(o->pattern, sizeof o->pattern, "%s", slash + 1);
    } else {
        snprintf(o->dir_path, sizeof o->dir_path, ".");
        snprintf(o->pattern, sizeof o->pattern, "%s", buf);
    }
    if (!strcmp(o->pattern, "*.*") || !o->pattern[0]) snprintf(o->pattern, sizeof o->pattern, "*");
    o->dir = opendir(o->dir_path);
    if (!o->dir) { fail(); obj_unref(o); return INVALID_HANDLE_VALUE; }
    if (!find_next(o, fd)) { obj_unref(o); last_error = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
    return o;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) {
    Obj *o = as(h, H_FIND);
    return o ? find_next(o, fd) : FALSE;
}
BOOL FindClose(HANDLE h) {
    Obj *o = as(h, H_FIND);
    if (!o) return FALSE;
    obj_unref(o);
    return TRUE;
}

/* ---- modules: "x.dll" is looked for as x.so (beside the given path, then on the library path); SDL2.dll is the
   system's libSDL2. Windows' own DLLs (user32, xinput, ...) do not exist here, so their lookups fail and the callers
   take their fallbacks. */
static void *try_open(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
HMODULE LoadLibraryA(LPCSTR name) {
    char buf[PATH_MAX];
    fix(name, buf);
    void *h = try_open(buf);
    if (h) return h;
    const size_t n = strlen(buf);
    if (n > 4 && !strcasecmp(buf + n - 4, ".dll")) {
        const char *base = strrchr(buf, '/');
        base = base ? base + 1 : buf;
        if (!strcasecmp(base, "SDL2.dll")) {
            static const char *const sdl[] = {"libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so"};
            char dir[PATH_MAX], p[PATH_MAX * 2];
            snprintf(dir, sizeof dir, "%.*s", (int)(base - buf), buf);
            for (int i = 0; i < 3; i++) {
                snprintf(p, sizeof p, "%s%s", dir, sdl[i]);
                if ((h = try_open(p)) || (h = try_open(sdl[i]))) return h;
            }
        }
        char p[PATH_MAX + 8];
        snprintf(p, sizeof p, "%.*s.so", (int)(n - 4), buf);
        if ((h = try_open(p))) return h;
        if (base != buf) {   /* (lib<name>.so beside it) */
            snprintf(p, sizeof p, "%.*slib%.*s.so", (int)(base - buf), buf, (int)(buf + n - 4 - base), base);
            if ((h = try_open(p))) return h;
        }
    }
    last_error = ERROR_FILE_NOT_FOUND;
    return NULL;
}
HMODULE GetModuleHandleA(LPCSTR name) {
    if (!name) {
        static void *self;
        if (!self) self = dlopen(NULL, RTLD_NOW);
        return self;
    }
    last_error = 126;   /* ERROR_MOD_NOT_FOUND */
    return NULL;
}
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name_or_addr, HMODULE *out) {
    *out = NULL;
    if (!(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)) return (*out = GetModuleHandleA(name_or_addr)) != NULL;
    Dl_info di;
    if (!dladdr((const void *)name_or_addr, &di) || !di.dli_fbase) return FALSE;
    *out = di.dli_fbase;   /* (a module is its load address: GetModuleFileNameA resolves it back) */
    return TRUE;
}
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD cap) {
    if (!cap) return 0;
    if (m && m != GetModuleHandleA(NULL)) {
        Dl_info di;
        if (dladdr(m, &di) && di.dli_fname) {
            char real[PATH_MAX];
            const char *name = realpath(di.dli_fname, real) ? real : di.dli_fname;
            snprintf(out, cap, "%s", name);
            return (DWORD)strlen(out);
        }
    }
    const ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n < 0) { out[0] = 0; return (DWORD)fail(); }
    out[n] = 0;
    return (DWORD)n;
}
FARPROC GetProcAddress(HMODULE m, LPCSTR name) {
    if (!m) return NULL;
    void *p = dlsym(m, name);
    FARPROC f;
    memcpy(&f, &p, sizeof f);
    return f;
}
BOOL FreeLibrary(HMODULE m) { return m && !dlclose(m); }

/* ---- virtual memory */
static int prot_of(DWORD p) {
    switch (p & 0xFF) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot) {
    if (type & MEM_RESET) {   /* (contents no longer needed; pages stay usable) */
        madvise(addr, size, MADV_FREE);
        return addr;
    }
    if (addr && !(type & MEM_RESERVE)) {   /* commit inside an existing reservation */
        return mprotect(addr, size, prot_of(prot)) ? (fail(), (LPVOID)NULL) : addr;
    }
    /* (a reservation without a commit is PROT_NONE; MAP_NORESERVE either way: nothing is charged before it is
       touched, as with Windows' commit of untouched pages) */
    const int pr = (type & MEM_COMMIT) ? prot_of(prot) : PROT_NONE;
    void *p = mmap(addr, size, pr, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (addr ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (p == MAP_FAILED || (addr && p != addr)) {
        if (p != MAP_FAILED) munmap(p, size);
        fail();
        return NULL;
    }
    return p;
}
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    if (type & MEM_DECOMMIT) {   /* (the pages go back to the system and read as zeros afterwards) */
        return madvise(addr, size, MADV_DONTNEED) ? fail() : TRUE;
    }
    if (type & MEM_RELEASE) {
        /* (Windows releases the whole allocation with size 0: the mapping it lies in is looked up) */
        if (!size) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(addr, &mbi, sizeof mbi) || mbi.State == MEM_FREE) return FALSE;
            size = (SIZE_T)((uint8_t *)mbi.BaseAddress + mbi.RegionSize - (uint8_t *)addr);
        }
        return munmap(addr, size) ? fail() : TRUE;
    }
    return FALSE;
}
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD *old) {
    if (old) {
        MEMORY_BASIC_INFORMATION mbi;
        *old = VirtualQuery(addr, &mbi, sizeof mbi) ? mbi.Protect : PAGE_READWRITE;
    }
    const uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)4095;
    return mprotect((void *)lo, size + ((uintptr_t)addr - lo), prot_of(prot)) ? fail() : TRUE;
}
/* from /proc/self/maps: the mapping that holds addr (MEM_COMMIT; MEM_RESERVE if it is PROT_NONE), or the gap up to the
   next one (MEM_FREE). Slow (a file read a call): for the debugging tools and one-off checks that use it. */
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len) {
    if (len < sizeof *mbi) return 0;
    const uintptr_t a = (uintptr_t)addr & ~(uintptr_t)4095;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t gap_lo = 0, gap_hi = UINTPTR_MAX;
    int found = 0;
    memset(mbi, 0, sizeof *mbi);
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) != 3) continue;
        if (a >= lo && a < hi) {
            const int r = perm[0] == 'r', w = perm[1] == 'w', x = perm[2] == 'x';
            mbi->BaseAddress = (void *)a;
            mbi->AllocationBase = (void *)(uintptr_t)lo;
            mbi->RegionSize = hi - a;
            mbi->State = (r || w || x) ? MEM_COMMIT : MEM_RESERVE;
            mbi->Protect = !(r || w || x) ? PAGE_NOACCESS : x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                                                              : w ? PAGE_READWRITE : PAGE_READONLY;
            mbi->AllocationProtect = mbi->Protect;
            mbi->Type = perm[3] == 's' ? MEM_MAPPED : MEM_PRIVATE;
            found = 1;
            break;
        }
        if (hi <= a) gap_lo = hi;
        else if (lo > a) { gap_hi = lo; break; }
    }
    fclose(f);
    if (!found) {
        mbi->BaseAddress = (void *)a;
        mbi->AllocationBase = NULL;
        mbi->RegionSize = gap_hi - (a > gap_lo ? a : gap_lo);
        mbi->State = MEM_FREE;
        mbi->Protect = PAGE_NOACCESS;
    }
    return sizeof *mbi;
}

/* ---- vectored exception handlers over signals */
#define MAX_VEH 16
static PVECTORED_EXCEPTION_HANDLER veh[MAX_VEH];
static int veh_n;
static pthread_mutex_t veh_lock = PTHREAD_MUTEX_INITIALIZER;
static LPTOP_LEVEL_EXCEPTION_FILTER top_filter;
static struct sigaction old_act[32];
static void fill_context(CONTEXT *c, const ucontext_t *uc) {
    const greg_t *g = uc->uc_mcontext.gregs;
    memset(c, 0, sizeof *c);
    c->ContextFlags = CONTEXT_FULL;
    c->Rax = g[REG_RAX], c->Rcx = g[REG_RCX], c->Rdx = g[REG_RDX], c->Rbx = g[REG_RBX];
    c->Rsp = g[REG_RSP], c->Rbp = g[REG_RBP], c->Rsi = g[REG_RSI], c->Rdi = g[REG_RDI];
    c->R8 = g[REG_R8], c->R9 = g[REG_R9], c->R10 = g[REG_R10], c->R11 = g[REG_R11];
    c->R12 = g[REG_R12], c->R13 = g[REG_R13], c->R14 = g[REG_R14], c->R15 = g[REG_R15];
    c->Rip = g[REG_RIP], c->EFlags = (DWORD)g[REG_EFL];
}
static void store_context(const CONTEXT *c, ucontext_t *uc) {
    greg_t *g = uc->uc_mcontext.gregs;
    g[REG_RAX] = c->Rax, g[REG_RCX] = c->Rcx, g[REG_RDX] = c->Rdx, g[REG_RBX] = c->Rbx;
    g[REG_RSP] = c->Rsp, g[REG_RBP] = c->Rbp, g[REG_RSI] = c->Rsi, g[REG_RDI] = c->Rdi;
    g[REG_R8] = c->R8, g[REG_R9] = c->R9, g[REG_R10] = c->R10, g[REG_R11] = c->R11;
    g[REG_R12] = c->R12, g[REG_R13] = c->R13, g[REG_R14] = c->R14, g[REG_R15] = c->R15;
    g[REG_RIP] = c->Rip, g[REG_EFL] = c->EFlags;
}
#include <setjmp.h>
static __thread sigjmp_buf *guard_jb;   /* (w32_guard's, while its call runs) */
static void install_signals(void);
int w32_guard(void (*fn)(void *), void *arg) {
    install_signals();
    sigjmp_buf jb, *outer = guard_jb;
    if (sigsetjmp(jb, 1)) {
        guard_jb = outer;
        return 0;
    }
    guard_jb = &jb;
    fn(arg);
    guard_jb = outer;
    return 1;
}
static void on_signal(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = (ucontext_t *)ucv;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    memset(&rec, 0, sizeof rec);
    fill_context(&ctx, uc);
    rec.ExceptionAddress = (void *)(uintptr_t)ctx.Rip;
    switch (sig) {
    case SIGSEGV: case SIGBUS:
        rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = (uc->uc_mcontext.gregs[REG_ERR] & 2) ? 1 : 0;   /* (x86 page fault: the W bit) */
        rec.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
        break;
    case SIGILL: rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION; break;
    case SIGFPE: rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO; break;
    case SIGTRAP: rec.ExceptionCode = si->si_code == TRAP_BRKPT ? EXCEPTION_BREAKPOINT : EXCEPTION_SINGLE_STEP; break;
    default: break;
    }
    EXCEPTION_POINTERS ep = {&rec, &ctx};
    const int n = __atomic_load_n(&veh_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        PVECTORED_EXCEPTION_HANDLER h = veh[i];
        if (h && h(&ep) == EXCEPTION_CONTINUE_EXECUTION) { store_context(&ctx, uc); return; }
    }
    if (guard_jb && sig != SIGTRAP) siglongjmp(*guard_jb, 1);   /* (a w32_guard call: its __except) */
    if (top_filter && top_filter(&ep) == EXCEPTION_CONTINUE_EXECUTION) { store_context(&ctx, uc); return; }
    /* (unhandled: whatever was there before, or the default action, which ends the process with this signal) */
    struct sigaction *o = &old_act[sig];
    if ((o->sa_flags & SA_SIGINFO) && o->sa_sigaction) { o->sa_sigaction(sig, si, ucv); return; }
    if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN && o->sa_handler) { o->sa_handler(sig); return; }
    signal(sig, SIG_DFL);
    raise(sig);
}
static void install_signals(void) {
    static int done;
    if (done++) return;
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, &old_act[sigs[i]]);
}
PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) {
    pthread_mutex_lock(&veh_lock);
    install_signals();
    if (veh_n >= MAX_VEH) { pthread_mutex_unlock(&veh_lock); return NULL; }
    if (first) {
        memmove(veh + 1, veh, (size_t)veh_n * sizeof *veh);
        veh[0] = h;
    } else {
        veh[veh_n] = h;
    }
    __atomic_add_fetch(&veh_n, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&veh_lock);
    return (PVOID)h;
}
ULONG RemoveVectoredExceptionHandler(PVOID h) {
    pthread_mutex_lock(&veh_lock);
    for (int i = 0; i < veh_n; i++)
        if ((PVOID)veh[i] == h) {
            memmove(veh + i, veh + i + 1, (size_t)(veh_n - i - 1) * sizeof *veh);
            __atomic_sub_fetch(&veh_n, 1, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&veh_lock);
            return 1;
        }
    pthread_mutex_unlock(&veh_lock);
    return 0;
}
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    pthread_mutex_lock(&veh_lock);
    install_signals();
    LPTOP_LEVEL_EXCEPTION_FILTER was = top_filter;
    top_filter = f;
    pthread_mutex_unlock(&veh_lock);
    return was;
}

/* ---- misc */
int MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type) {
    (void)w, (void)type;
    fprintf(stderr, "%s: %s\n", caption ? caption : "Hookshot", text ? text : "");
    fflush(stderr);
    return 1;   /* IDOK */
}
void OutputDebugStringA(LPCSTR s) { fputs(s, stderr); }
int w32_screen_w, w32_screen_h;
int GetSystemMetrics(int which) { return which == SM_CXSCREEN ? w32_screen_w : which == SM_CYSCREEN ? w32_screen_h : 0; }
#endif
