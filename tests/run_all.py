"""Every game-free check in one run: each tests/test_*.py and nx2/test_*.py in its own process, the C++ unit tests under
gpu/vk/tests and a byte-compile of every tracked Python file. All of them run even when one fails; the failures are
printed whole at the end, with a table of every check (slowest first), also to $GITHUB_STEP_SUMMARY when it is set.
Exits 1 when anything failed.

    python -I -B tests/run_all.py [--jobs N] [--timeout SECONDS] [--strict] [--no-skip] [name ...]

A name picks the checks whose path contains it. --strict: a check that needs a tool this PC lacks (a compiler, the
SPIR-V or Vulkan headers, spirv-val) fails instead of being skipped (CI installs them all). --no-skip runs the checks on
the skip list below too. Standard library only: no package is installed for the runner itself.
"""
import argparse
import concurrent.futures
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINDOWS = os.name == "nt"
OS = "windows" if WINDOWS else "linux"

# Arguments some tests need. {name} is a tool or folder found below (find_tools); a check whose tools are missing is
# skipped, or fails with --strict. Tests not listed run with no arguments and their own defaults.
ARGS = {
    "tests/test_pipeline_cache_save.py": ["--cxx", "{cxx}"],
    "tests/test_capture_copy_runs.py": ["--cxx", "{cxx}"],
    "tests/test_parallel_rb_refresh.py": ["--cxx", "{cxx}"],
    "tests/test_prof_sampling.py": ["--cxx", "{cxx}"],
    "tests/test_screensaver.py": ["--cc", "gcc"],
    "tests/test_system_header_cache.py": ["--cc", "clang"],
    "tests/test_precompile_compute_guard.py": ["--cxx", "clang++"],
    "tests/test_vulkan_clear_mip.py": ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"],
    "tests/test_input_background.py": ["--cc", "gcc"],
    "tests/test_oot_wide_save_picture.py": ["--cc", "gcc"],
    "nx2/test_import_windows.py": ["--cxx", "g++"],
    "tests/test_lower_depbar.py": ["--cxx", "clang++"],
    "tests/test_lower_register_markers.py": ["--cxx", "clang++"],
    "tests/test_text_speed.py": ["--cc", "gcc"],
    "tests/test_audio_in.py": ["--cc", "gcc", "--cxx", "g++"],
    "tests/test_upscale_fallback.py": ["--cxx", "g++"],
    "tests/test_fg_scale_sharpen.py": ["--cxx", "g++"],
    "tests/test_taau_env_lookup.py": ["--cxx", "g++"],
    "tests/test_target_overlap.py": ["--cxx", "g++"],
    "tests/test_menu_hit.py": ["--cxx", "g++"],
    "tests/test_menu_restart.py": ["--cxx", "g++", "--cc", "gcc"],
    "tests/test_output_resolution.py": ["--cxx", "{cxx}"],
    "tests/test_nvn_buffer_map.py": ["--cc", "gcc"],
    "tests/test_startup_progress.py": ["--cxx", "g++"],
    "tests/test_audio_voice_biquad.py": ["--cc", "gcc"],
    "tests/test_motion.py": ["--cc", "gcc"],
    "tests/test_cpp_preflight.py": ["--cxx", "g++"],
    "tests/test_invalidate_vcore.py": ["--cxx", "g++"],
    "tests/test_linux_driver_fault_guard.py": ["--cxx", "{cxx}"],
    "tests/test_seed_cs_pointers.py": ["--cxx", "g++"],
    "tests/test_program_record_head.py": ["--cxx", "g++"],
    "tests/test_seed_kept_loaded.py": ["--cxx", "g++"],
    "tests/test_lower_atomg_cas.py": ["--cxx", "g++"],
    "tests/test_wb_diff_split.py": ["--cxx", "g++"],
    "tests/test_native_suatom.py": ["--cxx", "clang++"],
    "nx2/test_cbuf_table_extent.py": ["--cxx", "g++"],
    "nx2/test_dump_ssbo_read.py": ["--cxx", "g++"],
    "nx2/test_mesh_dynamic_state.py": ["--cxx", "g++"],
    "nx2/test_timeline_modes.py": ["--cxx", "g++"],
    "tests/test_keep_rendered.py": ["--cxx", "g++"],
    "tests/test_program_answer_domains.py": ["--cxx", "clang++", "--headers", "{vulkan_headers}"],
    "nx2/test_texture_phi_dominance.py": ["--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"],
    "nx2/test_bc_storage_views.py": ["--headers", "{vulkan_headers}"],
    "nx2/test_mesh_first_types.py": ["--headers", "{spirv_headers}"],
    "nx2/test_texture_init_barrier.py": ["--headers", "{vulkan_headers}"],
    "tests/test_parallel_init_order.py": ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"],
    "nx2/test_default_point_size.py": ["--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"],
    "nx2/test_unsigned_image_types.py": ["--cxx", "{cxx}", "--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"],
    "nx2/test_depth_sample_types.py": ["--cxx", "{cxx}", "--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"],
    "nx2/test_draw_parameters.py": ["--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"],
    "nx2/test_coop_spirv.py": ["--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"],
    "nx2/test_spirv_struct_identity.py": ["--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"],
    "tests/test_astc_bc_policy.py": ["--cxx", "g++"],
}

