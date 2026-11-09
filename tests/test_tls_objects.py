"""vk_draw.cpp's namespace-scope thread_locals need no constructor or destructor: those are TLS_OBJECTs.

With llvm-mingw every use of a thread_local whose type has a constructor or destructor calls the file's TLS init function
(~2.4% of the replay thread before the file's per-thread objects went behind TLS_OBJECT pointers). This fails when a
namespace-scope thread_local's type is a standard container or holds one (directly or through a struct of this file), so a
new one goes through TLS_OBJECT instead. Function-local static thread_locals are guarded at their own use, not by the file's
init function, and are not checked.

python -I -B tests/test_tls_objects.py [--source gpu/vk/vk_draw.cpp]
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
# (a member of one of these types gives a struct a destructor)
HEAVY = re.compile(r"std::(vector|unordered_map|map|unordered_set|set|string|deque|list|unique_ptr|shared_ptr|function|array)\b")
DECL = re.compile(r"^(?:static\s+)?thread_local\s+(?:const\s+)?([\w:<>,\s*]+?)\s*\b(\w+)\s*(?:\[[^\]]*\]\s*)*(?:=|;|\{)")


def struct_bodies(text):
    """struct name -> its body, for the structs defined at the start of a line"""
    out = {}
    for m in re.finditer(r"(?m)^struct (\w+)\s*\{", text):
        i, depth = m.end(), 1
        while depth and i < len(text):
            depth += (text[i] == "{") - (text[i] == "}")
            i += 1
        out[m.group(1)] = text[m.end():i]
    return out


def heavy_structs(bodies):
    """the structs that hold a heavy member, directly or through another such struct"""
    heavy = {name for name, body in bodies.items() if HEAVY.search(body)}
    while True:
        more = {name for name, body in bodies.items() if name not in heavy and any(re.search(r"\b%s\b" % h, body) for h in heavy)}
        if not more:
            return heavy
        heavy |= more


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_draw.cpp")
    args = parser.parse_args()
    text = args.source.read_text(encoding="utf-8")
    heavy = heavy_structs(struct_bodies(text))
    bad, light = [], 0
    for n, line in enumerate(text.split("\n"), 1):
        m = DECL.match(line)
        if not m:
            continue
        typ, name = m.group(1).strip(), m.group(2)
        if typ.endswith("*"):
            light += 1
            continue
        base = re.sub(r"<.*", "", typ).strip()
        if HEAVY.search(typ) or base in heavy:
            bad.append("%d: %s (%s)" % (n, name, typ))
        else:
            light += 1
    objects = len(re.findall(r"(?m)^TLS_OBJECT\(", text))
    if objects < 10:
        raise SystemExit("only %d TLS_OBJECTs: the file's per-thread objects are not where this test looks" % objects)
    if bad:
        raise SystemExit("thread_locals with a constructor or destructor (use TLS_OBJECT):\n  " + "\n  ".join(bad))
    print("PASS: %d TLS_OBJECTs; %d other namespace-scope thread_locals, none with a constructor or destructor" % (objects, light))


if __name__ == "__main__":
    main()
