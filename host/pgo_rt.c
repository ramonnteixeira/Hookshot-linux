/* (hookshot) Profile-guided builds (RX_PGO=gen, nx2/build.py): what clang's -fprofile-instr-generate needs, in place of compiler-rt's
   profile library, which this toolchain lacks. The counters sit in sections .lprfc/.lprfd/.lprfn; this writes them in LLVM's raw
   format for llvm-profdata.
   HOS_PGO=<path>: every 500 ms, if <path>.reset exists the counters are cleared, and if <path>.dump exists they are written to <path>;
   each file is deleted when done. So a run can leave out the loading. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int __llvm_profile_runtime = 0;

#pragma section(".lprfc$A", read, write)
#pragma section(".lprfc$Z", read, write)
#pragma section(".lprfd$A", read, write)
#pragma section(".lprfd$Z", read, write)
#pragma section(".lprfn$A", read)
#pragma section(".lprfn$Z", read)
__declspec(allocate(".lprfc$A")) __attribute__((used, aligned(8))) char pgo_cnt_a[8] = {0};
__declspec(allocate(".lprfc$Z")) __attribute__((used, aligned(8))) char pgo_cnt_z[8] = {0};
__declspec(allocate(".lprfd$A")) __attribute__((used, aligned(8))) char pgo_dat_a[72] = {0};
__declspec(allocate(".lprfd$Z")) __attribute__((used, aligned(8))) char pgo_dat_z[72] = {0};
__declspec(allocate(".lprfn$A")) __attribute__((used)) const char pgo_nam_a[1] = {0};
__declspec(allocate(".lprfn$Z")) __attribute__((used)) const char pgo_nam_z[1] = {0};

enum { DATA_SIZE = 72 };

static void pgo_write(const char *path) {
    const char *cb = pgo_cnt_a + 8, *ce = pgo_cnt_z, *db = pgo_dat_a + DATA_SIZE, *de = pgo_dat_z, *nb = pgo_nam_a + 1, *ne = pgo_nam_z;
    const uint64_t ndata = (uint64_t)(de - db) / DATA_SIZE, ncnt = (uint64_t)(ce - cb) / 8, nnames = (uint64_t)(ne - nb);
    const uint64_t header[] = {
        0xff6c70726f667281ULL,        /* magic: \xfflprofr\x81 */
        11,                           /* (the format llvm-profdata 24 reads) */
        0,                            /* binary ids */
        ndata, 0, ncnt, 0,
        0, 0,                         /* bitmap */
        0, 0, 0,                      /* uniform counters */
        nnames,
        (uint64_t)(uintptr_t)cb - (uint64_t)(uintptr_t)db,   /* CountersDelta */
        (uint64_t)0,                  /* BitmapDelta */
        (uint64_t)(uintptr_t)nb,      /* NamesDelta */
        0, 0,                         /* vtables */
        2,                            /* ValueKindLast (IPVK_Last) */
    };
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(header, sizeof header, 1, f);
    /* (the records' pointers to their counters are 32 bits in a COFF image, the rest zero, and the counters come first: negative) */
    char *copy = malloc((size_t)(de - db));
    memcpy(copy, db, (size_t)(de - db));
    for (uint64_t i = 0; i < ndata; i++) {
        int32_t rel;
        memcpy(&rel, copy + i * DATA_SIZE + 16, 4);
        const int64_t wide = rel;
        memcpy(copy + i * DATA_SIZE + 16, &wide, 8);
    }
    fwrite(copy, 1, (size_t)(de - db), f);
    free(copy);
    fwrite(cb, 1, (size_t)(ce - cb), f);
    fwrite(nb, 1, (size_t)nnames, f);
    static const char zero[8];
    fwrite(zero, 1, (size_t)(-(int64_t)nnames & 7), f);
    fclose(f);
}

static DWORD WINAPI pgo_thread(void *arg) {
    const char *path = arg;
    char dump[MAX_PATH], reset[MAX_PATH];
    snprintf(dump, sizeof dump, "%s.dump", path);
    snprintf(reset, sizeof reset, "%s.reset", path);
    for (;;) {
        Sleep(500);
        if (GetFileAttributesA(reset) != INVALID_FILE_ATTRIBUTES) {
            memset(pgo_cnt_a + 8, 0, (size_t)(pgo_cnt_z - (pgo_cnt_a + 8)));
            DeleteFileA(reset);
        }
        if (GetFileAttributesA(dump) != INVALID_FILE_ATTRIBUTES) {
            pgo_write(path);
            DeleteFileA(dump);
        }
    }
    return 0;
}

__attribute__((constructor)) static void pgo_start(void) {
    const char *e = getenv("HOS_PGO");
    if (e) CreateThread(NULL, 0, pgo_thread, _strdup(e), 0, NULL);
}
