"""Hookshot Builder for Linux: what builder/HookshotBuilder (WPF) is on Windows, in Tk. It runs nx2/make_game.py and
follows its @@ lines (make_game.py's docstring), then offers to start the game. The AppImage starts it (builder/linux/AppRun),
with the compiler, sysroot and libraries it brings in the environment.

    hookshot_builder.py                     the window. Its button is Build, Update, Play, or Update builder
    hookshot_builder.py --play              the game built last time, if there is one (else the window)
    hookshot_builder.py --launch <folder>   that folder's launcher: Play, and Update when a newer release is out
    hookshot_builder.py --update <folder>   download the new release (only parts that changed) and build the folder again
"""
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("HOOKSHOT_TOOLS") or os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "nx2"))
sys.path.insert(0, os.path.join(ROOT, "third_party", "a64-kit", "tools"))
import linux_update  # noqa: E402  (the release check and the part download)
import make_game  # noqa: E402  (LANGUAGES, BUILD_ID)
import toolchain  # noqa: E402  (default_jobs: RX_JOBS)

CONFIG = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "hookshot", "builder.ini")
STAGES = {"read": "Reading your game files", "lift": "Translating the game to PC code", "compile": "Compiling",
          "finish": "Putting the game folder together"}


def left_text(seconds):
    """The countdown make_game.py and build.py work out from this PC's earlier builds."""
    if seconds < 45:
        return "less than a minute left"
    if seconds < 90:
        return "about a minute left"
    minutes = int(round(seconds / 60))
    return "about %d h %d min left" % (minutes // 60, minutes % 60) if minutes >= 60 else "about %d minutes left" % minutes


def load_config():
    cfg = {}
    try:
        for line in open(CONFIG, encoding="utf-8"):
            k, eq, v = line.rstrip("\n").partition("=")
            if eq:
                cfg[k] = v
    except OSError:
        pass
    return cfg


def save_config(cfg):
    os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
    with open(CONFIG, "w", encoding="utf-8") as f:
        f.writelines("%s=%s\n" % kv for kv in cfg.items())


def game_ready(folder):
    return linux_update.game_exe(folder) is not None


def play(folder):
    """the game, on its own (it outlives this window), from its folder. The launcher already asked about updates."""
    exe = linux_update.game_exe(folder)
    if not exe:
        return
    env = {k: v for k, v in os.environ.items() if not k.startswith(("HOOKSHOT_", "PYTHON", "APPDIR", "APPIMAGE", "ARGV0", "OWD"))}
    env.pop("LD_LIBRARY_PATH", None)
    env["HOOKSHOT_NO_UPDATE_CHECK"] = "1"
    subprocess.Popen([exe], cwd=folder, env=env, start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def desktop_exec(path):
    """One Exec argument. A % in the path is a field code, so "100% Games" is rejected and the menu entry does not run.
    Inside the quotes the Desktop Entry spec wants the characters double quote, backtick, dollar and backslash each
    preceded by a backslash, and every backslash written twice because the file's string escaping is applied first
    (so a literal backslash is four, and a dollar is two backslashes and the dollar)."""
    escaped = path.replace("\\", "\\\\\\\\")
    for char in ('"', "`", "$"):
        escaped = escaped.replace(char, "\\\\" + char)
    return '"%s"' % escaped.replace("%", "%%")


APP_ID = "io.github.NotRexed.Hookshot"   # (the game's window names itself so: host/posix/display_sdl.c's HOS_APP_ID)


def add_to_menu(folder):
    """an entry in the applications menu that starts the game"""
    icon = os.path.join(folder, "hookshot.png")
    src = os.path.join(ROOT, "assets", "hookshot.png")
    if os.path.exists(src):
        shutil.copyfile(src, icon)
    apps = os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "applications")
    os.makedirs(apps, exist_ok=True)
    with open(os.path.join(apps, "hookshot.desktop"), "w", encoding="utf-8") as f:
        f.write("[Desktop Entry]\nType=Application\nName=Hookshot\nComment=The Legend of Zelda: Ocarina of Time, built from your copy\n"
                "Exec=%s\nPath=%s\nIcon=%s\nCategories=Game;\nTerminal=false\nStartupWMClass=%s\n"
                % (desktop_exec(os.path.join(folder, "hookshot")), folder, icon, APP_ID))


def free_gb(path):
    path = os.path.expanduser(path or "")   # (the field takes ~/Games/Hookshot, as build() and apply_mode() read it)
    while path and not os.path.exists(path):
        path = os.path.dirname(path)
    try:
        return shutil.disk_usage(path or "/").free / 2 ** 30
    except OSError:
        return None


def xft_dpi():
    """the DPI KDE and GNOME hand X apps when the desktop is scaled. Tk on X11 and XWayland never reads it."""
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    try:
        out = subprocess.run(["xrdb", "-query"], env=env, capture_output=True, text=True, timeout=2).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    found = re.search(r"^Xft\.dpi:\s*([0-9.]+)", out, re.M)
    return float(found.group(1)) if found else None


def ui_scale(screen_height, dpi=None, environ=os.environ):
    """how much bigger than 96 DPI to draw the window. HOOKSHOT_UI_SCALE, then GDK_SCALE or QT_SCALE_FACTOR above 1
    (Wayland sessions export GDK_SCALE=1 whatever the screen), then Xft.dpi; with none of them (gamescope, a bare X
    session) a 1440p or 4K screen is still scaled by its height."""
    for name, least in (("HOOKSHOT_UI_SCALE", 0), ("GDK_SCALE", 1), ("QT_SCALE_FACTOR", 1)):
        try:
            value = float(environ.get(name, ""))
        except ValueError:
            continue
        if value > least:
            return min(max(value, 0.5), 4.0)
    if dpi:
        return min(max(dpi / 96, 0.5), 4.0)
    return min(screen_height / 1080, 4.0) if screen_height >= 1440 else 1.0


def _arg(name):
    args = sys.argv[1:]
    if name in args:
        index = args.index(name)
        if index + 1 < len(args):
            return args[index + 1]
    return None


class Builder(tk.Tk):
    def __init__(self, launch=None, update=None, fetched=False, tag=None):
        super().__init__()
        self.title("Hookshot Builder")
        self.apply_scale()
        self.minsize(self.px(620), self.px(420))
        self.cfg = load_config()
        self.proc = None
        self.lines = queue.Queue()
        self.started = 0.0
        self.launch_folder = launch
        self.update_folder = update
        self.fetched = fetched
        self.update_tag = tag
        self.mode = "build"
        self.latest = None
        self.source_picked = False
        self.tools = linux_update.tools_tag(linux_update.running_root() or ROOT)
        self.status = tk.StringVar()
        self.kind = tk.StringVar(value=self.cfg.get("kind", "nup"))
        self.nup = tk.StringVar(value=self.cfg.get("nup", ""))
        self.romfs = tk.StringVar(value=self.cfg.get("romfs", ""))
        self.exefs = tk.StringVar(value=self.cfg.get("exefs", ""))
        self.out = tk.StringVar(value=self.cfg.get("out", os.path.expanduser("~/Games/Hookshot")))
        names = [n for _, n in make_game.LANGUAGES]
        code = self.cfg.get("language", "en-US")
        self.language = tk.StringVar(value=dict(make_game.LANGUAGES).get(code, names[0]))
        self.check_text = tk.StringVar()
        self.space_text = tk.StringVar()
        self.stage_text = tk.StringVar()
        self.time_text = tk.StringVar()
        self.pad = {"padx": self.px(12), "pady": self.px(4)}
        self.page = None
        self.build_button = None
        if update is not None:
            self.after(200, self.begin_update)
        elif launch:
            self.show_launch(launch)
        else:
            self.show_form()
            self.after(200, self.lookup_latest)
        self.after(100, self.pump)
        self.protocol("WM_DELETE_WINDOW", self.close)

    def apply_scale(self):
        """Fonts sized in points follow tk scaling; a named font sized in pixels (Tk 8.6 on X11: -12) does not, so it is
        multiplied."""
        self.scale = ui_scale(self.winfo_screenheight(), xft_dpi())
        if self.scale == 1.0:
            return
        self.tk.call("tk", "scaling", self.scale * 96 / 72)
        for name in self.tk.splitlist(self.tk.call("font", "names")):
            size = int(self.tk.call("font", "configure", name, "-size"))
            if size < 0:
                self.tk.call("font", "configure", name, "-size", -max(1, round(-size * self.scale)))

    def px(self, n):
        return int(round(n * self.scale))

    # ---- the form ----
    def show_form(self):
        self.title("Hookshot Builder")
        if self.page:
            self.page.destroy()
        f = self.page = ttk.Frame(self)
        f.pack(fill="both", expand=True, pady=self.px(8))
        f.columnconfigure(1, weight=1)
        ttk.Label(f, text="Hookshot", font=("TkDefaultFont", 16, "bold")).grid(row=0, column=0, columnspan=3, sticky="w", **self.pad)
        ttk.Label(f, text="Turns your own copy of Ocarina of Time into a game folder you can start on PC.").grid(
            row=1, column=0, columnspan=3, sticky="w", **self.pad)
        if game_ready(self.cfg.get("out")):
            ttk.Button(f, text="Play the game built in %s" % self.cfg["out"], command=lambda: self.play_and_close(self.cfg["out"])).grid(
                row=2, column=0, columnspan=3, sticky="we", **self.pad)
        ttk.Label(f, text="Game files", font=("TkDefaultFont", 10, "bold")).grid(row=3, column=0, sticky="w", **self.pad)
        kinds = ttk.Frame(f)
        kinds.grid(row=3, column=1, columnspan=2, sticky="w")
        ttk.Radiobutton(kinds, text=".nup file", value="nup", variable=self.kind, command=self.show_form).pack(side="left")
        ttk.Radiobutton(kinds, text="romfs and exefs", value="folders", variable=self.kind, command=self.show_form).pack(side="left", padx=self.px(12))
        row = 4
        if self.kind.get() == "nup":
            self.path_row(f, row, ".nup file", self.nup, lambda: self.pick_file(self.nup))
        else:
            self.path_row(f, row, "romfs", self.romfs, lambda: self.pick_dir(self.romfs))
            row += 1
            self.path_row(f, row, "exefs", self.exefs, lambda: self.pick_dir(self.exefs))
        row += 1
        ttk.Label(f, textvariable=self.check_text, wraplength=self.px(560)).grid(row=row, column=0, columnspan=3, sticky="w", **self.pad)
        row += 1
        self.path_row(f, row, "Save the game to", self.out, lambda: self.pick_dir(self.out))
        row += 1
        ttk.Label(f, text="Language").grid(row=row, column=0, sticky="w", **self.pad)
        ttk.Combobox(f, textvariable=self.language, values=[n for _, n in make_game.LANGUAGES], state="readonly").grid(
            row=row, column=1, sticky="w", **self.pad)
        row += 1
        ttk.Label(f, text="Takes about 45 minutes and needs 60 GB.").grid(row=row, column=0, columnspan=3, sticky="w", **self.pad)
        row += 1
        ttk.Label(f, textvariable=self.space_text).grid(row=row, column=0, columnspan=3, sticky="w", **self.pad)
        row += 1
        ttk.Label(f, textvariable=self.status, wraplength=self.px(560)).grid(row=row, column=0, columnspan=3, sticky="w", **self.pad)
        row += 1
        self.build_button = ttk.Button(f, text="Build", command=self.build)
        self.build_button.grid(row=row, column=0, columnspan=3, sticky="we", padx=self.px(12), pady=self.px(12))
        self.out.trace_add("write", lambda *a: self.update_space())
        self.update_space()
        self.apply_mode()

    def path_row(self, f, row, label, var, browse):
        ttk.Label(f, text=label).grid(row=row, column=0, sticky="w", **self.pad)
        ttk.Entry(f, textvariable=var).grid(row=row, column=1, sticky="we", **self.pad)
        ttk.Button(f, text="Browse", command=browse).grid(row=row, column=2, **self.pad)

    def pick_file(self, var):
        p = filedialog.askopenfilename(title="Your game's .nup", filetypes=[("Switch 2 game package", "*.nup"), ("All files", "*")])
        if p:
            var.set(p)
            self.source_picked = True
            self.apply_mode()
            self.check_version()

    def pick_dir(self, var):
        p = filedialog.askdirectory(title="Choose a folder", initialdir=var.get() or os.path.expanduser("~"))
        if p:
            var.set(p)
            if var is self.exefs:
                self.source_picked = True
                self.apply_mode()
                self.check_version()

    def update_space(self):
        gb = free_gb(self.out.get())
        self.space_text.set("%s has %.0f GB free." % (self.out.get(), gb) if gb is not None else "")
        self.apply_mode()

    def check_version(self):
        args = ["--nup", self.nup.get()] if self.kind.get() == "nup" else ["--exefs", self.exefs.get()]
        self.check_text.set("Checking the game version...")

        def run():
            r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "nx2", "make_game.py"), "--check"] + args,
                               capture_output=True, text=True, cwd=ROOT)
            said = []
            for l in r.stdout.splitlines():
                t = make_game.reported(l)
                if t is not None:
                    said.append(t)
            tail = [l for l in (r.stderr or "").splitlines() if l.strip()]
            text = said[-1] if said else (tail[-1] if tail else "The version could not be read.")
            self.lines.put(("check", text))
        threading.Thread(target=run, daemon=True).start()

    def lookup_latest(self):
        def run():
            forced = os.environ.get("HOOKSHOT_LATEST_TAG")
            tag = None if forced == "none" else (forced or linux_update.latest_tag())
            self.lines.put(("latest", tag or ""))
        threading.Thread(target=run, daemon=True).start()

    def apply_mode(self):
        if self.launch_folder and self.build_button is None:
            self.apply_launch()
            return
        folder = self.out.get().strip()
        game, kept = linux_update.game_version(os.path.expanduser(folder)) if folder else (None, False)
        self.mode = linux_update.choose_mode(game, self.tools, self.latest, self.source_picked, kept)
        button = self.build_button
        if button is None:
            return
        try:
            if not button.winfo_exists():
                return
        except tk.TclError:
            return
        button.configure(text={"update-game": "Update", "up-to-date": "Play", "update-builder": "Update builder",
                                "build": "Build"}[self.mode])
        target = linux_update.update_target(self.latest, self.tools)
        if self.mode == "update-game":
            self.status.set("Hookshot %s is available. Your game is %s. Update downloads only what changed, then compiles what changed."
                            % (target, game))
        elif self.mode == "up-to-date" and self.latest is None:
            self.status.set("Your game is %s, as this builder. GitHub did not answer, so newer versions could not be checked." % game)
        elif self.mode == "up-to-date":
            self.status.set("%s is the latest version." % game)
        elif self.mode == "update-builder":
            self.status.set("Hookshot Builder %s is available (this one is %s). Update it first, then build." % (self.latest, self.tools or "this one"))
        else:
            self.status.set("")

    def apply_launch(self):
        if not self.launch_folder or not hasattr(self, "launch_status"):
            return
        game, kept = linux_update.game_version(self.launch_folder)
        target = linux_update.update_target(self.latest, self.tools)
        show = False
        if target and game and game != target and kept:
            self.launch_status.set("Update available: %s" % target)
            show = True
        elif target and game and game != target:
            self.launch_status.set("%s is out. Build this folder again with Hookshot Builder to get it." % target)
        elif game and self.latest is None:
            self.launch_status.set("Up to date with this builder (GitHub did not answer)")
        elif game:
            self.launch_status.set("Up to date")
        else:
            self.launch_status.set("Could not check for updates (GitHub did not answer)" if self.latest is None else "Hookshot")
        if hasattr(self, "launch_update"):
            self.launch_update.pack(side="right", padx=self.px(4)) if show else self.launch_update.pack_forget()

    def show_launch(self, folder):
        if self.page:
            self.page.destroy()
        self.title("Hookshot")
        f = self.page = ttk.Frame(self)
        f.pack(fill="both", expand=True, pady=self.px(8))
        game, _kept = linux_update.game_version(folder)
        ttk.Label(f, text="Hookshot %s" % game if game else "Hookshot", font=("TkDefaultFont", 16, "bold")).pack(anchor="w", **self.pad)
        ttk.Label(f, text=folder, wraplength=self.px(560)).pack(anchor="w", **self.pad)
        self.launch_status = tk.StringVar(value="Checking for updates...")
        ttk.Label(f, textvariable=self.launch_status, wraplength=self.px(560)).pack(anchor="w", **self.pad)
        buttons = ttk.Frame(f)
        buttons.pack(anchor="e", **self.pad)
        self.launch_update = ttk.Button(buttons, text="Update", command=lambda: self.begin_update(folder))
        ttk.Button(buttons, text="Play", command=lambda: self.play_and_close(folder)).pack(side="right", padx=self.px(4))
        ttk.Button(buttons, text="Open folder", command=lambda: subprocess.Popen(["xdg-open", folder])).pack(side="right", padx=self.px(4))
        self.lookup_latest()

    def begin_update(self, folder=None):
        if folder is not None:
            self.update_folder = folder
        self.show_progress("Updating your game" if self.update_folder else "Updating Hookshot Builder")
        folder = self.update_folder
        # (a game older than this builder is built again with it: nothing to download, and GitHub need not answer. So is one
        # from a builder that is not an older release, e.g. from the Actions tab: issue 902)
        target = self.update_tag or linux_update.update_target(self.latest, self.tools)
        fetched = self.fetched or bool(folder and self.tools and target == self.tools)

        def run():
            try:
                if not fetched:
                    root = linux_update.running_root()
                    if root is None:
                        raise RuntimeError("These tools are a source checkout, so they cannot download a release. Start the AppImage to update.")
                    manifest = linux_update.startup_manifest(root)
                    if manifest.tag != linux_update.tools_tag(root):
                        source = linux_update.read_manifest(root)
                        apprun = linux_update.install(manifest, root, source, note=lambda message: self.lines.put(("note", message)))
                        for name in ("APPDIR", "APPIMAGE", "ARGV0", "OWD", "LD_LIBRARY_PATH"):
                            os.environ.pop(name, None)
                        args = [apprun]
                        if folder:
                            args += ["--update", folder, "--tag", manifest.tag, "--fetched"]
                        os.execv(apprun, args)
                self.lines.put(("do-build", folder or ""))
            except Exception as error:
                self.lines.put(("update-error", str(error)))

        threading.Thread(target=run, daemon=True).start()

    def start_make_update(self, folder):
        if not folder:
            self.show_form()
            self.status.set("This builder is the latest version.")
            self.lookup_latest()
            return
        cmd = [sys.executable, "-I", os.path.join(ROOT, "nx2", "make_game.py"), "--update", folder]
        self.heading.configure(text="Updating your game")
        self.stage_text.set("Compiling what changed")
        self.started = time.time()
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                                     encoding="utf-8", errors="replace", start_new_session=True)
        threading.Thread(target=self.read, args=(self.proc,), daemon=True).start()
        self.tick()

    # ---- the build ----
    def build(self):
        mode = getattr(self, "mode", "build")
        if mode == "up-to-date":
            folder = os.path.expanduser(self.out.get().strip())
            if game_ready(folder):
                self.play_and_close(folder)
            return
        if mode == "update-game":
            self.fetched = False
            self.begin_update(os.path.expanduser(self.out.get().strip()))
            return
        if mode == "update-builder":
            self.fetched = False
            self.update_folder = ""
            self.begin_update("")
            return
        out = os.path.expanduser(self.out.get().strip())
        if self.kind.get() == "nup":
            if not os.path.isfile(self.nup.get()):
                return messagebox.showerror("Hookshot", "Choose your game's .nup file.")
            src = ["--nup", self.nup.get()]
        else:
            if not (os.path.isdir(self.romfs.get()) and os.path.isdir(self.exefs.get())):
                return messagebox.showerror("Hookshot", "Choose your game's romfs and exefs folders.")
            src = ["--romfs", self.romfs.get(), "--exefs", self.exefs.get()]
        if not out:
            return messagebox.showerror("Hookshot", "Choose where to save the game.")
        try:
            jobs = make_game.requested_jobs(os.environ.get("HOOKSHOT_JOBS"))
            if jobs is None and os.environ.get("RX_JOBS"):   # (the older setting, still honoured)
                jobs = toolchain.default_jobs()
        except ValueError as error:
            return messagebox.showerror("Hookshot", str(error))
        code = next((c for c, n in make_game.LANGUAGES if n == self.language.get()), "en-US")
        self.cfg.update(kind=self.kind.get(), nup=self.nup.get(), romfs=self.romfs.get(), exefs=self.exefs.get(), out=out, language=code)
        save_config(self.cfg)
        # (issue 531) Without HOOKSHOT_JOBS or RX_JOBS, make_game chooses as on Windows, from the memory free now; this window chose
        # fewer, 80% of the total memory at 3.5 GB each, and the CPU sat at 29%
        cmd = [sys.executable, "-I", os.path.join(ROOT, "nx2", "make_game.py")] + src + ["--out", out, "--language", code] + \
            ([] if jobs is None else ["--jobs", str(jobs)])
        self.show_progress()
        self.started = time.time()
        self.proc = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                                     encoding="utf-8", errors="replace", start_new_session=True)
        threading.Thread(target=self.read, args=(self.proc,), daemon=True).start()

    def read(self, proc):
        for line in proc.stdout:
            self.lines.put(("line", line.rstrip("\n")))
        self.lines.put(("exit", proc.wait()))

    def show_progress(self, title="Building"):
        if self.page:
            self.page.destroy()
        self.changed, self.compiled_files = -1, 0
        self.eta = None
        self.compile_started = None
        self.times = None
        f = self.page = ttk.Frame(self)
        f.pack(fill="both", expand=True, pady=self.px(8))
        f.columnconfigure(0, weight=1)
        f.rowconfigure(5, weight=1)
        self.heading = ttk.Label(f, text=title, font=("TkDefaultFont", 16, "bold"))
        self.heading.grid(row=0, column=0, sticky="w", **self.pad)
        ttk.Label(f, text="This takes a while, and your PC stays usable.").grid(row=1, column=0, sticky="w", **self.pad)
        ttk.Label(f, textvariable=self.stage_text).grid(row=2, column=0, sticky="w", **self.pad)
        self.bar = ttk.Progressbar(f, maximum=1.0, mode="determinate")
        self.bar.grid(row=3, column=0, sticky="we", **self.pad)
        ttk.Label(f, textvariable=self.time_text).grid(row=4, column=0, sticky="w", **self.pad)
        self.log = tk.Text(f, height=12, wrap="none", font=("TkFixedFont", 9))
        self.log.grid(row=5, column=0, sticky="nsew", **self.pad)
        self.buttons = ttk.Frame(f)
        self.buttons.grid(row=6, column=0, sticky="e", **self.pad)
        ttk.Button(self.buttons, text="Cancel", command=self.cancel).pack(side="right")
        self.stage_text.set("Starting")
        self.tick()

    def tick(self):
        if self.proc and self.proc.poll() is None:
            s = int(time.time() - self.started)
            text = "Elapsed %d:%02d:%02d" % (s // 3600, s // 60 % 60, s % 60)
            if self.compile_started:
                c = int(time.time() - self.compile_started)
                text += " · compiling %d:%02d" % (c // 60, c % 60)
            if self.eta is not None:
                text += " · " + left_text(self.eta)
            self.time_text.set(text)
            self.after(1000, self.tick)

    def pump(self):
        try:
            while True:
                kind, v = self.lines.get_nowait()
                if kind == "check":
                    self.check_text.set(v)
                elif kind == "latest":
                    self.latest = v or None
                    self.apply_mode()
                elif kind == "note":
                    if hasattr(self, "stage_text"):
                        self.stage_text.set(v)
                elif kind == "do-build":
                    self.start_make_update(v)
                elif kind == "update-error":
                    if hasattr(self, "stage_text"):
                        self.stage_text.set(v + " Your game folder is unchanged.")
                    if hasattr(self, "buttons"):
                        for widget in self.buttons.winfo_children():
                            widget.destroy()
                        ttk.Button(self.buttons, text="Back", command=self.show_form).pack(side="right", padx=self.px(4))
                elif kind == "line":
                    self.on_line(v)
                elif kind == "exit":
                    self.on_exit(v)
        except queue.Empty:
            pass
        self.after(100, self.pump)

    def on_line(self, line):
        if line.startswith("@@stage "):
            stage = line.split(" ", 2)[1]
            self.stage_text.set(STAGES.get(stage, line.split(" ", 2)[-1]))
            self.bar.configure(mode="determinate", value=0)
            if stage == "compile":
                self.compile_started = time.time()
        elif line.startswith("@@progress "):
            try:
                self.bar.configure(value=float(line.split()[1]))
            except ValueError:
                pass
        elif line.startswith("@@plan "):
            parts = line.split(" ")
            if len(parts) == 3:
                try:
                    self.changed, total = int(parts[1]), int(parts[2])
                except ValueError:
                    pass
                else:
                    self.stage_text.set("Compiling: everything is already built" if not self.changed else
                                        "Compiling %d changed files of %d" % (self.changed, total) if self.changed < total else
                                        "Compiling %d files" % total)
            return
        elif line.startswith("@@file "):
            parts = line.split(" ")
            if len(parts) == 4 and parts[3] in ("new", "again"):
                self.compiled_files += 1
                if self.changed > 0:
                    self.stage_text.set("Compiling %d of %d changed files" % (self.compiled_files, self.changed))
            return
        elif line.startswith("@@eta "):
            try:
                self.eta = float(line.split()[1])
            except (ValueError, IndexError):
                pass
            return
        elif line.startswith("@@times "):
            parts = line.split(" ")
            if len(parts) == 4:
                try:
                    self.times = (float(parts[1]), float(parts[2]), float(parts[3]))
                except ValueError:
                    pass
            return
        elif line.startswith("@@done "):
            self.built = line[7:]
        elif line.startswith("@@error "):
            self.error = line[8:]
        if hasattr(self, "log"):
            self.log.insert("end", line + "\n")
            self.log.see("end")

    def on_exit(self, code):
        self.proc = None
        for w in self.buttons.winfo_children():
            w.destroy()
        folder = getattr(self, "built", None)
        if code == 0 and folder and game_ready(folder):
            s = int(time.time() - self.started)
            text = "Your game is ready, built in %d:%02d" % (s // 3600, s // 60 % 60)
            if self.times:
                total, compiled, previous = self.times
                text += ", compiled in %d:%02d" % (int(compiled) // 60, int(compiled) % 60)
                if previous > 0:
                    text += " · last time %d:%02d" % (int(previous) // 3600, int(previous) // 60 % 60)
            self.stage_text.set(text)
            self.bar.configure(value=1.0)
            try:
                add_to_menu(folder)
            except OSError:
                pass
            ttk.Button(self.buttons, text="Play", command=lambda: self.play_and_close(folder)).pack(side="right", padx=self.px(4))
            ttk.Button(self.buttons, text="Open folder", command=lambda: subprocess.Popen(["xdg-open", folder])).pack(side="right", padx=self.px(4))
        else:
            self.stage_text.set("The build stopped: %s" % getattr(self, "error", "see the lines above"))
            ttk.Button(self.buttons, text="Back", command=self.show_form).pack(side="right", padx=self.px(4))

    def cancel(self):
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            self.error = "Build cancelled. You can start again from the same folder."

    def play_and_close(self, folder):
        play(folder)
        self.destroy()

    def close(self):
        if self.proc and self.proc.poll() is None:
            if not messagebox.askyesno("Hookshot", "Stop the build and close?"):
                return
            os.killpg(self.proc.pid, signal.SIGTERM)
        self.destroy()


def main():
    if "--play" in sys.argv[1:]:
        folder = _arg("--play") or load_config().get("out")
        if game_ready(folder):
            play(folder)
            return
    launch = _arg("--launch")
    update = _arg("--update")
    if update is not None:
        Builder(update=update, fetched="--fetched" in sys.argv[1:], tag=_arg("--tag")).mainloop()
    elif launch:
        Builder(launch=launch).mainloop()
    else:
        Builder().mainloop()


if __name__ == "__main__":
    main()
