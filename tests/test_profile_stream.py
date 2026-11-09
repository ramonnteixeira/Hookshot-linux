"""Game-free profile output and bounded input reading: python -I -B tests/test_profile_stream.py."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("profile_under_test", ROOT / "scripts" / "prof.py")
prof = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prof)


class IteratedFile(io.StringIO):
    """A profile may be iterated, but must not be materialized as one string or list."""
    def read(self, *args):
        raise AssertionError("read must not load the profile")

    def readlines(self, *args):
        raise AssertionError("readlines must not load the profile")


class ProfileTests(unittest.TestCase):
    def capture(self, text, skip="0", only=None, maps=None):
        with tempfile.TemporaryDirectory(prefix="hookshot-profile-") as folder:
            if maps:
                Path(folder, "hookshot.map").write_text(maps, encoding="utf-8")
            profile = IteratedFile(text)
            real_open = open

            def open_input(path, *args, **kwargs):
                if path == "synthetic-profile":
                    return profile
                return real_open(path, *args, **kwargs)

            args = ["prof.py", "synthetic-profile", "40", skip]
            if only is not None:
                args.append(only)
            output = io.StringIO()
            with mock.patch.object(prof, "BUILD", folder), mock.patch.object(sys, "argv", args), \
                    mock.patch("builtins.open", side_effect=open_input), contextlib.redirect_stdout(output):
                prof.main()
            self.assertTrue(profile.closed)
            return output.getvalue()

    def test_output_and_duplicate_frames(self):
        self.assertEqual(self.capture("a.dll+1;a.dll+2;b.dll+3\na.dll+4\n\n"),
                         "2 samples\n-- leaf modules\n 100.0%  a.dll\n-- inclusive\n"
                         " 100.0%  a.dll\n  50.0%  b.dll\n-- self\n 100.0%  a.dll\n")

    def test_skip_counts_physical_lines_before_filtering(self):
        text = "a.dll+1\n\nb.dll+2\nc.dll+3\n"
        expected = {"0.5": "2 samples", "0.75": "1 samples", "1": "0 samples", "2": "0 samples",
                    "-0.5": "2 samples", "-2": "3 samples", "0.1": "3 samples", "1e30": "0 samples"}
        for skip, count in expected.items():
            with self.subTest(skip=skip):
                self.assertEqual(self.capture(text, skip).splitlines()[0], count)
        self.assertIn("100.0%  c.dll", self.capture(text, "0.5", "c.dll"))
        self.assertTrue(self.capture(text, "0.5", "missing").startswith("0 samples\n"))

    def test_empty_input(self):
        for skip in ("0", "0.5"):
            self.assertEqual(self.capture("", skip), "0 samples\n-- leaf modules\n-- inclusive\n-- self\n")

    def test_symbol_resolution_and_filter(self):
        maps = " 0001:00000000 ?draw@renderer@@ 0000000140000010 f\n"
        output = self.capture("hookshot.exe+10;driver.dll+1\nother.dll+2\n", only="renderer::draw", maps=maps)
        self.assertTrue(output.startswith("1 samples\n"))
        self.assertIn("100.0%  hookshot:renderer::draw", output)
        self.assertNotIn("other.dll", output)


if __name__ == "__main__":
    unittest.main()
