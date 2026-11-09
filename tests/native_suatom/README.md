# Direct surface atomic regression

The fields in `../native_suatom.cpp` are independently authored. No game shader,
dump, cache or private SDK instruction is used. The small Python check compiles
the production lowering header; the CMake target additionally runs the same
instructions through the actual Maxwell CFG/translator, optimization passes and
SPIR-V emitter. Every emitted module is validated in the existing Linux SPIR-V
job. No GPU or game executes.

The accepted form is direct-descriptor `0x593`, 1D U32 ADD, sample addressing,
IGN, Strong(GPU), normal eviction, and no fault-predicate destination. Other
forms and modifiers are refused. Registers and descriptor fields are authored
separately, with alias/high-register, predication, branch relocation, offset/bank
boundary and conflict cases. The full path checks the descriptor bank and byte
offset, atomic coordinate/addend, and a store consuming the returned old value.

Strong(GPU) is not an acquire/release qualifier. Mesa emits image atomics with
`MemOrder::Strong(MemScope::GPU)` and handles explicit barriers separately. The
existing image atomic backend emits a Device-scoped relaxed atomic; no fences
are added by this lowering. A case with explicit native MEMBARs verifies those
barriers survive independently.

Encoding references:

- [Independent decoder](https://github.com/seedds/nxnative/blob/7f50222003e4270e3655d4f798e85744d390edf0/dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Instructions/InstEmitAmpereMem.cs#L529): direct descriptor bank/word offset and SUATOM operands.
- [Mesa encoder](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/b1fe47e94409d81924c8663f58fac97e66ab4527/src/nouveau/compiler/nak/sm70_encode.rs): shared atomic/dimension/type/order fields. Mesa encodes its register-handle form, not this direct-descriptor opcode.
- [Mesa NIR translation](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/b1fe47e94409d81924c8663f58fac97e66ab4527/src/nouveau/compiler/nak/from_nir.rs): Strong(GPU) image atomics and separate barriers.
- [Public SM87 encoding tables](https://mbuchel.github.io/sass-encoding-lab/): `0x593` and IGN. The opcode table SHA-256 is `559048b6a840e5548ba5fa577e31230f0a217f370d50c7c378e12997d96dd747`.

Configure with Clang C++20 and `-DREPO_ROOT=<repo>` plus
`-DSPIRV_HEADERS_ROOT=<SPIRV-Headers>`, then build `native_suatom`. Run it with an
authored output directory and validate its `.spv` files using `spirv-val
--target-env vulkan1.3`. These checks establish translation and module validity,
not driver execution, general surface-atomic coverage or a visible game fix.
