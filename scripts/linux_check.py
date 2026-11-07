"""(linux port) Compile-check on Linux without a game, with the flags nx2/build.py uses. Nothing is linked; this tracks
what still needs porting.

  host:  each host/*.c against the generated guest model (nx2/rtgen.py, a64-kit's runtime headers)
  --vk:  each source of the Vulkan back end (gpu/vk, Eden's recompiler, sirit), with the Vulkan and SPIR-V headers
         from the system (/usr/include) or VULKAN_HEADERS=<Vulkan-Headers checkout>, SPIRV_HEADERS=<SPIRV-Headers
         checkout>; RX_VMA=<VulkanMemoryAllocator checkout> when vk_mem_alloc.h is not in /usr/include

Usage:  python3 -I scripts/linux_check.py [file.c ...] [-v]
        python3 -I scripts/linux_check.py --vk [name-substring ...] [-v] [--jobs N]
"""
import concurrent.futures
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "lift"))
import rtgen  # noqa: E402
import a64_rtgen  # noqa: E402
import a64_lift  # noqa: E402

HOST = os.path.join(ROOT, "host")
OPUS = os.path.join(ROOT, "third_party", "opus", "include")


def report(results, verbose, what):
    ok = 0
    for name, err in results:
        if err is None:
            ok += 1
            if verbose:
                print("  ok    %s" % name)
            continue
        errs = [l for l in err.splitlines() if " error: " in l]
        print("  FAIL  %-28s %d errors" % (name, len(errs)))
        for l in errs[:None if verbose else 3]:
            print("        " + l.replace(ROOT + os.sep, ""))
    print("%d/%d %s compile on Linux" % (ok, len(results), what))
    return 0 if ok == len(results) else 1


def check_host(verbose, want):
    rt64 = rtgen.write(os.path.join(tempfile.gettempdir(), "hookshot_rt64"))
    # (gen/ is the lift's output; host code only needs the guest model's generated headers from it, written here the
    # way a64_lift.py writes them, with no FPX helpers in use)
    gen = os.path.join(tempfile.gettempdir(), "hookshot_gen")
    a64_rtgen.write(gen)
    open(os.path.join(gen, "a64_fpx.h"), "w", newline="\n").write(a64_lift.fpx_header(set()))
    flags = ["-fsyntax-only", "-std=gnu11", "-D_GNU_SOURCE", "-DRX_NO_WEBGPU", "-march=x86-64-v3", "-mcx16",
             "-DA64_POLLING", "-DA64_WRITE_TRACK", "-I" + HOST, "-I" + rt64, "-I" + gen, "-I" + OPUS, "-Wno-everything",
             "-ferror-limit=0"]
    files = sorted(f for f in os.listdir(HOST) if f.endswith(".c") and (not want or f in want))
    results = []
    for f in files:
        r = subprocess.run(["clang"] + flags + [os.path.join(HOST, f)], capture_output=True, text=True)
        results.append((f, None if r.returncode == 0 else r.stderr))
    return report(results, True, "host files")


def check_vk(verbose, want, jobs):
    import build
    vk_headers = os.environ.get("VULKAN_HEADERS") or "/usr"
    spirv_headers = os.environ.get("SPIRV_HEADERS") or "/usr"
    if not os.path.isfile(os.path.join(vk_headers, "include", "vulkan", "vulkan.h")):
        print("no Vulkan headers (install vulkan-headers, or set VULKAN_HEADERS)")
        return 2
    flags = ["-fsyntax-only", "-ferror-limit=0"] + [f for f in build.vk_flags("0", vk_headers, spirv_headers)
                                                    if f not in ("-O2", "-g0")]
    srcs = [s for s in build.vk_sources() if not want or any(w in s for w in want)]

    def one(src):
        r = subprocess.run(["clang++"] + flags + [src], capture_output=True, text=True)
        return os.path.relpath(src, ROOT), None if r.returncode == 0 else r.stderr

    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        results = sorted(ex.map(one, srcs))
    return report(results, verbose, "Vulkan back end files")


def main():
    args = sys.argv[1:]
    verbose = "-v" in args
    jobs = os.cpu_count() or 2
    if "--jobs" in args:
        jobs = int(args[args.index("--jobs") + 1])
    rest = [a for i, a in enumerate(args) if not a.startswith("-") and (i == 0 or args[i - 1] != "--jobs")]
    if "--vk" in args:
        return check_vk(verbose, rest, jobs)
    return check_host(verbose, [a for a in rest if a.endswith(".c")])


if __name__ == "__main__":
    sys.exit(main())
