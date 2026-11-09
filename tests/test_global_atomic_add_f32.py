"""A float atomic add at a global address no storage buffer was tracked to has a SPIR-V fallback.

OoT's compute program 499dbfd0721d2858 did not translate: "SPIR-V Instruction is not implemented
(GlobalAtomicAddF32)". The back end now calls a fallback like the 32-bit integer atomics': the buffers searched by
address, then the page table where stores follow it, the sum made in a compare-exchange loop.
python -I -B tests/test_global_atomic_add_f32.py
"""
import re
from pathlib import Path

SPIRV = Path(__file__).resolve().parents[1] / "gpu" / "vk" / "eden" / "shader_recompiler" / "backend" / "spirv"


def main():
    atomic = (SPIRV / "emit_spirv_atomic.cpp").read_text(encoding="utf-8")
    m = re.search(r"Id EmitGlobalAtomicAddF32\(([^)]*)\)\s*\{(.*?)\n\}", atomic, re.S)
    if not m:
        raise SystemExit("emit_spirv_atomic.cpp: no EmitGlobalAtomicAddF32")
    if "Id address" not in m.group(1) or "atomic_global_add_f32_func" not in m.group(2):
        raise SystemExit("EmitGlobalAtomicAddF32 does not call the fallback (it threw 'not implemented')")
    decl = (SPIRV / "emit_spirv_instructions.h").read_text(encoding="utf-8")
    if "Id EmitGlobalAtomicAddF32(EmitContext& ctx, Id address, Id value);" not in decl:
        raise SystemExit("emit_spirv_instructions.h: EmitGlobalAtomicAddF32 is declared without its operands")
    context = (SPIRV / "spirv_emit_context.cpp").read_text(encoding="utf-8")
    start = context.find("atomic_global_add_f32_func = func;")
    body = context[context.rfind("if (info.stores_global_memory)", 0, start):start]
    for piece in ("OpFAdd(F32[1]", "OpAtomicCompareExchange(", "define_body(", "guest_lookup("):
        if piece not in body:
            raise SystemExit(f"spirv_emit_context.cpp: the f32 add fallback has no {piece}")
    print("PASS: GlobalAtomicAddF32 has a fallback: buffers, then the page table, in a compare-exchange loop")


if __name__ == "__main__":
    main()
