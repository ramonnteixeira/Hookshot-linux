"""Every shader program in the game's archives, for the precompile before the first start.

    python -I nx2/shader_census.py <romfs> [--out <file>]

Walks romfs/Shader/*.bfsha.zs (and the shader packages inside other archives), and each BNSH inside them. A BNSH's variations
are programs: which stages go together, so what comes out are real pairs, not a heap of stages. --out writes them for the back
end to translate (gpu/vk/vk_shader.cpp); nothing here goes into the repository: the file is made from the player's own game.

The game's compute programs, and many of its graphics ones, are not in a BNSH: agl's shader archives (BAHS, in Lib/ and in
Shader/ApplicationPackage), agl's networks (NDSR, Lib/agl), the movie upscaler's (NDVE, Movie/Ndve) and the particle sets
(Effect/*.vsetb.byml.zs, each a BNSH inside a byml) are walked too, each in its own way (below). A stage is its control section
(0xd8 bytes) and the code area it sizes, the bytes the game hands the GPU: those are what the back end keys by.

After tools/shader_census.py of TotK Recomp (github.com/valentderah/totk-pc-recomp, MIT), itself after MacNativeGames'
prepare_shader_seed.py (MIT): the BNSH layout and the walk of its variations. BAHS, NDSR and NDVE were read from the stages a
play run dumped (RX_VK_DUMP_STAGES), after the first reading of BAHS (the code offset 0x68 bytes before a control section,
where the control section follows its own stage record).

    <out>:  "HKSEED01", u32 programs, u32 stages; then the stages, each once:
            u64 key, u32 stage, u32 control bytes, u32 code bytes, control, code
            then the programs: u32 count, u32 stage indices
"""
import argparse
import collections
import hashlib
import os
import re
import struct
import sys
import time

CONTROL_MAGIC = b"\x34\x12\x76\x98"
ZSTD = b"\x28\xb5\x2f\xfd"
# the stages a variation's program points at (a 0 offset: none), as far as the structure reaches
SLOTS = (8, 16, 24, 32, 40, 48, 56, 64, 72)


def dictionaries(romfs, zs):
    """The game's zstd dictionaries: the files of a SARC, Pack/ZsDic.pack.zs"""
    raw = zs.ZstdDecompressor().decompress(open(os.path.join(romfs, "Pack", "ZsDic.pack.zs"), "rb").read(), max_output_size=1 << 28)
    start = struct.unpack_from("<I", raw, 0x0c)[0]
    out = []
    for k in range(struct.unpack_from("<H", raw, 0x1a)[0]):
        _, _, st, end = struct.unpack_from("<IIII", raw, 0x20 + k * 16)
        out.append(raw[start + st:start + end])
    return out


def unpack(data, dicts, zs):
    if data[:4] != ZSTD:
        return data
    try:
        return zs.ZstdDecompressor().decompress(data, max_output_size=1 << 31)
    except zs.ZstdError:
        pass
    for d in dicts:
        try:
            return zs.ZstdDecompressor(dict_data=zs.ZstdCompressionDict(d, dict_type=zs.DICT_TYPE_FULLDICT)).decompress(
                data, max_output_size=1 << 31)
        except zs.ZstdError:
            continue
    raise ValueError("no dictionary fits")


def bnsh_programs(data, stats):
    """Each variation of a BNSH (its offsets are its own) as [(stage, control, code)...]"""
    if data[:4] != b"BNSH" or data[12:14] != b"\xff\xfe" or data[96:100] != b"grsc":
        raise ValueError("unsupported BNSH header")
    size = struct.unpack_from("<I", data, 28)[0]
    data = data[:size]
    count, variations = struct.unpack_from("<IQ", data, 124)
    for i in range(count):
        program = struct.unpack_from("<Q", data, variations + i * 64 + 16)[0]
        if not program or program + 80 > size:
            stats["no program"] += 1
            continue
        stages = []
        for rel in SLOTS:
            offset = struct.unpack_from("<Q", data, program + rel)[0]
            if not offset or offset + 32 > size:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from("<QQII", data, offset + 8)
            if not 0x60 <= control_size <= 0x1000 or control_off + control_size > size or code_off + code_size > size:
                continue
            control = data[control_off:control_off + control_size]
            if control[:4] != CONTROL_MAGIC:
                continue
            stage = struct.unpack_from("<H", control, 0x56)[0]
            stats["slot %d, stage %d" % (rel, stage)] += 1
            stages.append((stage, control, data[code_off:code_off + code_size]))
        if stages:
            yield stages


def control_at(data, c):
    """The 0xd8-byte NVN2 control section at c, and its code area's size (+0x44), or None"""
    control = data[c:c + 0xd8]
    if len(control) < 0xd8 or control[:4] != CONTROL_MAGIC or struct.unpack_from("<H", control, 0x56)[0] > 7:
        return None, 0
    return control, struct.unpack_from("<I", control, 0x44)[0]


