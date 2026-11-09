"""Changes to the lifted C for Ocarina of Time, applied by lift.py after it writes the code (and by hand: python -I nx2/oot_patches.py).

Each is a runtime switch where it can be, so the game's own path stays reachable.

- Dynamic resolution (Lichii's built-in "Max Resolution" patch, main+0x805BD0): the engine's config flag at +0x94 picks the path that
  follows the GPU-time controller; with hos_fixed_resolution set it takes the other path, the 3D at full scale. The controller made
  a new set of render targets at each size, ~11 images a frame at times, and held the GPU near its budget.
- Text speed (the Insert menu's "Text speed"): f_80b1e140 (main+0xafe140) steps a text pane's letter animation, the
  LetterAnimControl of the panes with "LetterAnimOn" user data (dialogue, Navi, signs). Its time argument (s0) moves the
  letters' clock (+0x60 += rate +0x4c * time, the rate from the message's speed tag) and the pauses' timer (+0x58 -= time);
  hos_text_scale multiplies it, 1.0 as shipped. The auto-advance timer (+0x5c, before) keeps the game's time. A huge
  scale shows a page at once (one frame per pause), as the game's own skip on A (+0x72 = 2, f_80794b00) without its flags.
- The fixed path's scale (hos_fixed_scale_bits: the Insert menu's Resolution) reaching the 3D, in the caller f_831f3200
  (main+0x31d3200). It recomputes the 3D's scale (+0xe5c = scale * +0xe58 / +0xe50) only while +0xe2c bit 3 is set. The dynamic
  path sets it on a level change; the fixed path's flag 1 calls f_831f41d0, whose bit 3 the apply block clears in the same frame,
  before any recompute. So the fixed path kept the dynamic path's last scale (0.75 docked: 1536x864 at every setting). Flag 1 now
  also asks for the recompute, and on the fixed path the flags +0xe20 bits 3/4 (a hardcoded 0.75 / 0.5 in place of the scale)
  are not looked at.
"""
import glob
import os
import sys

DECL = "extern volatile int hos_fixed_resolution;\nextern volatile int hos_dynres_runs;\nextern volatile uint32_t hos_fixed_scale_bits;\n"
OLD = "if (!((c->x[8] >> 0) & 1)) { goto L_8082640c; }"
NEW = "if (hos_fixed_resolution || !((c->x[8] >> 0) & 1)) { goto L_8082640c; }\n    hos_dynres_runs++;"
LABEL = "L_8082640c:;"
SCALE_OLD = "c->x[9] = 0x3f800000ULL;"
SCALE_NEW = "c->x[9] = hos_fixed_scale_bits;"   # (the fixed path's scale, 1.0 as shipped)
DECL2 = "extern volatile int hos_fixed_resolution;\nextern volatile uint64_t hos_res_module;\nextern volatile uint32_t hos_fixed_scale_bits;\n"
APPLY_OLD = "L_831f3efc:;\n    c->x[9]=recomp_load32(c,c->x[20] + 3628);"
APPLY_NEW = "L_831f3efc:;\n    if (hos_fixed_resolution) {   /* (only while +0xe5c is not the chosen scale: a recompute every frame remade the targets) */\n        union { uint32_t u; float f; } now = {(uint32_t)recomp_load32(c,c->x[20] + 3676)}, want = {hos_fixed_scale_bits},\n            e50 = {(uint32_t)recomp_load32(c,c->x[20] + 3664)}, e58 = {(uint32_t)recomp_load32(c,c->x[20] + 3672)};\n        const float d = now.f - (e50.f != 0 ? want.f * e58.f / e50.f : want.f);\n        if (d > 1e-4f || d < -1e-4f) c->x[8] |= 8;\n    }\n    c->x[9]=recomp_load32(c,c->x[20] + 3628);"
FLAGS_OLD = "L_831f3f08:;\n    c->x[8]=recomp_load64(c,c->x[20] + 3616);\n"
FLAGS_NEW = FLAGS_OLD + "    hos_res_module = c->x[20];\n    if (hos_fixed_resolution) c->x[8] &= ~0x18ULL;\n"


