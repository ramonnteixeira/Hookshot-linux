"""The Linux builder must grow on a 1440p or 4K screen: Tk on X11 ignores the desktop's scale, so its text stays 12 px.

python -I -B tests/test_builder_scale.py
"""
import importlib.util
from pathlib import Path
import sys
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
    spec = importlib.util.spec_from_file_location("hookshot_builder_scale", ROOT / "builder" / "linux" / "hookshot_builder.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load_builder()


class FakeTcl:
    """Tk 8.6's X11 named fonts: pixel sizes, except one a theme gave in points"""

    def __init__(self):
        self.fonts = {"TkDefaultFont": -12, "TkFixedFont": -12, "TkHeadingFont": -12, "PointFont": 10}
        self.scaling = None

    def splitlist(self, value):
        return value

    def call(self, *args):
        if args[:2] == ("tk", "scaling"):
            self.scaling = args[2]
        elif args == ("font", "names"):
            return tuple(self.fonts)
        elif args[:2] == ("font", "configure") and len(args) == 4:
            return str(self.fonts[args[2]])
        elif args[:2] == ("font", "configure"):
            self.fonts[args[2]] = args[4]


def window(height, dpi):
    win = types.SimpleNamespace(tk=FakeTcl(), winfo_screenheight=lambda: height)
    with mock.patch.object(builder, "xft_dpi", return_value=dpi), mock.patch.dict(builder.os.environ, {}, clear=True):
        builder.Builder.apply_scale(win)
    return win


class ScaleTests(unittest.TestCase):
    def test_a_1080p_screen_is_left_alone(self):
        self.assertEqual(builder.ui_scale(1080, environ={}), 1.0)
        win = window(1080, None)
        self.assertIsNone(win.tk.scaling)
        self.assertEqual(win.tk.fonts["TkDefaultFont"], -12)

    def test_a_4k_screen_with_no_desktop_scale_doubles(self):
        self.assertEqual(builder.ui_scale(2160, environ={}), 2.0)
        self.assertAlmostEqual(builder.ui_scale(1440, environ={}), 1440 / 1080)

    def test_xft_dpi_wins_over_the_screen_height(self):
        self.assertEqual(builder.ui_scale(2160, dpi=144, environ={}), 1.5)
        self.assertEqual(builder.ui_scale(2160, dpi=96, environ={}), 1.0)

    def test_gdk_scale_of_one_is_not_an_answer(self):
        self.assertEqual(builder.ui_scale(2160, environ={"GDK_SCALE": "1"}), 2.0)
        self.assertEqual(builder.ui_scale(1080, environ={"GDK_SCALE": "2"}), 2.0)
        self.assertEqual(builder.ui_scale(1080, environ={"QT_SCALE_FACTOR": "1.25"}), 1.25)

    def test_hookshot_ui_scale_overrides_everything(self):
        self.assertEqual(builder.ui_scale(2160, dpi=192, environ={"HOOKSHOT_UI_SCALE": "1", "GDK_SCALE": "2"}), 1.0)
        self.assertEqual(builder.ui_scale(1080, environ={"HOOKSHOT_UI_SCALE": "junk"}), 1.0)
        self.assertEqual(builder.ui_scale(1080, environ={"HOOKSHOT_UI_SCALE": "40"}), 4.0)

    def test_pixel_fonts_grow_and_point_fonts_follow_tk_scaling(self):
        win = window(2160, None)
        self.assertEqual(win.tk.scaling, 2.0 * 96 / 72)
        self.assertEqual(win.tk.fonts["TkDefaultFont"], -24)
        self.assertEqual(win.tk.fonts["TkFixedFont"], -24)
        self.assertEqual(win.tk.fonts["PointFont"], 10)
        self.assertEqual(builder.Builder.px(win, 560), 1120)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
