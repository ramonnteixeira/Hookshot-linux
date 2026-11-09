"""No pool's mirror is made inside a parallel replay's segment.

gpu_buffer_bind makes a pool's mirror at its first binding: the page table points at the mirror at once, and its fill
is recorded into the uploads of the segment whose worker bound the pool first. A parallel replay (RX_VK_PARALLEL)
records its segments out of order, so an earlier segment, submitted first, read the mirror unfilled (houses, rocks
and bushes missing, geometry torn; #994). In a segment the mirror is wanted instead, made at the next submission's
start. This checks the first-binding condition in gpu/vk/vk_memory.cpp.
python -I -B tests/test_parallel_mirror_make.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    start = source.index("BufferRef gpu_buffer_bind(")
    body = source[start:source.index("\n}\n", start)]
    if not re.search(r"const bool par = par_active\.load\(", body):
        raise SystemExit("FAIL: gpu_buffer_bind no longer reads par_active into par")
    made = [line for line in body.splitlines() if "mirror_make(*p)" in line]
    if len(made) != 1:
        raise SystemExit(f"FAIL: expected one mirror_make at the first binding, found {len(made)}")
    if "!par" not in made[0]:
        raise SystemExit("FAIL: the first binding makes a mirror inside a parallel segment: " + made[0].strip())
    print("PASS: a pool's mirror is not made inside a parallel replay's segment")


if __name__ == "__main__":
    main()