def fixed_scale(t):
    """The fixed path's scale reaching the 3D (the Resolution row). (t, 1) when applied here"""
    if APPLY_OLD not in t or FLAGS_OLD not in t:
        return t, 0
    t = t.replace(APPLY_OLD, APPLY_NEW, 1).replace(FLAGS_OLD, FLAGS_NEW, 1)
    return t.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL2, 1), 1

TEXT_DECL = "extern volatile float hos_text_scale;\n"
LIFE_BUILD_ID = "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348"
LIFE_HEAD = "void f_804bd8c0(Ctx *__restrict A64_NOESC c) {\n"
MOVE_HEAD = "void f_82c415e0(Ctx *__restrict A64_NOESC c) {\n"
MOVE_ANCHOR = ("if (((c->x[8] >> 1) & 1)) { goto L_82c41748; }\n")
MOVE_MATRIX = "L_82c41778:;\n"
WARP_HEAD = "void f_806d2530(Ctx *__restrict A64_NOESC c) {\n"
WARP_ANCHOR = "{ uint64_t _t = c->x[8]; c->x[30] = 0x806d25c8ULL; A64_ICALL(c, _t); }\n"
VIEW_HEAD = "void f_804e48c0(Ctx *__restrict A64_NOESC c) {\n"
VIEW_ANCHOR = "L_804e5068:; A64_POLL(c);\n"
LIFE_CAS = ("{ uint64_t _a = c->x[13]; uint32_t _e = (uint32_t)c->x[11]; "
            "__atomic_compare_exchange_n((uint32_t *)A64_PTR(_a), &_e, (uint32_t)c->x[12], "
            "0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); A64_MARK(_a, 4); c->x[11] = (uint64_t)_e; }")


