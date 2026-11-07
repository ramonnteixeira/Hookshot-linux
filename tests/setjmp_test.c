/* (linux port) A run-time test of host/hos_setjmp.c's hand-written setjmp/longjmp on the host's own calling convention
   (on Linux, System V: the buffer in rdi, longjmp's value in esi): a first return of 0, a longjmp from nested calls
   back to it with the value (0 becoming 1), and the caller's locals and callee-saved registers intact after it.

   clang -O2 -D_GNU_SOURCE -Ihost -I<rt64> -I<gen> tests/setjmp_test.c -o /tmp/sjt && /tmp/sjt
   (rt64 and gen: the generated headers, as scripts/linux_check.py makes them) */
#include "../host/hos_setjmp.c"
#include <stdio.h>

void hos_log(const char *fmt, ...) { (void)fmt; }
void a64_icall(Ctx *c, uint64_t target) { (void)c, (void)target; }

static void *buf[32];
static volatile int depth;

__attribute__((noinline)) static void jump_from_deep(int n, int value) {
    volatile char pad[256];
    pad[0] = (char)n;
    depth = n + pad[0] - n;
    if (n > 0) jump_from_deep(n - 1, value);
    recomp_sj_longjmp(buf, value);
}

__attribute__((noinline)) static int run(int value) {
    /* (locals the compiler keeps in callee-saved registers across the call) */
    register uint64_t a = 0x1111222233334444ull, b = 0x5555666677778888ull;
    volatile int rounds = 0;
    const int r = recomp_sj_setjmp(buf);
    if (r == 0) {
        rounds++;
        jump_from_deep(20, value);
        return -1;   /* (not reached) */
    }
    if (a != 0x1111222233334444ull || b != 0x5555666677778888ull || rounds != 1) return -2;
    return r;
}

int main(void) {
    int failures = 0;
    const int r7 = run(7), r0 = run(0);
    if (r7 != 7) printf("FAIL: longjmp(7) came back as %d\n", r7), failures++;
    if (r0 != 1) printf("FAIL: longjmp(0) came back as %d (want 1)\n", r0), failures++;
    printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures != 0;
}
