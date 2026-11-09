"""NVIDIA DLSS's camera math (gpu/vk/dlss_math.h): the jitter's Halton sequence, the reprojection that makes the motion
vectors, the jitter taken off the motion, the jitter in DLSS's pixels and which projections get it. Built as C++ and run;
the header is also built as C, as host/rx_backend.c includes it. No game, GPU or NVIDIA SDK required.

python -I -B tests/test_dlss_math.py [--cxx g++] [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dlss-math-") as tmp:
        c = Path(tmp) / "c.c"
        c.write_text('#include "%s"\nint main(void) { float j[2]; dlss_jitter_pixels(3, 16, j); return j[0] < 0.5f ? 0 : 1; }\n'
                     % (ROOT / "gpu" / "vk" / "dlss_math.h").as_posix())
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(Path(tmp) / "c")], check=True)
        subprocess.run([str(Path(tmp) / "c")], check=True)
        exe = Path(tmp) / "dlss-math"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_dlss_math.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