def stage(control, code):
    return struct.unpack_from("<H", control, 0x56)[0], control, code


def bahs_programs(data):
    """agl's shader archive (.sharcb, magic BAHS: gsys, agl, terrain and ApplicationPackage's AglShader / FoliageShader).

    Header +0x14 u32 the data start, +0x18 the code blob's size, +0x1c its start. At data +0x150: u32 the program table (from
    there), u32 the stage count; the stages follow from data +0x158, each {u32 bytes to the next, u32 kind (0 vertex, 1 fragment,
    3 compute, 4 task, 5 mesh), u32, u32, u64 its code area's offset in the blob, u32 control bytes, s32 its control section
    from there}. Stages share control sections (one control, several codes). The program table: u32 bytes, u32 programs, each
    {u32 bytes, u32 name bytes, u32 its kinds (a bit each), u32 its first stage, name, options}; a program's variations are its
    run of stages up to the next program's first, one stage of each of its kinds per variation."""
    base, _, blob = struct.unpack_from("<III", data, 0x14)
    table, count = struct.unpack_from("<II", data, base + 0x150)
    at, stages = base + 0x158, []
    for _ in range(count):
        step = struct.unpack_from("<I", data, at)[0]
        if step < 32:
            raise ValueError("stage at %#x" % at)
        offset, _, rel = struct.unpack_from("<QIi", data, at + 16)
        control, end = control_at(data, at + 16 + rel)
        if control is None or blob + offset + end > len(data):
            raise ValueError("stage at %#x" % at)
        stages.append(stage(control, data[blob + offset:blob + offset + end]))
        at += step
    table += base + 0x150
    at, heads = table + 8, []
    for _ in range(struct.unpack_from("<I", data, table + 4)[0]):
        size, _, kinds, first = struct.unpack_from("<4I", data, at)
        heads.append((first, bin(kinds).count("1")))
        at += size
    heads.sort()
    for i, (first, width) in enumerate(heads):
        last = heads[i + 1][0] if i + 1 < len(heads) else count
        for v in range(first, last, max(width, 1)):
            yield stages[v:v + width]


def ndsr_programs(data, base):
    """agl's networks (agl_ndsr*.pck, magic NDSR; also inside NDVE at +0x28): u32 +0x0c the item count, from +0x14 each item
    {u32 offset from the NDSR, u32 bytes << 4 | type}. Types 0 / 3 / 6 are a vertex / fragment / compute control section, the item
    after each (1 / 4 / 7) its code; a vertex and the fragment after it are a program, a compute stage is one alone."""
    items = [struct.unpack_from("<II", data, base + 0x14 + 8 * i) for i in range(struct.unpack_from("<I", data, base + 0x0c)[0])]
    graphics = []
    for (c, ct), (x, xt) in zip(items, items[1:]):
        if ct & 15 not in (0, 3, 6) or xt & 15 != (ct & 15) + 1:
            continue
        control, end = control_at(data, base + c)
        if control is None or base + x + end > len(data):
            continue
        s = stage(control, data[base + x:base + x + end])
        if ct & 15 == 6:
            yield [s]
        elif ct & 15 == 0:
            if graphics:
                yield graphics
            graphics = [s]
        elif graphics:
            graphics.append(s)
    if graphics:
        yield graphics


def ndve_programs(data):
    """The movie upscaler's networks (Movie/Ndve, magic NDVE): an NDSR at +0x28, then tables {u32 stages, u32 blobs, blobs x {u32
    offset, u32 bytes}, stages x {u32 control section from the table, u32 code from the control, u32 code bytes, u32 0}}. Each
    control's entry is found from its code bytes (the pair "bytes, 0") and checked against its table's header."""
    if data[0x28:0x2c] == b"NDSR":
        yield from ndsr_programs(data, 0x28)
    for m in re.finditer(re.escape(CONTROL_MAGIC), data):
        c = m.start()
        control, end = control_at(data, c)
        if control is None:
            continue
        for e in re.finditer(re.escape(struct.pack("<II", end, 0)), data):
            entry = e.start() - 8
            if entry < 0 or entry % 4:
                continue
            to_control, to_code = struct.unpack_from("<II", data, entry)
            table = c - to_control
            if not to_control or table < 0 or table % 4 or c + to_code + end > len(data):
                continue
            n, blobs = struct.unpack_from("<II", data, table)
            first = table + 8 + 8 * blobs
            if 0 < n < 256 and blobs < 16 and (entry - first) % 16 == 0 and 0 <= (entry - first) // 16 < n:
                yield [stage(control, data[c + to_code:c + to_code + end])]
                break