# Python packages some tests import (CI installs them; the runner itself needs none)
MODULES = {
    "nx2/test_nup_codec.py": ["zstandard"],
    "nx2/test_nup_verify.py": ["zstandard"],
}

# Checks that cannot run in CI, with why, by OS. Each still runs with --no-skip (or by name).
SKIP = {
    "tests/test_precompile_compute_guard.py": ({"linux"}, "Windows structured exceptions"),
    # (the game: these run the lifted code of a real dump against Unicorn)
    "nx2/test_insn.py": ({"linux", "windows"}, "needs the game's modules (work/dump) and Unicorn"),
    "nx2/test_leaf.py": ({"linux", "windows"}, "needs the game's modules (work/dump) and Unicorn"),
    "nx2/test_dll.py": ({"linux", "windows"}, "needs a full game build (work/build/hookshot_game.dll) and Unicorn"),
    # (a GPU)
    "nx2/test_bc_storage_views.py": ({"linux", "windows"}, "needs a Windows Vulkan device"),
    # (Linux only)
    "tests/test_appimage_source_headers.py": ({"windows"}, "the AppImage source-copy block uses POSIX Bash and cp"),
    "tests/test_hookshot_launch.py": ({"windows"}, "the Linux launcher, a POSIX shell script"),
    "tests/test_linux_update.py": ({"windows"}, "the Linux builder's update and folder launcher, POSIX files and shell scripts"),
    "tests/test_oot_saves.py": ({"windows"}, "the save script's folder discovery is the Linux one (on Windows Hookshot's saves are in the game folder: --hookshot)"),
    "tests/test_linux_driver_fault_guard.py": ({"windows"}, "POSIX signals: the Linux guard"),
}
# (spirv-val: SPIRV-Tools comes with the Linux runner's packages, not on Windows)
for _name in ("nx2/test_texture_phi_dominance.py", "nx2/test_unsigned_image_types.py", "nx2/test_depth_sample_types.py", "nx2/test_default_point_size.py", "nx2/test_draw_parameters.py", "nx2/test_coop_spirv.py",
              "nx2/test_spirv_struct_identity.py"):
    SKIP[_name] = ({"windows"}, "needs spirv-val: checked on Linux")


def which(*names):
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    return None


def header_dir(env, cpm_name, probe):
    """A headers folder: the environment variable, else the CPM cache nx2/build.py reads (RX_CPM, build-linux/deps/cpm)"""
    if os.environ.get(env):
        return os.environ[env]
    for cpm in (os.environ.get("RX_CPM"), os.path.join(ROOT, "build-linux", "deps", "cpm")):
        d = os.path.join(cpm or "", cpm_name)
        if cpm and os.path.isdir(d) and os.listdir(d):
            inc = os.path.join(d, sorted(os.listdir(d))[-1], "include")
            if os.path.exists(os.path.join(inc, probe)):
                return inc
    return None


