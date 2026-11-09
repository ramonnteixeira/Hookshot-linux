"""The shader census finds the game's compute programs: Effect/*.vsetb, the agl shader archives (.sharcb), agl's networks
(NDSR) and where they are.

Without them the seed had no compute program at all, and each was translated and built the first time the game met it,
2-18 s with the picture frozen on an RX 7700S. Synthetic data only, no game files. python -I -B tests/test_shader_census.py
"""
import os
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import shader_census as sc  # noqa: E402

CONTROL_SIZE = 0xD8


def control(stage, end=0x100):
    c = bytearray(CONTROL_SIZE)
    c[0:4] = sc.CONTROL_MAGIC
    struct.pack_into("<I", c, 0x38, 0x40)
    struct.pack_into("<II", c, 0x40, end, end)
    struct.pack_into("<H", c, 0x56, stage)
    return c


def code(seed, end=0x100):
    return bytes((seed + i) & 0xFF for i in range(end))


def sharc():
    """A BAHS blob: a vertex + fragment program and a compute program, each stage pointing at its control section and code"""
    base, blob, size = 0x100, 0x2000, 0x3000
    data = bytearray(size)
    data[0:4] = b"BAHS"
    struct.pack_into("<III", data, 0x14, base, size - blob, blob)
    stages = [(0, 0x1000, 0x000), (1, 0x1100, 0x100), (5, 0x1200, 0x200)]   # (stage, its control section, its code in the blob)
    at = base + 0x158
    for k, (stage, ctl, off) in enumerate(stages):
        struct.pack_into("<IIIIQIi", data, at, 0x30, (0, 1, 3)[k], 0, 0, off, 0xE0, ctl - (at + 16))
        data[ctl:ctl + CONTROL_SIZE] = control(stage)
        data[blob + off:blob + off + 0x100] = code(k)
        at += 0x30
    table = at
    struct.pack_into("<II", data, base + 0x150, table - (base + 0x150), len(stages))
    struct.pack_into("<II", data, table, 0x30, 2)
    struct.pack_into("<IIII", data, table + 8, 0x14, 0, 0b11, 0)    # vertex + fragment, from stage 0
    struct.pack_into("<IIII", data, table + 0x1C, 0x14, 0, 0b1, 2)  # compute, stage 2
    return bytes(data), blob


def check_sharc():
    data, blob = sharc()
    found = [[s[0] for s in p] for p in sc.bahs_programs(data)]
    if found != [[0, 1], [5]]:
        raise SystemExit("expected a vertex + fragment and a compute program, got %r" % found)
    stage, ctl, cs = list(sc.bahs_programs(data))[1][0]
    if ctl != data[0x1200:0x1200 + CONTROL_SIZE] or cs != data[blob + 0x200:blob + 0x300]:
        raise SystemExit("the compute program's control section or code is not the archive's")


def check_ndsr():
    data = bytearray(0x600)
    data[0:4] = b"NDSR"
    struct.pack_into("<I", data, 0x0C, 2)
    struct.pack_into("<IIII", data, 0x14, 0x100, CONTROL_SIZE << 4 | 6, 0x200, 0x100 << 4 | 7)
    data[0x100:0x100 + CONTROL_SIZE] = control(5)
    data[0x200:0x300] = code(9)
    found = list(sc.ndsr_programs(bytes(data), 0))
    if len(found) != 1 or found[0][0][0] != 5 or found[0][0][2] != bytes(data[0x200:0x300]):
        raise SystemExit("expected the network's compute program, got %r" % [[s[0] for s in p] for p in found])


def check_archives():
    with tempfile.TemporaryDirectory() as romfs:
        for folder, name in (("Shader", "Model.Product.bfsha.zs"), ("Shader", "ApplicationPackage.release.sarc.zs"),
                             ("Effect", "Animal_Cow.Nin_Ounce_NVN2.vsetb.byml.zs"), ("Effect", "notes.txt"),
                             ("Lib/agl", "agl_resource.Nin_Ounce_NVN2.release.sarc.zs"), ("Movie/Ndve", "Ndve-Nvn2-vid_seq5.dat.zs"),
                             ("Layout2Shader", "MergeShader.bfsha.zs")):
            os.makedirs(os.path.join(romfs, folder), exist_ok=True)
            open(os.path.join(romfs, folder, name), "wb").close()
        names = sorted(os.path.basename(p) for p in sc.archives(romfs))
    want = sorted(["Model.Product.bfsha.zs", "ApplicationPackage.release.sarc.zs", "Animal_Cow.Nin_Ounce_NVN2.vsetb.byml.zs",
                   "agl_resource.Nin_Ounce_NVN2.release.sarc.zs", "Ndve-Nvn2-vid_seq5.dat.zs", "MergeShader.bfsha.zs"])
    if names != want:
        raise SystemExit("archives: %r, expected %r" % (names, want))


def check_main_reads_them():
    source = (ROOT / "nx2" / "shader_census.py").read_text(encoding="utf-8")
    if "programs_in(data, stats)" not in source or 'data[:4] == b"BAHS"' not in source or 'data[:4] == b"NDVE"' not in source:
        raise SystemExit("main() does not hand BAHS, NDSR and NDVE containers to their readers")


def main():
    check_sharc()
    check_ndsr()
    check_archives()
    check_main_reads_them()
    print("shader census: programs from .vsetb, .sharcb and agl's networks ok")


if __name__ == "__main__":
    main()
