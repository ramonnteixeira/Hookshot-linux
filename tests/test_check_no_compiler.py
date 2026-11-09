"""--check says a compiler is missing, not "The game files could not be read".

The check tests the compiler before it opens the package. toolchain.need() raises SystemExit, which make_game.py
re-raised without an @@error line; the builder app then showed its fallback for the game files, for any .nup.
python -I -B tests/test_check_no_compiler.py
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory() as d:
        env = dict(os.environ)
        env["PATH"] = d                   # (no clang on PATH)
        env["EMSDK"] = d                  # (an emsdk folder that holds none)
        env["HOOKSHOT_MSVC"] = "1"        # (the Visual Studio route: no tools/llvm-mingw either)
        env.pop("HOOKSHOT_MINGW", None)
        r = subprocess.run([sys.executable, "-I", "-B", str(ROOT / "nx2" / "make_game.py"), "--check", "--nup",
                            str(ROOT / "no-such-game.nup"), "--work", str(Path(d) / "work")],
                           capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    said = [line for line in r.stdout.splitlines() if line.startswith("@@")]
    if not said:
        raise SystemExit("the check left stdout empty: the builder shows its 'game files could not be read' fallback"
                         " (exit %d, stderr %r)" % (r.returncode, r.stderr.strip()))
    if not said[0].startswith("@@error ") or "compiler" not in said[0].lower():
        raise SystemExit("the check did not name the missing compiler: %r" % said[0])
    print("PASS: a missing compiler is said in the builder's protocol")


if __name__ == "__main__":
    main()