def patch_life(source):
    start = source.find(LIFE_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    if end < 0 or source[start:end].count(LIFE_CAS) != 2:
        return None
    # x19 is the actor. Confirm its type and life pointer before protecting this write.
    guard = ("    if ((uint32_t)c->x[12] < (uint32_t)c->x[11] && hos_oot_hearts_enabled() &&\n"
             "        hos_oot_is_player(c, c->x[19]) &&\n"
             "        c->x[13] == recomp_load64(c,c->x[19] + 7184) + 8)\n"
             "        c->x[12] = c->x[11];\n")
    body = source[start:end].replace(LIFE_CAS, guard + LIFE_CAS)
    return (source[:start] + body + source[end:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern int hos_oot_hearts_enabled(void);\n'
        'extern int hos_oot_is_player(Ctx *c, uint64_t actor);\n', 1)


def patch_movement(source):
    start = source.find(MOVE_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    if end < 0 or source[start:end].count(MOVE_ANCHOR) != 1 or source[start:end].count(MOVE_MATRIX) != 1:
        return None
    bypass = ("    hos_oot_lift_player(c, c->x[19]);\n"
              + MOVE_ANCHOR)
    body = source[start:end].replace(MOVE_ANCHOR, bypass, 1).replace(
        MOVE_MATRIX, MOVE_MATRIX + "    hos_oot_flight_pose(c, c->x[19], c->x[25]);\n", 1)
    source = source[:start] + body + source[end:]
    return source.replace('#include "a64.h"\n', '#include "a64.h"\n'
                          'extern void hos_oot_lift_player(Ctx *c, uint64_t actor);\n'
                          'extern void hos_oot_flight_pose(Ctx *c, uint64_t actor, uint64_t matrix);\n', 1)


def patch_flight_rotation(source):
    start = source.find(WARP_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    body = source[start:end]
    if end < 0 or body.count(WARP_ANCHOR) != 1:
        return None
    body = body.replace(WARP_ANCHOR, "    hos_oot_flight_rotation(c, c->x[0], c->x[1]);\n" + WARP_ANCHOR)
    return (source[:start] + body + source[end:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\n'
        'extern void hos_oot_flight_rotation(Ctx *c, uint64_t controller, uint64_t matrix);\n', 1)


def patch_view(source):
    start = source.find(VIEW_HEAD)
    if start < 0:
        return None
    end = source.find("\n}", start)
    body = source[start:end]
    if end < 0 or body.count(VIEW_ANCHOR) != 1:
        return None
    body = body.replace(VIEW_ANCHOR, VIEW_ANCHOR + "    hos_oot_free_view(c, c->x[20]);\n")
    return (source[:start] + body + source[end:]).replace(
        '#include "a64.h"\n', '#include "a64.h"\nextern void hos_oot_free_view(Ctx *c, uint64_t view);\n', 1)

TEXT_HEAD = "void f_80b1e140(Ctx *__restrict A64_NOESC c) {"
# (the letters' block: past the early returns, the auto-advance timer and the hidden / no-text checks; reached once a call)
TEXT_OLD = ("L_80b1e1b4:;\n"
            "    c->x[9]=recomp_load16(c,c->x[19] + 110);\n"
            "if ((uint32_t)c->x[9] == 0) { goto L_80b1e18c; }\n")
TEXT_NEW = TEXT_OLD + ("    if (hos_text_scale != 1.0f) { float _t; uint32_t _u; memcpy(&_t,c->vreg[0],4); _t *= hos_text_scale; "
                       "memcpy(&_u,&_t,4); a64_vset64(c->vreg[0],_u); }   /* (nx2/oot_patches.py: text speed) */\n")


def patch_dynres(s):
    if OLD not in s:
        return None
    s = s.replace(OLD, NEW, 1)
    at = s.index(LABEL)
    scale = s.index(SCALE_OLD, at)
    s = s[:scale] + SCALE_NEW + s[scale + len(SCALE_OLD):]
    return s.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL, 1)


def patch_text_speed(s):
    """The scale at the head of the letters' block of f_80b1e140; None when the file has no such function, s unchanged when the
    function is there but its code differs (another game version)."""
    head = s.find(TEXT_HEAD)
    if head < 0:
        return None
    end = s.find("\nvoid f_", head + len(TEXT_HEAD))
    body = s[head:end if end >= 0 else len(s)]
    if TEXT_NEW in body:   # (applied already: a second run)
        return s
    if body.count(TEXT_OLD) != 1 or body.count("goto L_80b1e1b4;") != 1:
        return s
    at = head + body.index(TEXT_OLD)
    s = s[:at] + TEXT_NEW + s[at + len(TEXT_OLD):]
    return s.replace('#include "a64.h"\n', '#include "a64.h"\n' + TEXT_DECL, 1)


def apply(gen, *, protect_hearts=False):
    import oot_native_resolution   # (HOS_SCENE_NATIVE=1: the scene's base scale 1.0; off by default since #849's output resolution)
    oot_native_resolution.apply(gen)
    done = {"dynamic resolution branch": 0, "text speed block": 0, "fixed path's scale": 0}
    if protect_hearts:
        done["player heart protection"] = 0
        done["player flight"] = 0
        done["player flight rotation"] = 0
        done["free camera"] = 0
    for path in sorted(glob.glob(os.path.join(gen, "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        out = s
        d = patch_dynres(out)
        if d is not None:
            out = d
            done["dynamic resolution branch"] += 1
            print("oot_patches: dynamic resolution switch in", os.path.basename(path))
        t = patch_text_speed(out)
        if t is not None and t != out:
            out = t
            done["text speed block"] += 1
            print("oot_patches: text speed scale in", os.path.basename(path))
        out, n = fixed_scale(out)
        if n:
            done["fixed path's scale"] += 1
            print("oot_patches: the fixed path's scale in", os.path.basename(path))
        life = patch_life(out) if protect_hearts else None
        if life is not None:
            out = life
            done["player heart protection"] += 1
            print("oot_patches: player heart protection in", os.path.basename(path))
        move = patch_movement(out) if protect_hearts else None
        if move is not None:
            out = move
            done["player flight"] += 1
            print("oot_patches: player flight in", os.path.basename(path))
        rotation = patch_flight_rotation(out) if protect_hearts else None
        if rotation is not None:
            out = rotation
            done["player flight rotation"] += 1
            print("oot_patches: player flight rotation in", os.path.basename(path))
        view = patch_view(out) if protect_hearts else None
        if view is not None:
            out = view
            done["free camera"] += 1
            print("oot_patches: free camera in", os.path.basename(path))
        if out != s:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(out)
    for what, n in done.items():
        if n != 1:
            print("oot_patches: WARNING: the %s was found %d times" % (what, n))
    apply_output_size(gen)


# Output size (host/nn_applet.c hos_output_size: the Insert menu's Output resolution, 2560 x 1440 as shipped):
# - nnMain (f_81222ce0) puts the display size in the framework's create arg (+20/+24, the framework's +156/+160); the window
#   textures are made at it.
# - f_810a7b70 sets up the display-size object (B = [[0x84ca29c0]+216]) from a config (x1): its docked size list (+8, first
#   2560 x 1440) is the frame's base (B+244; the frame is the base times the graphics scale's factor, f_806702e0, capped by the
#   scale's display class: docked 3840 x 2160 at scale 0). Its first entry asks the host when it differs.
OUT_DECL = "uint32_t hos_output_size(int height);\nuint64_t hos_rt_pool_size(uint64_t game);\n"
OUT_EDITS = [
    # (the picture's 2560 x 1440 at 0x81202ee4, not f_831f3200's copy at +0xb68; nx2/oot_wide.py's widescreen size, after it,
    # stays over it: host/hos_main.c makes that one at this height)
    ("    c->x[8] = 0xa00ULL;\n    c->x[8] = (c->x[8] & ~(0xFFFFULL<<32)) | (0x5a0ULL<<32);\n"
     "{ extern volatile uint64_t hos_wide_out; const uint64_t _s = hos_wide_out; if (_s) c->x[8] = _s; }   /* (nx2/oot_wide.py: hos_wide_out) */\n"
     "    { uint64_t _b=c->x[21]; int64_t _o=20; recomp_store64(c,(_b+_o),c->x[8]); }",
     "    c->x[8] = hos_output_size(0) | ((uint64_t)hos_output_size(1) << 32);\n"
     "{ extern volatile uint64_t hos_wide_out; const uint64_t _s = hos_wide_out; if (_s) c->x[8] = _s; }   /* (nx2/oot_wide.py: hos_wide_out) */\n"
     "    { uint64_t _b=c->x[21]; int64_t _o=20; recomp_store64(c,(_b+_o),c->x[8]); }"),
    ("    { uint64_t _a=c->x[0], _b=724ULL; uint64_t _r=_a+_b; c->x[21]=_r; }\n"
     "    { uint64_t _r = ((uint64_t)0 | ((c->x[1]) << 0)); c->x[20] = _r; }\n",
     "    { uint64_t _a=c->x[0], _b=724ULL; uint64_t _r=_a+_b; c->x[21]=_r; }\n"
     "    { uint64_t _r = ((uint64_t)0 | ((c->x[1]) << 0)); c->x[20] = _r; }\n"
     "    if (hos_output_size(0) != 2560 && recomp_load32(c, c->x[20] + 8) == 2560) { recomp_store32(c, c->x[20] + 8, hos_output_size(0)); recomp_store32(c, c->x[20] + 12, hos_output_size(1)); }\n"),
    # - f_80ea2f30: the 3D's largest size, ResolutionParam's BaseResolution (or 1920 x 1080 as constants), 3/4 of 2560 x 1440;
    #   above it the game takes a smaller upscale network (1/3: 1280 x 720 at 4K). Scaled with the output.
    ("c->x[30] = 0x80ea2ffcULL; f_831cca50(c);\n"
     "    { uint64_t _b=c->x[0]; int64_t _o=0; { uint64_t _p0,_p1; recomp_ldp32(c,(_b+_o),&_p0,&_p1); c->x[24]=_p0; c->x[25]=_p1; } }\n",
     "c->x[30] = 0x80ea2ffcULL; f_831cca50(c);\n"
     "    { uint64_t _b=c->x[0]; int64_t _o=0; { uint64_t _p0,_p1; recomp_ldp32(c,(_b+_o),&_p0,&_p1); c->x[24]=_p0; c->x[25]=_p1; } }\n"
     "    if (hos_output_size(0) != 2560) { c->x[24] = (uint32_t)c->x[24] * hos_output_size(0) / 2560; c->x[25] = (uint32_t)c->x[25] * hos_output_size(1) / 1440; }\n"),
    ("    c->x[8] = 0x44f00000ULL;\n    c->x[8] &= 0xFFFFFFFFULL;\n    recomp_store32(c,c->x[19] + 3668,c->x[8]);\n"
     "    c->x[8] = 0x44870000ULL;\n    c->x[8] &= 0xFFFFFFFFULL;\n",
     "    c->x[8] = hos_output_size(0) * 3 / 4;\n    { float _f = (float)c->x[8]; uint32_t _u; memcpy(&_u, &_f, 4); c->x[8] = _u; }\n"
     "    recomp_store32(c,c->x[19] + 3668,c->x[8]);\n"
     "    c->x[8] = hos_output_size(1) * 3 / 4;\n    { float _f = (float)c->x[8]; uint32_t _u; memcpy(&_u, &_f, 4); c->x[8] = _u; }\n"),
    # - The render targets' GPU pool: 0x13100000 bytes (the graphics config's +1464, read in f_836ce9d0, which takes its storage from
    #   the "Graphics" heap and makes a physical pool and a virtual one over it: f_80c4a1c0 -> f_80c49070 -> f_80c49380 -> f_811c9ba0).
    #   Sized for 2560 x 1440: a 3840x2160 3D at the 4K output ran out of it and the next target came back null (crash in f_8005ed00,
    #   the pool's allocator, its +0x28 0x13100000). Scaled with the output's pixels (host/nn_applet.c
    #   hos_rt_pool_size: 687 MB at 3840 x 2160), at the config, so the storage and the pool agree.
    ("L_836cea54:;\n",
     "L_836cea54:;\n"
     "    if (recomp_load64(c, c->x[19] + 1464) == 0x13100000ULL) recomp_store64(c, c->x[19] + 1464, hos_rt_pool_size(0x13100000ULL));\n"),
    # - The "Graphics" heap holds it, made by f_83b7b340 with the other module heaps (twice: 0x34900000 bytes in f_83b7a2c0's first
    #   pass, then the heap kept, 0x348ffe20; it had under 16 MB to spare): grown by as much as the pool.
    ("c->x[30] = 0x83b7b440ULL; f_809df710(c);\n",
     "if (c->x[0] == 0x34900000ULL || c->x[0] == 0x348ffe20ULL) c->x[0] += hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL;\n"
     "c->x[30] = 0x83b7b440ULL; f_809df710(c);\n"),
    # - f_83b7b340 then takes each module heap's use (its vtable +120) off the module's budget (the configured size, a table of slots)
    #   and aborts below zero (f_8351c280): the Graphics heap's use counts as the shipped one.
    ("{ uint64_t _t = c->x[8]; c->x[30] = 0x83b7b4e0ULL; A64_ICALL(c, _t); }\n",
     "{ uint64_t _t = c->x[8]; c->x[30] = 0x83b7b4e0ULL; A64_ICALL(c, _t); }\n"
     "{ const uint64_t _b = recomp_load64(c, c->x[21] + (c->x[22] << 3)), _g = hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL; "
     "if ((_b == 0x34900000ULL || _b == 0x348ffe20ULL) && c->x[0] > _g) c->x[0] -= _g; }\n"),
    # - f_80b72340 (from the graphics init f_836cf4b0) makes a heap of what the Graphics heap can still give, which took the growth as
    #   well (0x1b6cb48 bytes as shipped, 0x1996cb48 grown) and left too little for the pool: it keeps its shipped size.
    ("c->x[30] = 0x80b723acULL; f_809df700(c);\n",
     "if (c->x[0] > hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL) c->x[0] -= hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL;\n"
     "c->x[30] = 0x80b723acULL; f_809df700(c);\n"),
]


def apply_output_size(gen):
    found = [0] * len(OUT_EDITS)
    for path in sorted(glob.glob(os.path.join(gen, "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        hit = False
        for i, (old, new) in enumerate(OUT_EDITS):
            if new in s:   # (applied already: a second run. Each new text holds its old one, which would be patched again)
                found[i] += 1
            elif old in s:
                found[i] += s.count(old)
                s = s.replace(old, new)
                hit = True
        if hit:
            if OUT_DECL not in s:
                s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + OUT_DECL, 1)
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(s)
            print("oot_patches: output size in", os.path.basename(path))
    if found != [1] * len(OUT_EDITS):
        print("oot_patches: WARNING: the output size places were found %s times" % found)


if __name__ == "__main__":
    apply(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "sa", "gen"))
