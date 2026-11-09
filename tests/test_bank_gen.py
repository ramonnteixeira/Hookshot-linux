"""A graphics stage's driver banks are kept by the generation of the bindings they are made from.

c2 of a graphics stage holds its texture, sampler and image handles and c0 its storage buffer table, so a uniform buffer's bind (nearly every
draw) does not make the bank stale: tex_gen[stage] and ssbo_gen[stage] are what bind_gen was when those last changed. Compute, task and
mesh stages (c0 holds a grid, push constants, the pointer table) stay on bind_gen.
python -I -B tests/test_bank_gen.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    vk = ROOT / "gpu" / "vk"
    bank = (vk / "vk_bank.cpp").read_text(encoding="utf-8")
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    draw = (vk / "vk_draw.cpp").read_text(encoding="utf-8")
    fill = block(bank, "void driver_bank_fill(")
    c2 = block(fill, "if (bank == 2) {")
    if set(re.findall(r"\bs\.(\w+)", c2)) - {"tex", "img", "samp"}:
        raise SystemExit("c2 is made from more than tex_gen covers: " + str(set(re.findall(r"\bs\.(\w+)", c2))))
    tail = fill[fill.index("static constexpr uint32_t kSsboTable"):]
    if set(re.findall(r"\bs\.(\w+)", tail)) - {"ssbo"} or "pointer_table(s, stage)" not in block(tail, "if ((stage == 6 || stage == 7)"):
        raise SystemExit("a graphics stage's c0 is made from more than ssbo_gen covers")
    if "stage == 5" not in fill[: fill.index("static constexpr uint32_t kSsboTable")]:
        raise SystemExit("compute's c0 is no longer apart from the graphics stages'")
    # (every place the handles or storage buffers change moves their generation with bind_gen)
    for marker, field in (("case NOP_BIND_TEX:", "tex_gen"), ("case NOP_SET_TEX_POOL:", "tex_gen")):
        at = replay.index(marker)
        if field not in replay[at: at + 1400]:
            raise SystemExit(f"{marker} does not set {field}")
    if replay.count("ssbo_gen[b.stage] = s.bind_gen") != 2:
        raise SystemExit("a storage buffer bind does not set ssbo_gen (NOP_BIND_BUFS and NOP_BIND_SSBO)")
    if "s->tex_gen[k] = gen_base, s->ssbo_gen[k] = gen_base" not in block(replay, "void use_queue_state("):
        raise SystemExit("another state's generations are not made afresh with bind_gen")
    for name in ("s.tex[", "s.samp[", "s.img[", "s.ssbo["):
        stores = [m for m in re.finditer(re.escape(name) + r"[^;\n]*\]\s*=[^=]", replay)]
        if stores:
            raise SystemExit(f"{name} is stored to outside the bind handlers' arrays")
    uniform = block(draw, "VkDescriptorBufferInfo uniform_range(")
    if "bank_stage > 4 ? s.bind_gen" not in uniform or "RX_VK_BANK_GEN_OFF" not in uniform:
        raise SystemExit("compute, task and mesh banks are not kept by bind_gen, or the switch is missing")
    if "c.bind_gen = gen;" not in uniform or "c.bind_gen == gen" not in uniform:
        raise SystemExit("the bank cache is not kept and checked by the same generation")
    print("PASS: a graphics stage's banks are kept by the generation of the bindings they are made from")


if __name__ == "__main__":
    main()
