"""nx2/make_game.py and NVIDIA DLSS: the runtime placed in the game folder's ngx folder (never beside the exe), and the
fetch the Hookshot Builder's DLSS tick asks for, which needs NVIDIA's license accepted. No network, no game.

Run with: python -I tests/test_make_game_ngx.py
"""
import importlib.util
import io
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_game_ngx_under_test", ROOT / "nx2" / "make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class NgxRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hookshot-make-game-ngx-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk"
        self.out = self.root / "game"
        (self.out / "bin").mkdir(parents=True)
        self.ngx = make_game.ngx_script()

    def sdk_files(self, platform, runtime=True):
        lib = Path(self.ngx.library(str(self.sdk), platform))
        lib.parent.mkdir(parents=True, exist_ok=True)
        lib.write_bytes(b"lib")
        if runtime:
            rt = Path(self.ngx.runtime(str(self.sdk), platform))
            rt.parent.mkdir(parents=True, exist_ok=True)
            rt.write_bytes(b"runtime")

    def place(self, platform, sdk=None):
        env = {"HOOKSHOT_NGX_SDK": str(self.sdk) if sdk is None else sdk}
        with mock.patch.object(make_game, "LINUX", platform == "linux"), mock.patch.dict(os.environ, env), \
                mock.patch("sys.stdout", io.StringIO()):
            return make_game.place_ngx_runtime(str(self.out))

    def test_windows_runtime_goes_to_the_ngx_folder(self):
        self.sdk_files("windows")
        dst = self.place("windows")
        self.assertEqual(Path(dst), self.out / "ngx" / "nvngx_dlss.dll")
        self.assertEqual(Path(dst).read_bytes(), b"runtime")
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ["bin", "ngx"])
        self.assertEqual(list((self.out / "bin").iterdir()), [])   # (nothing beside the exe)
        self.assertFalse([p for p in self.out.rglob("*") if p.name.lower() == "_nvngx.dll"])

    def built(self, platform, sdk=None):
        env = {"HOOKSHOT_NGX_SDK": str(self.sdk) if sdk is None else sdk}
        with mock.patch.object(make_game, "LINUX", platform == "linux"), mock.patch.dict(os.environ, env):
            return make_game.ngx_built()

    def test_jitter_only_in_a_build_with_the_sdk(self):
        # (make_game sets OOT_DLSS_JITTER=1 for the lift only then: nx2/oot_jitter.py)
        self.assertFalse(self.built("linux", sdk=""))
        self.assertFalse(self.built("linux"))   # (no library yet)
        self.sdk_files("linux", runtime=False)
        self.assertTrue(self.built("linux"))
        self.assertFalse(self.built("windows"))   # (another platform's library only)
        import sys
        sys.path.insert(0, str(ROOT / "nx2"))
        try:
            import oot_jitter
        finally:
            sys.path.pop(0)
        with mock.patch.dict(os.environ, {"OOT_DLSS_JITTER": "1"}):
            self.assertTrue(oot_jitter.wanted())
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertFalse(oot_jitter.wanted())

    def test_linux_runtime(self):
        self.sdk_files("linux")
        self.assertEqual(Path(self.place("linux")), self.out / "ngx" / "libnvidia-ngx-dlss.so.310.9.16")

    def test_nothing_without_the_sdk_or_its_platform(self):
        self.assertIsNone(self.place("windows", sdk=""))
        self.sdk_files("linux")
        self.assertIsNone(self.place("windows"))   # (a Linux SDK folder: no Windows library, no DLSS in this build)
        self.sdk_files("windows", runtime=False)
        self.assertIsNone(self.place("windows"))
        self.assertFalse((self.out / "ngx").exists())


class FetchGateTests(unittest.TestCase):
    def fetch(self, accept, sdk="sdk"):
        calls = []
        ngx = SimpleNamespace(NOTICE="notice", fetch=lambda *a, **k: calls.append(a))
        with mock.patch.object(make_game, "ngx_script", return_value=ngx), mock.patch.dict(os.environ, {"HOOKSHOT_NGX_SDK": sdk}), \
                mock.patch("sys.stdout", io.StringIO()) as out:
            try:
                make_game.fetch_ngx_sdk(SimpleNamespace(accept_nvidia_license=accept))
            except SystemExit as e:
                return calls, e.code, out.getvalue()
        return calls, 0, out.getvalue()

    def test_needs_the_license_accepted(self):
        calls, code, out = self.fetch(False)
        self.assertEqual((calls, code), ([], 1))
        self.assertIn("@@error", out)
        self.assertIn("license", out)

    def test_needs_the_sdk_folder(self):
        calls, code, _ = self.fetch(True, sdk="")
        self.assertEqual((calls, code), ([], 1))

    def test_fetches_this_platforms_files_after_the_notice(self):
        calls, code, out = self.fetch(True)
        self.assertEqual(code, 0)
        self.assertEqual(calls, [("sdk", make_game.ngx_platform())])
        self.assertTrue(out.startswith("notice"))

    def test_flags_are_off_by_default(self):
        with mock.patch("sys.argv", ["make_game.py", "--out", "x", "--nup", "y"]):
            a = make_game.parse_args()
        self.assertFalse(a.fetch_ngx_sdk)
        self.assertFalse(a.accept_nvidia_license)


if __name__ == "__main__":
    unittest.main()
