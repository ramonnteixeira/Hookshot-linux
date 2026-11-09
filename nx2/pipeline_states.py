"""The pipeline state lists (gpu/vk/vk_draw.cpp: hookshot_pipeline_states.bin, written as the game is played): show one, or merge
several into the list a release ships (work/pipeline_states.bin, copied by nx2/make_game.py beside the game as
hookshot_pipeline_states_shipped.bin, whose pipelines the first start's shader preparation builds before the title).

    python -I nx2/pipeline_states.py <list>...                    what they hold
    python -I nx2/pipeline_states.py --out work/pipeline_states.bin <list>...   merged, each pipeline once

A list holds no game code: each record is a pipeline's stage code keys (hashes of the game's shaders, which the player's own
hookshot_shaders.bin turns back into the shaders), the program variant's hash, and the formats and draw state baked into the pipeline.

    <list>: "HKSTATE1", u32 record size (528), then the records:
            u64 code[4] (vertex or mesh, fragment, task, the header-only stage), u64 variant,
            u32 mesh, topology, targets, samples, depth format, has depth, u32 color formats[8], u8 color types[8],
            u64 blend[8], u32 vertex attributes[16], u32 attribute count, u32 strides[16], u32 divisors[16],
            u32 blend enables, channel mask, polygon mode, depth clamp, alpha to coverage, pad,
            u32 answers, u32 questions, u32 answers[24] (stage, descriptor, image, type), u8 questions[32] (each answer as asked)
"""
import argparse
import collections
import struct
import sys

MAGIC = b"HKSTATE1"
SIZE = 528


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != MAGIC or struct.unpack_from("<I", data, 8)[0] != SIZE:
        raise SystemExit("%s: not a pipeline state list" % path)
    body = data[12:]
    return [body[i:i + SIZE] for i in range(0, len(body) - SIZE + 1, SIZE)]


def describe(recs):
    programs = collections.Counter()
    kinds = collections.Counter()
    for r in recs:
        code = struct.unpack_from("<4Q", r, 0)
        mesh = struct.unpack_from("<I", r, 40)[0]
        programs[(code, mesh)] += 1
        kinds["mesh" if mesh else "graphics"] += 1
    return "%d pipelines (%s) of %d programs" % (len(recs), ", ".join("%s %d" % kv for kv in sorted(kinds.items())), len(programs))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("lists", nargs="+")
    p.add_argument("--out")
    a = p.parse_args()
    merged, seen = [], set()
    for path in a.lists:
        recs = read(path)
        print("%s: %s" % (path, describe(recs)))
        for r in recs:
            if r not in seen:
                seen.add(r)
                merged.append(r)
    if a.out:
        with open(a.out, "wb") as f:
            f.write(MAGIC + struct.pack("<I", SIZE) + b"".join(merged))
        print("%s: %s" % (a.out, describe(merged)))


if __name__ == "__main__":
    sys.exit(main())