def find_tools():
    return {
        "spirv_headers": header_dir("HOOKSHOT_SPIRV_HEADERS", "spirv-headers", os.path.join("spirv", "unified1", "spirv.h")),
        "vulkan_headers": header_dir("HOOKSHOT_VULKAN_HEADERS", "vulkanheaders", os.path.join("vulkan", "vulkan.h")),
        "spirv_val": which("spirv-val"),
        "cxx": which(os.environ.get("CXX", ""), "clang++", "g++", "c++"),
    }


class Check:
    def __init__(self, name, run, why_skip=None):
        self.name, self.run, self.why_skip = name, run, why_skip
        self.status, self.secs, self.output = "skip", 0.0, ""


def python_check(rel, tools, timeout):
    args, missing = [], []
    for a in ARGS.get(rel, []):
        if a.startswith("{"):
            v = tools.get(a.strip("{}"))
            if not v:
                missing.append(a.strip("{}"))
            args.append(v or "")
        else:
            args.append(a)
    # (a compiler named by the arguments, or the test's own default one, must be on PATH)
    for i, a in enumerate(args[:-1]):
        if a in ("--cc", "--cxx") and not which(args[i + 1]):
            missing.append(args[i + 1])
    missing += [m for m in MODULES.get(rel, []) if importlib.util.find_spec(m) is None]

    def run():
        return subprocess.run([sys.executable, "-I", "-B", os.path.join(ROOT, rel)] + args, cwd=ROOT, timeout=timeout,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    return run, missing


def cpp_checks(tools, timeout):
    """gpu/vk/tests: the presentation rectangle (all of it), the lowering harness built and run over no programs
    (the programs it reads are dumped from the game, so here it only shows the lowering still compiles), and the
    lowering's register-marker regression"""
    cxx = tools["cxx"]
    out = []

    def build_and_run(src, flags, args):
        def run():
            with tempfile.TemporaryDirectory() as tmp:
                exe = os.path.join(tmp, "t.exe")
                r = subprocess.run([cxx, "-std=c++20", *flags, os.path.join(ROOT, src), "-o", exe], cwd=ROOT,
                                   timeout=timeout, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
                if r.returncode:
                    return r
                return subprocess.run([exe] + [a.replace("{tmp}", tmp) for a in args], cwd=ROOT, timeout=timeout,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        return run
    rect = "gpu/vk/tests/present_rect_test.cpp"
    out.append((rect, build_and_run(rect, ["-Wall", "-Wextra", "-Werror"], [])))
    lower = "gpu/vk/tests/lower_test.cpp"
    eden, compat = os.path.join(ROOT, "gpu", "vk", "eden"), os.path.join(ROOT, "gpu", "vk", "compat")
    out.append((lower, build_and_run(lower, ["-O1", "-Wno-everything", "-I" + eden, "-I" + compat], ["{tmp}"])))
    regmap = "gpu/vk/tests/regmap_marker_test.cpp"   # (the lowering's renamed-register markers: no number mapped twice)
    out.append((regmap, build_and_run(regmap, ["-O1", "-Wno-everything", "-I" + eden, "-I" + compat], [])))
    return out, ([] if cxx else ["a C++ compiler"])


class Result:
    def __init__(self, returncode, stdout):
        self.returncode, self.stdout = returncode, stdout


def compile_all():
    """Every tracked .py file byte-compiled (in memory: nothing written), so a syntax error anywhere fails"""
    r = subprocess.run(["git", "ls-files", "-z", "--", "*.py"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        return Result(r.returncode, r.stdout)
    files = [f for f in r.stdout.decode("utf-8").split("\0") if f]
    bad = []
    for f in files:
        try:
            with open(os.path.join(ROOT, f), "rb") as src:
                compile(src.read(), f, "exec", dont_inherit=True)
        except (SyntaxError, ValueError) as e:
            bad.append("%s: %s" % (f, e))
    text = "%d files byte-compiled\n" % len(files) + "".join(b + "\n" for b in bad)
    return Result(1 if bad else 0, text.encode())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("names", nargs="*")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    p.add_argument("--timeout", type=int, default=600, help="seconds per check (default 600)")
    p.add_argument("--strict", action="store_true", help="a missing tool fails the check instead of skipping it")
    p.add_argument("--no-skip", action="store_true", help="also run the checks on the skip list")
    a = p.parse_args()
    tools = find_tools()

    checks = []
    tests = sorted(d + "/" + f for d in ("tests", "nx2") for f in os.listdir(os.path.join(ROOT, d))
                   if f.startswith("test_") and f.endswith(".py"))
    for rel in tests:
        oses, reason = SKIP.get(rel, (set(), ""))
        run, missing = python_check(rel, tools, a.timeout)
        c = Check(rel, run)
        if OS in oses and not a.no_skip and not any(n in rel for n in a.names):
            c.why_skip = reason
        elif missing and not a.strict:
            c.why_skip = "not found here: " + ", ".join(missing)
        elif missing:
            c.run = lambda m=missing: Result(1, ("not found: %s (--strict)\n" % ", ".join(m)).encode())
        checks.append(c)
    cpp, missing = cpp_checks(tools, a.timeout)
    for name, run in cpp:
        c = Check(name, run)
        if missing:
            c.why_skip = "not found here: " + ", ".join(missing)
            if a.strict:
                c.why_skip = None
                c.run = lambda m=missing: Result(1, ("not found: %s (--strict)\n" % ", ".join(m)).encode())
        checks.append(c)
    checks.append(Check("(byte-compile every tracked .py)", compile_all))
    if a.names:
        checks = [c for c in checks if any(n in c.name for n in a.names)]

    def one(c):
        t = time.time()
        try:
            r = c.run()
            c.status = "pass" if r.returncode == 0 else "FAIL"
            c.output = r.stdout.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as e:
            c.status = "TIMEOUT"
            c.output = (e.stdout or b"").decode("utf-8", "replace") + "\n(stopped after %d s)\n" % e.timeout
        except Exception as e:   # (a check that could not even start is a failure, not a crash of the runner)
            c.status = "FAIL"
            c.output = "%s: %s\n" % (type(e).__name__, e)
        c.secs = time.time() - t
        return c

    to_run = [c for c in checks if not c.why_skip]
    t0 = time.time()
    print("%d checks, %d skipped, %d at once (%s)" % (len(to_run), len(checks) - len(to_run), a.jobs, OS), flush=True)
    with concurrent.futures.ThreadPoolExecutor(max(1, a.jobs)) as ex:
        for c in concurrent.futures.as_completed([ex.submit(one, c) for c in to_run]):
            c = c.result()
            print("%-7s %6.1fs  %s" % (c.status, c.secs, c.name), flush=True)
    wall = time.time() - t0

    failed = [c for c in to_run if c.status != "pass"]
    gh = os.environ.get("GITHUB_ACTIONS") == "true"
    for c in failed:
        print(("::group::" if gh else "\n==== ") + "%s %s" % (c.status, c.name), flush=True)
        print(c.output.rstrip())
        if gh:
            print("::endgroup::")
            print("::error title=%s %s::see the step's log for its output" % (c.status, c.name))

    order = sorted(to_run, key=lambda c: -c.secs) + sorted((c for c in checks if c.why_skip), key=lambda c: c.name)
    passed = sum(c.status == "pass" for c in to_run)
    head = "%d passed, %d failed, %d skipped in %.0f s (%s, %d at once)" % (passed, len(failed), len(checks) - len(to_run),
                                                                         wall, OS, a.jobs)
    print("\n%-8s %8s  %s" % ("result", "time", "check"))
    for c in order:
        print("%-8s %8s  %s%s" % (c.status, "" if c.why_skip else "%.1fs" % c.secs, c.name,
                                  "  (%s)" % c.why_skip if c.why_skip else ""))
    print(head, flush=True)

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        mark = {"pass": "pass", "FAIL": "**FAIL**", "TIMEOUT": "**TIMEOUT**", "skip": "skip"}
        lines = ["### Tests (%s): %s" % (OS, head), "", "| Result | Time | Check | Note |", "|---|---:|---|---|"]
        for c in order:
            note = c.why_skip or ""
            if c.status in ("FAIL", "TIMEOUT"):
                last = [l for l in c.output.strip().splitlines() if l.strip()][-1:] or [""]
                note = "`%s`" % last[0].replace("|", "\\|").replace("`", "'")[:160]
            lines.append("| %s | %s | `%s` | %s |" % (mark[c.status], "" if c.why_skip else "%.1f s" % c.secs, c.name, note))
        with open(summary, "a", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
