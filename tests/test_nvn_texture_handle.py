"""Exercise the production handle constructor with SDK-observed boundary values.

Castle Town's guard house passes texture -1 and sampler 264. The SDK returns
UINT64_MAX; the former range check stopped the game. Texture zero is special:
it returns zero even with a nonzero or negative sampler. Fixtures are authored
constants; this test contains no game code and needs no dump or GPU.
"""
import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="C compiler (otherwise clang, gcc or cc from PATH)")
    parser.add_argument("--source", type=Path, default=ROOT / "host/nvn_native.c")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("null_pool", ROOT / "tests/test_nvn_null_pool.py")
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    body = helpers.block(args.source.read_text(), "NV(nvnDeviceGetTextureHandle)")
    fixture = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint64_t x[32]; } Ctx;
#define I(i) ((int)c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define NV(name) static void n_##name(Ctx *c, void *unused)
static void die(const char *fmt, ...) { (void)fmt; exit(86); }
''' + body + r'''
static const struct { uint64_t texture, sampler, expected; } cases[] = {
    {0xffffffff, 264, UINT64_MAX},
    {0, 264, 0},
    {0, 0xffffffff, 0},
    {1, 264, 0x10800001},
    {0xfffff, 0xfff, 0xffffffff},
    {0x100000, 264, 0x10900000},
    {0xfffffffe, 264, UINT64_MAX - 1},
    {1, 0xffffffff, UINT64_C(0xfffffffffff00001)},
    {0x80000000, 0, UINT64_C(0xffffffff80000000)},
    {1, 0x80000000, UINT64_C(0xfff8000000000001)},
    {0xfeedface00000000, 264, 0},
    {0x12345678ffffffff, 264, UINT64_MAX},
    {1, 0xfeedface00000108, 0x10800001},
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    unsigned i = (unsigned)atoi(argv[1]);
    if (i >= sizeof cases / sizeof *cases) return 2;
    Ctx c, before;
    memset(&c, 0xa5, sizeof c);
    c.x[1] = cases[i].texture;
    c.x[2] = cases[i].sampler;
    before = c;
    n_nvnDeviceGetTextureHandle(&c, NULL);
    if (c.x[0] != cases[i].expected) {
        fprintf(stderr, "case %u: got %016llx, expected %016llx\n", i,
                (unsigned long long)c.x[0], (unsigned long long)cases[i].expected);
        return 1;
    }
    if (memcmp(c.x + 1, before.x + 1, sizeof c.x - sizeof c.x[0])) return 1;
    return 0;
}
'''
    compiler = shutil.which(args.cc) if args.cc else next(
        (path for name in ("clang", "gcc", "cc") if (path := shutil.which(name))), None)
    if not compiler:
        raise SystemExit("No C compiler found (pass --cc)")
    with tempfile.TemporaryDirectory(prefix="nvn-texture-handle-") as directory:
        path = Path(directory)
        source, binary = path / "fixture.c", path / "fixture.exe"
        source.write_text(fixture)
        subprocess.run([compiler, "-std=c11", "-O2", str(source), "-o", str(binary)], check=True)
        failed = []
        for case in range(13):
            result = subprocess.run([str(binary), str(case)], capture_output=True, text=True, timeout=5)
            if result.returncode:
                failed.append((case, result.returncode, result.stderr.strip()))
        if failed:
            raise SystemExit("FAIL: " + repr(failed))
    print("PASS: guard-house sentinel, empty handles, signed ids and 32-bit arguments (13 cases)")


if __name__ == "__main__":
    main()
