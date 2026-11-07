"""(linux port) Compile-check the host runtime on Linux without a game: each host/*.c against the generated guest
model (nx2/rtgen.py), with the flags nx2/build.py uses. Nothing is linked; this only tracks what still needs porting.

Usage:  python3 -I scripts/linux_check.py [file.c ...] [-v]
"""
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


def main():
    verbose = "-v" in sys.argv
    want = [a for a in sys.argv[1:] if a.endswith(".c")]
    rt64 = rtgen.write(os.path.join(tempfile.gettempdir(), "hookshot_rt64"))
    # (gen/ is the lift's output; host code only needs the guest model's generated headers from it, written here the
    # way a64_lift.py writes them, with no FPX helpers in use)
    gen = os.path.join(tempfile.gettempdir(), "hookshot_gen")
    a64_rtgen.write(gen)
    open(os.path.join(gen, "a64_fpx.h"), "w", newline="\n").write(a64_lift.fpx_header(set()))
    flags = ["-fsyntax-only", "-std=gnu11", "-D_GNU_SOURCE", "-march=x86-64-v3", "-mcx16", "-DA64_POLLING", "-DA64_WRITE_TRACK",
             "-I" + HOST, "-I" + rt64, "-I" + gen, "-I" + OPUS, "-Wno-everything", "-ferror-limit=0"]
    files = sorted(f for f in os.listdir(HOST) if f.endswith(".c") and (not want or f in want))
    ok = 0
    for f in files:
        r = subprocess.run(["clang"] + flags + [os.path.join(HOST, f)], capture_output=True, text=True)
        errs = [l for l in r.stderr.splitlines() if " error: " in l]
        if r.returncode == 0:
            ok += 1
            print("  ok    %s" % f)
        else:
            print("  FAIL  %-18s %d errors" % (f, len(errs)))
            for l in errs[:None if verbose else 3]:
                print("        " + l.split("host/")[-1])
    print("%d/%d host files compile on Linux" % (ok, len(files)))
    return 0 if ok == len(files) else 1


if __name__ == "__main__":
    sys.exit(main())
