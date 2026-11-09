"""Build a64emit (suyu's AArch64 -> C instruction translator) as a shared library for a64_lift.py.

Usage:  python build_emit.py      -> a64emit.dll (Windows) / liba64emit.so (elsewhere), next to this file
Rebuilds only when a64emit.cpp or suyu/arm64_to_c.h changed.
"""
import hashlib, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import toolchain

SRC = [os.path.join(HERE, "a64emit.cpp"), os.path.join(HERE, "suyu", "arm64_to_c.h")]
OUT = os.path.join(HERE, "a64emit.dll" if os.name == "nt" else "liba64emit.so")


def build(force=False):
    key = hashlib.sha1(b"".join(open(f, "rb").read() for f in SRC)).hexdigest()
    stamp = OUT + ".sha1"
    if not force and os.path.exists(OUT) and os.path.exists(stamp) and open(stamp).read() == key:
        return OUT
    cmd = [toolchain.need(toolchain.find("clang++", host=True)), "-std=c++20", "-O2", "-shared", "-o", OUT, SRC[0],
           "-Wno-everything"] + toolchain.native_target(host=True) + toolchain.native_link(host=True)
    if os.name != "nt":
        cmd += ["-fPIC"]
    print("building", os.path.basename(OUT))
    subprocess.run(cmd, check=True)
    open(stamp, "w").write(key)
    return OUT


if __name__ == "__main__":
    print(build(force="--force" in sys.argv))
