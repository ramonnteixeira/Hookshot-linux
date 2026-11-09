"""A chunk of giant functions is cut into parts of about equal compile cost, each part a C file of its own with the chunk's
declarations; the functions, in order, are the chunk's. A chunk of ordinary functions stays whole.

python -I -B tests/test_chunk_split.py
"""
import os
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path)
import chunks  # noqa: E402

PREAMBLE = '#include "a64.h"\nvoid f_90000000(Ctx *A64_NOESC);\n'


def function(addr, size):
    head = "void f_%08x(Ctx *__restrict A64_NOESC c) {\n" % addr
    line = "c->x[1] = c->x[2] + 1;\n"
    return head + line * max(1, (size - len(head) - 2) // len(line)) + "}\n"


def check(what, ok):
    if not ok:
        raise SystemExit("FAIL: " + what)


def main():
    budget = 200000
    with tempfile.TemporaryDirectory(prefix="hookshot-chunk-split-") as d:
        small = [function(0x80000000 + 16 * i, 2000) for i in range(60)]
        big = [function(0x81000000 + 16 * i, 2000) for i in range(40)] + [function(0x81100000, 300000)] + \
              [function(0x81200000 + 16 * i, 2000) for i in range(200)]
        files = []
        for n, fns in enumerate((small, big)):
            path = os.path.join(d, "code_%03d.c" % n)
            Path(path).write_text(PREAMBLE + "".join(fns), newline=chr(10))
            files.append(path)
        Path(d, "code_001_9.c").write_text("stale")
        made = chunks.split_big(d, files, budget=budget)
        check("the small chunk stays whole", not any("code_000_" in m for m in made))
        check("a stale part of an earlier lift is removed", not Path(d, "code_001_9.c").exists())
        parts = [files[1]] + sorted(made, key=lambda p: int(re.search(r"_(\d+)\.c$", p).group(1)))
        check("the giant chunk is cut", len(parts) >= 2)
        texts = []
        for p in parts:   # (read as written, with no newline translation: Path.read_text(newline=) is Python 3.13's)
            with open(p, newline="") as f:
                texts.append(f.read())
        check("every part has the chunk's declarations", all(t.startswith(PREAMBLE) for t in texts))
        check("the parts hold the chunk's functions in order", "".join(t[len(PREAMBLE):] for t in texts) == "".join(big))
        check("no part is cut inside a function", all(t[len(PREAMBLE):].startswith("void f_") for t in texts))
        giant = next(i for i, t in enumerate(texts) if "f_81100000(" in t)
        check("the parts after the giant function are not slivers",
              all(chunks.text_cost(t) > budget // 4 for t in texts[giant + 1:]))
        costs = dict(l.split() for l in Path(d, chunks.COSTS).read_text().splitlines())
        check("every chunk and part has a cost", set(costs) == {os.path.basename(p) for p in [files[0]] + parts})
        check("a part's cost is its estimate", all(int(costs[os.path.basename(p)]) == chunks.text_cost(t) for p, t in zip(parts, texts)))
    print("ok: %d parts, the functions as they were" % len(parts))


if __name__ == "__main__":
    main()
