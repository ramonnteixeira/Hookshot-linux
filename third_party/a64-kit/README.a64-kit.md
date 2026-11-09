# a64-kit: AArch64 (Nintendo Switch)

The fourth kit, for 64-bit ARM console code. First port: [Tears of the Kingdom](../ports/zelda-totk/PORT.md).

**Status (2026-10-11): milestone 2.** The whole of TOTK's `main` + `sdk` lifts to C in about a minute, every
instruction form the game uses matches Unicorn bit for bit, and the native build compiles and links. The Horizon OS
layer (`hos/`) is only the image builder and a loader so far: nothing boots yet.

**Licensing:** the instruction translator is suyu's (GPL-2.0-or-later, `lift/suyu/`), so this kit and everything
built with it are GPL. The other kits are untouched by it. See [third_party/NOTICE.md](../third_party/NOTICE.md).

## Pipeline

```
 NSP ─► tools/nx.py ─► exefs/ ─► hos/nx_image.py ─► image.bin + image.json     we are rtld: modules at fixed
                                  (tools/nso.py)                                 addresses, relocations applied,
                                                                                 imports bound to hooks
                                        │
                                        ▼
                      lift/a64_lift.py (functions, control flow, jump tables, PLT, calls)
                      + lift/a64emit.dll (suyu's per-instruction translator, via ctypes)
                                        │
                                        ▼
            gen/code_NNN.c, a64_table.c, a64_suyu.h/.c (generated runtime), a64_stubs.c (import stubs)
                                        │
                                        ▼
                      build.py native: clang -O1 + rt/ + hos/ -> <name>.exe
```

```bash
python a64-kit/build.py lift   totk --exefs TOTK/exefs --skip subsdk0 --build TOTK/build
python a64-kit/build.py native totk --build TOTK/build
python a64-kit/tests/insn_test.py TOTK/image --per 8 --states 12 --fpsr
```

## Layout

| Path | What |
|---|---|
| `tools/nx.py` | NSP (PFS0) → NCA → ExeFS / RomFS. AES-XTS header, AES-CTR sections, keys from `~/.switch/prod.keys` (never printed) |
| `tools/nso.py` | NSO loader: LZ4 segments, MOD0, dynamic section, symbols, relocations, init_array, `.eh_frame_hdr` function starts |
| `tools/survey.py` | Instruction census of a module's `.text` |
| `tools/toolchain.py` | emsdk clang/lld lookup, native target flags |
| `hos/nx_image.py` | The image builder (rtld's job at build time) |
| `hos/hos_main.c` | Native process start (loads the image; booting is next) |
| `lift/a64_lift.py` | The lifter |
| `lift/a64emit.cpp`, `build_emit.py`, `a64emit.py` | suyu's translator as a DLL and its ctypes binding |
| `lift/a64_rtgen.py` | Writes suyu's FP helpers and runtime pieces, adapted to `rt/a64.h`, next to the lifted code |
| `lift/suyu/` | `arm64_to_c.h`, vendored unmodified, with its licenses |
| `rt/a64.h` | The guest model the lifted code sees |
| `rt/a64_rt.c` | Dispatch, indirect calls and jumps, traps, exclusive monitor, CNTPCT |
| `tests/insn_test.py` | Every instruction form a game uses, Unicorn vs lifted C |

## The guest model

| | a64-kit |
|---|---|
| Guest address space | 64-bit registers; every mapping below 4 GB (our loader and memory HLE pick all addresses), so an access is `A64_MEM + (uint32_t)addr`, natively and in wasm32 |
| Context | suyu's `GuestContext` shape: `x[32]` (x[31] = SP), NZCV bytes, `vreg[32][2]`, `fpcr`/`fpsr`, TPIDR(RO)_EL0, plus our exclusive monitor and thread pointer |
| Functions | `void f_XXXXXXXX(Ctx *restrict c)`: an entry plus everything reachable without crossing another entry (ppc-kit's model). Branches and fallthrough into another entry are `TAILCALL`s |
| Calls | `bl` writes LR and calls; `blr` → `a64_icall` (binary search over the function table); `ret` returns |
| `br` | Recovered jump tables → `switch` on the target; PLT stubs resolved through the relocated GOT; anything else (virtual tail calls) → `a64_ijump` |
| Exclusives | `ldxr` remembers (address, size, value); `stxr` compare-and-swaps against it, so real host threads are safe |
| Entries | `.eh_frame_hdr` starts, exports, init_array, RELATIVE/ABS64 targets in code, `bl` targets, ADR / ADRP+ADD targets |

## Numbers on TOTK (2026-10-11)

| | |
|---|---|
| Lift | 281,751 functions, 12.6M instructions emitted (shared tails included), 44 files, 1.1 GB of C, ~60 s |
| Translator coverage | 868,649 distinct words; 114 untranslatable, all data in code or the SDK's LSE path the A57 never takes (they trap) |
| Jump tables | 2,858 recognised; 27,126 `br` stay indirect: ~26,750 of them are virtual tail calls (`ldr xN, [vtable]; br xN`), ~380 other shapes are unchecked |
| FP guards | 654k suyu FPX1 guards, each one call of a generated helper (`a64_fpx.h`) instead of inline code |
| Instruction test | 574 forms, 38,494 random states, registers + NZCV + V regs + FPSR + memory: 0 differences from Unicorn |
| Compile | 100-200 s per ~25 MB file at -O1 on one core (LoopRotate capped, see build.py) |

What made the compile tractable (each measured on TOTK's physics chunk, 20 functions of up to 18k instructions):
1. **No entries at ADR / ADRP+ADD targets.** 3,735 entries came only from those, nearly all jump-table base labels;
   each cut a function in two, and both halves re-walked the code between: 14.4M → 12.6M instructions.
2. **FPX guards outlined** into one helper call per lane op (`A64_FPX_FN`, noinline by default; define it as
   `static inline` to trade compile time for speed).
3. **`-mllvm -rotation-max-header-size=0`**: LoopRotate was 120 s of a 306 s compile; with it, 197 s.
4. Splitting giant functions (`--max-func`) does not help: they are loops end to end, so it finds almost no cut.