def bnsh_all(data, stats):
    """Each BNSH inside the data (bfsha, and Effect's particle sets: a BNSH inside a byml)"""
    for m in re.finditer(b"BNSH", data):
        if data[m.start() + 96:m.start() + 100] == b"grsc":
            yield from bnsh_programs(data[m.start():m.start() + struct.unpack_from("<I", data, m.start() + 28)[0]], stats)


def programs_in(data, stats):
    if data[:4] == b"BAHS":
        return bahs_programs(data)
    if data[:4] == b"NDSR":
        return ndsr_programs(data, 0)
    if data[:4] == b"NDVE":
        return ndve_programs(data)
    return bnsh_all(data, stats)


def archives(romfs):
    """The files that hold shaders: Shader/* (bfsha programs; ApplicationPackage's agl archives), Layout2Shader/*, Lib/** (agl,
    gsys and terrain archives, agl's networks), Movie/** (the upscaler's networks), Effect/* (particle sets)"""
    for folder in ("Shader", "Layout2Shader", "Lib", "Movie", "Effect"):
        for top, dirs, names in os.walk(os.path.join(romfs, folder)):
            dirs.sort()
            for name in sorted(names):
                if name.endswith(".zs") or ".bfsha" in name or ".sarc" in name or ".pck" in name:
                    yield os.path.join(top, name)


def sarc_files(data):
    start = struct.unpack_from("<I", data, 0x0c)[0]
    count = struct.unpack_from("<H", data, 0x1a)[0]
    for k in range(count):
        _, _, st, end = struct.unpack_from("<IIII", data, 0x20 + k * 16)
        yield data[start + st:start + end]


def blobs(data, dicts, zs):
    """The data itself, or each file when it is an archive"""
    if data[:4] == b"SARC":
        for inner in sarc_files(data):
            yield from blobs(unpack(inner, dicts, zs), dicts, zs)
    else:
        yield data


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("romfs")
    p.add_argument("--out")
    a = p.parse_args()
    import zstandard as zs
    dicts = dictionaries(a.romfs, zs)
    stats = collections.Counter()
    stage_index, stage_list, programs, seen = {}, [], [], set()
    started = time.time()
    effect = collections.Counter()
    for path in archives(a.romfs):
        made, found = 0, []
        try:   # (a file or container this doesn't read is told and skipped)
            for k, data in enumerate(blobs(unpack(open(path, "rb").read(), dicts, zs), dicts, zs)):
                try:
                    found += list(programs_in(data, stats))
                except Exception as e:
                    print("%-64s container %d %r skipped: %s" % (os.path.basename(path)[:64], k, data[:4], e), flush=True)
        except Exception as e:
            print("%-64s skipped: %s" % (os.path.basename(path)[:64], e), flush=True)
        for stages in found:
            ids = []
            for stage, control, code in stages:
                # (what the back end reads of a stage: the 0xd8 control bytes and the code area they size, so the same stage
                # from two archives is one)
                end = struct.unpack_from("<I", control, 0x44)[0]
                control, code = control[:0xd8], code[:end] if end <= len(code) else code
                key = int.from_bytes(hashlib.sha1(control + code).digest()[:8], "little")
                if key not in stage_index:
                    stage_index[key] = len(stage_list)
                    stage_list.append((key, stage, control, code))
                ids.append(stage_index[key])
            ids = tuple(sorted(set(ids)))
            if ids not in seen:
                seen.add(ids)
                programs.append(ids)
                made += 1
        if os.path.basename(os.path.dirname(path)) == "Effect":   # (1400 particle sets: one line for them all)
            effect["files"] += 1
            effect["programs"] += made
        else:
            print("%-64s %6d programs" % (os.path.basename(path)[:64], made), flush=True)
    print("%-64s %6d programs" % ("Effect (%d particle sets)" % effect["files"], effect["programs"]))
    print("%.0f s" % (time.time() - started))
    kinds = collections.Counter(tuple(sorted(stage_list[i][1] for i in ids)) for ids in programs)
    print("%d programs from %d stages" % (len(programs), len(stage_list)))
    print("by stages:", kinds.most_common(12))
    print("slots:", sorted(stats.items()))
    if a.out:
        with open(a.out, "wb") as f:
            f.write(b"HKSEED01" + struct.pack("<II", len(programs), len(stage_list)))
            for key, stage, control, code in stage_list:
                f.write(struct.pack("<QIII", key, stage, len(control), len(code)) + control + code)
            for ids in programs:
                f.write(struct.pack("<I%dI" % len(ids), len(ids), *ids))
        print("wrote", a.out, os.path.getsize(a.out) >> 20, "MB")


if __name__ == "__main__":
    sys.exit(main())
