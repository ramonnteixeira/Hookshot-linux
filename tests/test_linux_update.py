"""Linux updates download a part only when its checksum changed, and the launcher starts the installed builder.

python -I -B tests/test_linux_update.py
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder" / "linux"))
sys.path.insert(0, str(ROOT / "nx2"))
sys.path.insert(0, str(ROOT / "scripts" / "linux"))
import linux_update  # noqa: E402
import make_parts  # noqa: E402


def appdir(path):
    """A tiny AppDir: one file in each part, plus the launcher script an install copies to AppRun."""
    tools = path / "usr" / "share" / "hookshot"
    (tools / "builder" / "linux").mkdir(parents=True)
    shutil.copyfile(ROOT / "builder" / "linux" / "AppRun", tools / "builder" / "linux" / "AppRun")
    (tools / "VERSION").write_text("v1\n", encoding="utf-8")
    (tools / "nx2").mkdir()
    (tools / "nx2" / "marker.py").write_text("print('one')\n", encoding="utf-8")
    for dest in ("usr/python", "usr/lib/hookshot/emsdk", "usr/lib/hookshot/ffmpeg", "usr/lib/hookshot/sysroot"):
        folder = path / dest
        folder.mkdir(parents=True)
        (folder / "kept").write_text(dest + "\n", encoding="utf-8")
    lib = path / "usr" / "lib" / "hookshot"
    (lib / "hookshot_vk.so").write_bytes(b"vk")
    (lib / "libSDL2-2.0.so.0").write_bytes(b"sdl")
    (path / "AppRun").write_text("#!/bin/sh\n", encoding="utf-8")
    (path / "AppRun").chmod(0o755)


class UpdateTests(unittest.TestCase):
    def test_the_button_matches_the_windows_builder(self):
        self.assertEqual(linux_update.choose_mode("v1", "v2", "v2", False, True), "update-game")
        self.assertEqual(linux_update.choose_mode("v2", "v2", "v2", False, True), "up-to-date")
        self.assertEqual(linux_update.choose_mode("v1", "v1", None, False, True), "up-to-date")
        self.assertEqual(linux_update.choose_mode("v1", "v1", "v2", False, True), "update-game")
        self.assertEqual(linux_update.choose_mode(None, "v1", "v2", False, False), "update-builder")
        self.assertEqual(linux_update.choose_mode(None, "v2", "v2", False, False), "build")
        self.assertEqual(linux_update.choose_mode("v2", "v2", "v2", True, True), "build")

    def test_a_build_that_is_not_an_older_release_is_not_made_to_update(self):
        # (issue 902) an AppImage from the Actions tab is git describe's version, after the latest release: it builds
        ci = "v2.0.5-3-g1a2b3c4"
        self.assertEqual(linux_update.choose_mode(None, ci, "v2.0.5", False, False), "build")
        self.assertEqual(linux_update.choose_mode(None, "1a2b3c4", "v2.0.5", False, False), "build")
        self.assertEqual(linux_update.choose_mode(None, "dev", "v2.0.5", False, False), "build")
        self.assertEqual(linux_update.choose_mode(None, "v2.0.6", "v2.0.5", False, False), "build")
        self.assertEqual(linux_update.choose_mode(None, "v2.1.0-rc.1", "v2.0.5", False, False), "build")
        # (its game is played, not taken back to the release; an older game is built again with this builder)
        self.assertEqual(linux_update.choose_mode(ci, ci, "v2.0.5", False, True), "up-to-date")
        self.assertEqual(linux_update.choose_mode("v2.0.4", ci, "v2.0.5", False, True), "update-game")
        self.assertEqual(linux_update.update_target("v2.0.5", ci), ci)
        # (a game the release built is older than this Actions builder: its Update builds it again with the builder running,
        # nothing downloaded, as for any older game)
        self.assertEqual(linux_update.choose_mode("v2.0.5", ci, "v2.0.5", False, True), "update-game")
        # an older release still gets the newer one
        self.assertEqual(linux_update.choose_mode(None, "v2.0.4", "v2.0.5", False, False), "update-builder")
        self.assertEqual(linux_update.choose_mode(None, "v2.1.0-rc.1", "v2.1.0", False, False), "update-builder")
        self.assertEqual(linux_update.choose_mode(None, "v2.0.10", "v2.0.9", False, False), "build")
        self.assertEqual(linux_update.choose_mode("v2.0.4", "v2.0.4", "v2.0.5", False, True), "update-game")
        self.assertEqual(linux_update.update_target("v2.0.5", "v2.0.4"), "v2.0.5")
        # a build from the Actions tab made after an older release is offered the newer one
        self.assertEqual(linux_update.choose_mode(None, "v2.0.4-3-g1a2b3c4", "v2.0.5", False, False), "update-builder")
        self.assertEqual(linux_update.choose_mode(None, "v2.0.5-3-g1a2b3c4-dirty", "v2.0.5", False, False), "build")
        self.assertEqual(linux_update.choose_mode(None, "v2.1.0-rc.1-2-gabc1234", "v2.1.0", False, False), "update-builder")

    def test_an_rc_build_with_its_own_list_is_not_downgraded_to_the_release(self):
        # (issue 990: a -rc AppImage ships correct tools and a list naming its own parts, while releases/latest is an
        # older release whose zip lacks a file these tools have: reinstalling it every launch lost that file)
        from unittest import mock
        with tempfile.TemporaryDirectory(prefix="hookshot-updates-") as tmp:
            root = Path(tmp) / "rc"
            appdir(root)
            (root / "usr" / "share" / "hookshot" / "VERSION").write_text("v1.1-rc.1\n", encoding="utf-8")
            (root / "hookshot-linux.txt").write_text(
                "tag v1.1-rc.1\npart %s 10 usr/share/hookshot file://%s/rc.zip\nfile nx2/marker.py\n" % ("a" * 64, tmp),
                encoding="utf-8")
            stable = "tag v1.0\npart %s 10 usr/share/hookshot file://%s/old.zip\nfile nx2/marker.py\n" % ("b" * 64, tmp)
            got = linux_update.startup_manifest(str(root), get=lambda url: stable.encode())
            self.assertEqual((got.tag, got.parts[0].sha), ("v1.1-rc.1", "a" * 64))     # (its own: nothing reinstalled)
            newer = "tag v1.2\npart %s 10 usr/share/hookshot file://%s/new.zip\nfile nx2/marker.py\n" % ("c" * 64, tmp)
            got = linux_update.startup_manifest(str(root), get=lambda url: newer.encode())
            self.assertEqual((got.tag, got.parts[0].sha), ("v1.2", "c" * 64))          # (a newer release still wins)
            # a HOOKSHOT_LINUX_LIST is taken even when the tags match (a patched part for a release, issue 990)
            (root / "patched.txt").write_text(
                "tag v1.1-rc.1\npart %s 10 usr/share/hookshot file://%s/patched.zip\nfile nx2/marker.py\n" % ("d" * 64, tmp),
                encoding="utf-8")
            with mock.patch.dict(os.environ, {"HOOKSHOT_LINUX_LIST": str(root / "patched.txt")}):
                got = linux_update.startup_manifest(str(root), get=lambda url: stable.encode())
                self.assertEqual(got.parts[0].sha, "d" * 64)
            # an AppImage at the latest release keeps using its bundled list (nothing was ever reinstalled there)
            release = Path(tmp) / "release"
            appdir(release)     # (VERSION v1)
            (release / "hookshot-linux.txt").write_text(
                "tag v1\npart %s 10 usr/share/hookshot file://%s/release.zip\nfile nx2/marker.py\n" % ("e" * 64, tmp),
                encoding="utf-8")
            same = "tag v1\npart %s 10 usr/share/hookshot file://%s/old.zip\nfile nx2/marker.py\n" % ("b" * 64, tmp)
            got = linux_update.startup_manifest(str(release), get=lambda url: same.encode())
            self.assertEqual(got.parts[0].sha, "e" * 64)
            # and one older than the release still downloads it
            (release / "usr" / "share" / "hookshot" / "VERSION").write_text("v0.9\n", encoding="utf-8")
            got = linux_update.startup_manifest(str(release), get=lambda url: stable.encode())
            self.assertEqual(got.parts[0].sha, "b" * 64)

    def test_a_changed_part_is_downloaded_and_the_rest_are_kept(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-parts-") as tmp:
            root = Path(tmp)
            old = root / "old"
            appdir(old)
            first = root / "parts1"
            argv = sys.argv
            try:
                sys.argv = ["make_parts.py", str(old), "v1", str(first)]
                make_parts.main()
            finally:
                sys.argv = argv
            manifest = linux_update.parse_list((first / "hookshot-linux.txt").read_text(encoding="utf-8"))
            home = root / "home"
            os.environ["HOOKSHOT_BUILDER_HOME"] = str(home)
            self.addCleanup(lambda: os.environ.pop("HOOKSHOT_BUILDER_HOME", None))
            downloaded = []

            def get(url):
                downloaded.append(url)
                name = url.rsplit("/", 1)[-1]
                return (first / name).read_bytes()

            linux_update.install(manifest, str(old), manifest, get=get)
            self.assertEqual(downloaded, [])
            installed = home / "hookshot" / "builder" / "versions" / "v1"
            self.assertTrue((installed / "AppRun").stat().st_mode & stat.S_IXUSR)
            self.assertEqual((installed / "usr" / "share" / "hookshot" / "nx2" / "marker.py").read_text(encoding="utf-8"), "print('one')\n")

            new = root / "new"
            shutil.copytree(old, new)
            (new / "usr" / "share" / "hookshot" / "nx2" / "marker.py").write_text("print('two')\n", encoding="utf-8")
            second = root / "parts2"
            try:
                sys.argv = ["make_parts.py", str(new), "v2", str(second)]
                make_parts.main()
            finally:
                sys.argv = argv
            nxt = linux_update.parse_list((second / "hookshot-linux.txt").read_text(encoding="utf-8"))
            downloaded.clear()

            def get2(url):
                downloaded.append(url)
                name = url.rsplit("/", 1)[-1]
                return (second / name).read_bytes()

            linux_update.install(nxt, str(installed), manifest, get=get2)
            self.assertEqual(len(downloaded), 1)
            self.assertIn("usr-share-hookshot", downloaded[0])
            built = home / "hookshot" / "builder" / "versions" / "v2"
            self.assertEqual((built / "usr" / "share" / "hookshot" / "nx2" / "marker.py").read_text(encoding="utf-8"), "print('two')\n")
            self.assertEqual((built / "usr" / "lib" / "hookshot" / "hookshot_vk.so").read_bytes(), b"vk")
            self.assertFalse((home / "hookshot" / "builder" / "versions" / "v1").exists())

    def test_an_installed_builder_finds_its_own_root(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-root-") as tmp:
            root = Path(tmp) / "versions" / "v1"
            appdir(root)
            saved = {k: os.environ.get(k) for k in ("APPDIR", "HOOKSHOT_TOOLS")}
            os.environ.pop("APPDIR", None)
            os.environ["HOOKSHOT_TOOLS"] = str(root / "usr" / "share" / "hookshot")
            try:
                found = linux_update.running_root()
            finally:
                for k, v in saved.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v
            self.assertEqual(found, str(root))
            self.assertEqual(linux_update.tools_tag(found), "v1")

    @unittest.skipUnless(sys.platform.startswith("linux"), "the Linux folder layout")
    def test_a_packaged_build_puts_the_launcher_in_the_folder_and_the_game_in_bin(self):
        import ast
        from unittest import mock
        import make_game
        main = next(n for n in ast.parse((ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")).body
                    if isinstance(n, ast.FunctionDef) and n.name == "main")
        start = next(i for i, n in enumerate(main.body) if isinstance(n, ast.Assign)
                     and any(isinstance(t, ast.Name) and t.id == "staged" for t in n.targets))
        end = next(i for i, n in enumerate(main.body[start + 1:], start + 1) if isinstance(n, ast.If)
                   and any(isinstance(c, ast.Call) and isinstance(c.func, ast.Name) and c.func.id == "shader_census_cmd"
                           for c in ast.walk(n)))
        finish = compile(ast.Module(body=main.body[start:end], type_ignores=[]), "make_game.py", "exec")
        with tempfile.TemporaryDirectory(prefix="hookshot-finish-") as tmp:
            work, out = Path(tmp) / "work", Path(tmp) / "game"
            built = work / "sa" / "build"
            built.mkdir(parents=True)
            (work / "sa" / "image").mkdir()
            (work / "sa" / "image" / "image.bin").write_bytes(b"image")
            for name in make_game.BESIDE:
                (built / name).write_bytes(b"new " + name.encode())
            out.mkdir()
            for name in make_game.BESIDE:   # (a folder from a builder before the launcher: the game and its libraries in it)
                (out / name).write_bytes(b"old " + name.encode())
            with mock.patch.dict(os.environ, {"HOOKSHOT_TOOLS": str(ROOT)}):
                exec(finish, make_game.__dict__, {"built": str(built), "out": str(out), "work": str(work)})
            self.assertEqual((out / "hookshot").read_bytes(), (ROOT / "builder" / "linux" / "hookshot-folder").read_bytes())
            self.assertTrue((out / "hookshot").stat().st_mode & stat.S_IXUSR)
            self.assertEqual(sorted(p.name for p in out.iterdir()), ["bin", "hookshot", "image.bin"])
            self.assertEqual(sorted(p.name for p in (out / "bin").iterdir()), sorted(make_game.BESIDE))
            self.assertEqual((out / "bin" / "hookshot").read_bytes(), b"new hookshot")

    def test_the_sysroot_path_is_not_part_of_an_object_key(self):
        import build
        handle, name = tempfile.mkstemp()
        os.write(handle, b"int x;\n")
        os.close(handle)
        try:
            one = build.object_key(name, ["--sysroot=/tmp/.mount_aaa/usr/lib/hookshot/sysroot", "-O2"], b"deps")
            two = build.object_key(name, ["--sysroot=/tmp/.mount_bbb/usr/lib/hookshot/sysroot", "-O2"], b"deps")
        finally:
            os.remove(name)
        self.assertEqual(one, two)

    def test_the_folder_launcher_starts_the_builder(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-folder-") as tmp:
            root = Path(tmp)
            game = root / "game"
            (game / "bin").mkdir(parents=True)
            (game / "image.bin").write_bytes(b"image")
            started = root / "started"
            builder = root / "builder"
            builder.write_text("#!/bin/sh\nprintf '%%s\\n' \"$0\" \"$@\" > '%s'\n" % started, encoding="utf-8")
            builder.chmod(0o755)
            (game / "hookshot_version.txt").write_text("version=v1\nbuilder=%s\n" % builder, encoding="utf-8")
            script = game / "hookshot"
            shutil.copyfile(ROOT / "builder" / "linux" / "hookshot-folder", script)
            script.chmod(0o755)
            env = os.environ.copy()
            env["HOOKSHOT_BUILDER_HOME"] = str(root / "none")
            ran = subprocess.run([str(script)], env=env, check=False, text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            lines = started.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[1], "--launch")
            self.assertEqual(lines[2], str(game))

    def test_the_folder_launcher_finds_the_builder_under_the_builder_home(self):
        # HOOKSHOT_BUILDER_HOME is the data directory, as XDG_DATA_HOME is: linux_update installs to
        # <home>/hookshot/builder and host/hos_update.c looks there, so the launcher must too
        with tempfile.TemporaryDirectory(prefix="hookshot-folder-") as tmp:
            root = Path(tmp)
            game = root / "game"
            (game / "bin").mkdir(parents=True)
            home = root / "home"
            started = root / "started"
            installed = home / "hookshot" / "builder"
            (installed / "versions" / "v9").mkdir(parents=True)
            (installed / "current").write_text("v9\n", encoding="utf-8")
            apprun = installed / "versions" / "v9" / "AppRun"
            apprun.write_text("#!/bin/sh\nprintf '%%s\\n' \"$0\" \"$@\" > '%s'\n" % started, encoding="utf-8")
            apprun.chmod(0o755)
            script = game / "hookshot"
            shutil.copyfile(ROOT / "builder" / "linux" / "hookshot-folder", script)
            script.chmod(0o755)
            env = os.environ.copy()
            env["HOOKSHOT_BUILDER_HOME"] = str(home)
            ran = subprocess.run([str(script)], env=env, check=False, text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            lines = started.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[0], str(apprun))
            self.assertEqual(lines[1:], ["--launch", str(game)])

    def test_the_folder_launcher_starts_the_game_when_no_builder_is_installed(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-folder-") as tmp:
            root = Path(tmp)
            game = root / "game"
            (game / "bin").mkdir(parents=True)
            (game / "bin" / "hookshot").write_text("#!/bin/sh\nprintf 'game\\n'\n", encoding="utf-8")
            (game / "bin" / "hookshot").chmod(0o755)
            script = game / "hookshot"
            shutil.copyfile(ROOT / "builder" / "linux" / "hookshot-folder", script)
            script.chmod(0o755)
            env = os.environ.copy()
            env["HOOKSHOT_BUILDER_HOME"] = str(root / "none")
            ran = subprocess.run([str(script)], env=env, check=False, text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            self.assertEqual(ran.stdout.strip(), "game")


if __name__ == "__main__":
    unittest.main()
