"""The Linux builder's "N GB free" line must measure the disk the game folder is on, also when the field holds ~/...

The field takes a path such as ~/Games/Hookshot (build() and apply_mode() expand the ~), but free_gb walked up the
unexpanded text, never found a folder, and reported the free space of / instead.

python -I -B tests/test_builder_free_space.py
"""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load_builder():
    for name in ("tkinter", "tkinter.filedialog", "tkinter.messagebox", "tkinter.ttk"):
        if name not in sys.modules:
            sys.modules[name] = types.ModuleType(name)
    tk = sys.modules["tkinter"]
    tk.Tk = type("Tk", (), {})
    tk.filedialog = sys.modules["tkinter.filedialog"]
    tk.messagebox = sys.modules["tkinter.messagebox"]
    tk.ttk = sys.modules["tkinter.ttk"]
    spec = importlib.util.spec_from_file_location("hookshot_builder_free_space", ROOT / "builder" / "linux" / "hookshot_builder.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load_builder()


def measured(path, home):
    """The folder free_gb asked the disk about"""
    asked = []

    def usage(where):
        asked.append(where)
        return types.SimpleNamespace(free=2 ** 30)

    with mock.patch.dict(os.environ, {"HOME": home, "USERPROFILE": home}), mock.patch.object(builder.shutil, "disk_usage", usage):
        gb = builder.free_gb(path)
    return gb, asked


class FreeSpaceTests(unittest.TestCase):
    def test_a_tilde_path_is_measured_under_the_home_folder(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-free-") as home:
            gb, asked = measured("~/Games/Hookshot", home)
            self.assertEqual(gb, 1.0)
            self.assertEqual(asked, [home])    # (the nearest folder that exists, not "/")

    def test_a_path_that_exists_is_measured_as_it_is(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-free-") as home:
            gb, asked = measured(home, home)
            self.assertEqual(asked, [home])

    def test_an_empty_field_still_measures_something(self):
        gb, asked = measured("", "/")
        self.assertEqual(asked, ["/"])


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
