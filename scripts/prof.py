"""Offline profile of a thread sampled with HOS_SAMPLE + HOS_SAMPLE_FILE (host/mkw_glue.c, mkw-recomp work/prof.py): each line is a stack of
module+offset frames, leaf first. Frames in hookshot_vk.dll / hookshot.exe are named from the linker maps beside them.

    python -I scripts/prof.py work/runs/<run>/stacks.txt [top] [skip] [only]
    (skip: the first fraction of the samples, e.g. 0.5; only: samples with that function in their stack, e.g.
    bench_replay)

Prints the functions by inclusive share (in the stack at all) and by self share (the leaf), and the leaf modules.
"""
import bisect, collections, itertools, os, re, sys

BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "work", "sa", "build")


def load_map(path, base):
    syms = []
    pat = re.compile(r"^\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s", re.I)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pat.match(line)
            if m:
                syms.append((int(m.group(2), 16) - base, m.group(1)))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def demangle(n):
    # ?name@ns@@... -> ns::name (enough to read)
    m = re.match(r"\?([^@]+)@([^@]*)@", n)
    if m and not n.startswith("??"):
        return "%s::%s" % (m.group(2), m.group(1)) if m.group(2) else m.group(1)
    m = re.match(r"\?\?\$?([^@]+)@", n)
    return m.group(1) if m else n


def profile_lines(path, skip):
    """Keep the old line-based slice without holding the capture in memory."""
    with open(path, encoding="utf-8", errors="replace") as f:
        start = 0
        if skip:
            count = sum(1 for _ in f)
            start = int(count * skip)
            if start < 0:
                start = max(0, count + start)
            else:
                start = min(start, count)
            f.seek(0)
        yield from itertools.islice(f, start, None)


def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    skip = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    only = sys.argv[4] if len(sys.argv) > 4 else None
    maps = {}
    for mod in ("hookshot_vk.dll", "hookshot.exe"):
        mp = os.path.join(BUILD, mod[:-4] + ".map")
        if os.path.exists(mp):
            base = 0x180000000 if mod.endswith(".dll") else 0x140000000
            maps[mod] = load_map(mp, base)

    def name(frame):
        mod, _, off = frame.rpartition("+")
        if mod in maps:
            addrs, names = maps[mod]
            i = bisect.bisect_right(addrs, int(off, 16)) - 1
            if i >= 0:
                return mod.split(".")[0] + ":" + demangle(names[i])
        return mod

    incl, self_, leafmod = collections.Counter(), collections.Counter(), collections.Counter()
    n = 0
    for line in profile_lines(path, skip):
        frames = line.strip().split(";")
        if not frames or not frames[0]:
            continue
        named = [name(f) for f in frames]
        if only and not any(only in f for f in named):
            continue
        n += 1
        self_[named[0]] += 1
        leafmod[frames[0].rpartition("+")[0]] += 1
        for f in set(named):
            incl[f] += 1
    print("%d samples" % n)
    print("-- leaf modules")
    for k, v in leafmod.most_common(10):
        print("%6.1f%%  %s" % (100.0 * v / n, k))
    print("-- inclusive")
    for k, v in incl.most_common(top):
        print("%6.1f%%  %s" % (100.0 * v / n, k))
    print("-- self")
    for k, v in self_.most_common(top):
        print("%6.1f%%  %s" % (100.0 * v / n, k))


if __name__ == "__main__":
    main()
