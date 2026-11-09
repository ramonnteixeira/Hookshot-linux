"""A lift that writes no code_hot.c leaves none of an earlier lift's hot pieces (code_hot_*.c, code_hot.c.whole) in the gen folder:
build.py compiles every code_*.c, and those pieces defined again the functions the new lift put in its chunks. The lift is
driven through chunks.write and the lifter's own write() and split_hot, with made-up functions in place of the game's.

python -I -B tests/test_stale_hot_pieces.py
"""
import collections
import contextlib
import io
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path)
import a64_lift  # noqa: E402
import chunks  # noqa: E402
import lifted_c  # noqa: E402
import split_hot  # noqa: E402

a64_lift.a64emit.translate = lambda keys: ([], [])   # (no instructions to translate: the native translator is not built)


class Lift:
    """What chunks.write and Lifter.write use of a lifter: the functions, emitted as plain C"""
    write = a64_lift.Lifter.write
    _write_table = a64_lift.Lifter._write_table
    fname = a64_lift.Lifter.fname

    def __init__(self, addrs):
        self.funcs = {a: ({}, None) for a in addrs}
        self.hooks = {}
        self.stats, self.unhandled, self.ijump = collections.Counter(), collections.Counter(), collections.Counter()

    def emit_function(self, e, texts):
        return "void f_%08x(Ctx *__restrict A64_NOESC c) {\n" % e + "c->x[1] = 1;\n" * 40 + "}\n", {"f_%08x" % (e + 0x10)}


def check(what, ok):
    if not ok:
        raise SystemExit("FAIL: " + what)


def lift(work, addrs, hot):
    """The lift as make_game.py runs it: chunks.write, then split_hot when there is a code_hot.c"""
    gen, img = os.path.join(work, "gen"), os.path.join(work, "image")
    os.makedirs(img, exist_ok=True)
    os.environ["A64_HOT_CHUNK"] = "1"
    if hot is None:
        os.environ.pop("A64_HOT_FUNCS", None)
    else:
        path = os.path.join(work, "hot_funcs.txt")
        Path(path).write_text("".join("f_%08x\n" % a for a in hot))
        os.environ["A64_HOT_FUNCS"] = path
    with contextlib.redirect_stdout(io.StringIO()):
        chunks.write(Lift(addrs), gen, img, rechunk=True, jobs=1)
        if os.path.isfile(os.path.join(gen, "code_hot.c")):
            split_hot.split(gen, 4)
    defined = collections.Counter()
    for name in os.listdir(gen):
        if name.startswith("code_") and name.endswith(".c"):   # (what build.py compiles)
            with open(os.path.join(gen, name), newline="") as f:
                defined.update(m.group(1) for m in lifted_c.HEAD.finditer(f.read()))
    return gen, defined


def main():
    a64_lift.CHUNK_BYTES = 4000
    addrs = [0x80000000 + 0x40 * i for i in range(120)]
    want = {"f_%08x" % a for a in addrs}
    with tempfile.TemporaryDirectory(prefix="hookshot-hot-pieces-") as work:
        gen, defined = lift(work, addrs, addrs[::7])
        check("the hot functions are in pieces", any(n.startswith("code_hot_") for n in os.listdir(gen)))
        check("a lift with a hot list defines each function once", set(defined) == want and max(defined.values()) == 1)
        for what, hot in (("no hot list", None), ("a hot list none of whose functions are lifted", [0x90000000])):
            lift(work, addrs, addrs[::7])
            gen, defined = lift(work, addrs, hot)
            left = sorted(n for n in os.listdir(gen) if n.startswith("code_hot"))
            check("%s: no hot pieces of the earlier lift are left (%s)" % (what, ", ".join(left)), not left)
            check("%s: each function is defined once" % what, set(defined) == want and max(defined.values()) == 1)
        gen, defined = lift(work, addrs, addrs[::5])
        check("a hot list again: each function is defined once", set(defined) == want and max(defined.values()) == 1)
    print("ok: a lift without code_hot.c leaves no hot pieces")


if __name__ == "__main__":
    main()
