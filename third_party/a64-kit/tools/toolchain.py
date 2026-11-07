import os, shutil

HERE = os.path.dirname(os.path.abspath(__file__))


def emsdk():
    return os.environ.get("EMSDK") or (r"C:\emsdk" if os.name == "nt" else os.path.expanduser("~/emsdk"))


def find(tool):
    root = emsdk()
    exts = (".exe", ".bat") if os.name == "nt" else ("",)
    native = tool in ("clang", "clang++", "lld") or tool.startswith("llvm-")
    folder = os.path.join(root, "upstream", "bin") if native else os.path.join(root, "upstream", "emscripten")
    for ext in exts:
        path = os.path.join(folder, tool + ext)
        if os.path.isfile(path):
            return path
    return shutil.which(tool) or os.path.join(folder, tool + exts[0])


def need(path):
    if not os.path.isfile(path):
        raise SystemExit("%s not found: set EMSDK to an emsdk checkout (now %s), or put %s on PATH"
                         % (path, emsdk(), os.path.splitext(os.path.basename(path))[0]))
    return path


def lld_link():
    """emsdk's lld under the name lld-link (lld picks its COFF linker from its own file name), made on first use"""
    path = os.path.join(HERE, "lld-link.exe")
    if not os.path.exists(path):
        shutil.copyfile(need(find("lld")), path)
    return path


def native_target():
    return ["--target=x86_64-pc-windows-msvc", "-fms-extensions", "-D_CRT_SECURE_NO_WARNINGS"] if os.name == "nt" else []


def native_link():
    # (-fuse-ld=lld, found through -B: clang ignores --ld-path for the MSVC target and ran Microsoft's link.exe, which
    # cannot read the renderer's ThinLTO bitcode)
    if os.name == "nt":
        return ["-fuse-ld=lld", "-B" + os.path.dirname(lld_link())]
    # (linux port) lld when it is there: its symbol ordering file is how the hot functions are packed together
    return ["-fuse-ld=lld"] if shutil.which("ld.lld") or os.path.isfile(find("ld.lld")) else []


def default_jobs(gb_per_job=3.5):
    """Parallel compiles that fit in memory: a -O3 compile of one of the game's big lifted chunks takes 3-8 GB, and one
    job per logical core ran 16 GB machines out of memory (22 threads on an Ultra 7 155H). 80% of the physical memory
    at gb_per_job each, at most one a core. RX_JOBS=<n> sets it. (After SkyboundTotk PR 14 by leonelalej.)"""
    if os.environ.get("RX_JOBS"):
        return max(1, int(os.environ["RX_JOBS"]))
    total = 0
    try:
        if os.name == "nt":
            import ctypes

            class MemoryStatus(ctypes.Structure):
                _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + \
                           [(f, ctypes.c_ulonglong) for f in ("phys", "avail_phys", "page", "avail_page", "virt",
                                                              "avail_virt", "avail_ext")]
            m = MemoryStatus()
            m.length = ctypes.sizeof(m)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
                total = m.phys
        else:
            total = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    except (OSError, ValueError, AttributeError):
        pass
    cores = os.cpu_count() or 1
    if not total:
        return cores
    return max(1, min(cores, int(total / 2 ** 30 * 0.8 / gb_per_job)))
