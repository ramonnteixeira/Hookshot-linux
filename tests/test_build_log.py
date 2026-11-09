"""CLI build logs preserve live output and failures without a dump or compiler."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1] / "nx2" / "make_game.py"
spec = importlib.util.spec_from_file_location("make_game_under_test", SOURCE)
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


class BuildLogTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hookshot-log-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.log = self.root / "build log \u6f22.txt"

    def invoke(self, action, log=True):
        argv = [str(SOURCE), "--jobs", "1"]
        if log:
            argv += ["--log", str(self.log)]
        stdout, stderr = io.StringIO(), io.StringIO()
        with (mock.patch.object(sys, "argv", argv),
              contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr),
              mock.patch.object(make_game, "main", side_effect=action) as build):
            code = 0
            try:
                make_game.cli()
            except SystemExit as e:
                code = e.code
            self.assertIs(sys.stdout, stdout)
            self.assertIs(sys.stderr, stderr)
        return code, stdout.getvalue(), stderr.getvalue(), build

    def child(self, code):
        return make_game.PY + ["-c", code]

    def test_live_output_unicode_and_child_streams(self):
        self.log.write_text("stale run", encoding="utf-8")

        def build(args):
            self.assertEqual(args.log, str(self.log))
            make_game.say("stage", "compile", "Compiling")
            print("parent warning \u6f22", file=sys.stderr)
            self.assertIn("parent warning \u6f22", self.log.read_text(encoding="utf-8"))

            def on_line(line):
                # The log is readable before the step (and the build) has finished.
                self.assertIn(line, self.log.read_text(encoding="utf-8"))

            make_game.run(self.child("import sys; print('child out \u6f22'); print('child err', file=sys.stderr)"),
                          on_line=on_line)
            make_game.say("done", "fixture")

        code, stdout, stderr, _ = self.invoke(build)
        self.assertEqual(code, 0, stdout + stderr)
        text = self.log.read_text(encoding="utf-8")
        for line in ("@@stage compile Compiling", "child out \u6f22", "child err", "@@done fixture"):
            self.assertIn(line, stdout)
            self.assertIn(line, text)
        self.assertEqual(stderr, "parent warning \u6f22\n")
        self.assertNotIn("parent warning", stdout)
        self.assertNotIn("stale run", text)
        self.assertNotIn(b"\r\n", self.log.read_bytes())

    def test_failed_child_keeps_error_and_nonzero_exit(self):
        def build(args):
            make_game.run(self.child("import sys; print('error: authored failure', file=sys.stderr); sys.exit(7)"))
            self.fail("the failed step must stop the build")

        code, stdout, stderr, _ = self.invoke(build)
        self.assertEqual(code, 1)
        self.assertIn("error: authored failure", stdout)
        self.assertIn("@@error ", stdout)
        self.assertEqual(stderr, "")
        self.assertEqual(self.log.read_text(encoding="utf-8"), stdout)

    def test_nonfatal_shader_output_stays_on_stderr_and_is_logged(self):
        def build(args):
            code = make_game.run(self.child("import sys; print('shader out'); print('shader err', file=sys.stderr); sys.exit(3)"),
                                 check=False, output=sys.stderr)
            self.assertEqual(code, 3)
            make_game.say("done", "fixture")

        code, stdout, stderr, _ = self.invoke(build)
        self.assertEqual(code, 0, stdout + stderr)
        self.assertEqual(stdout, "@@done fixture\n")
        self.assertIn("shader out", stderr)
        self.assertIn("shader err", stderr)
        self.assertEqual(self.log.read_text(encoding="utf-8"), stderr + stdout)

    def test_unexpected_exception_includes_traceback_and_error(self):
        def build(args):
            raise RuntimeError("authored exception")

        code, stdout, stderr, _ = self.invoke(build)
        self.assertEqual(code, 1)
        self.assertIn("Traceback", stdout)
        self.assertIn("RuntimeError: authored exception", stdout)
        self.assertIn("@@error ", stdout)
        self.assertEqual(self.log.read_text(encoding="utf-8"), stdout + stderr)

    def test_system_exit_message_is_logged(self):
        def build(args):
            raise SystemExit("authored toolchain failure")

        code, stdout, stderr, _ = self.invoke(build)
        self.assertEqual(code, 1)
        # (said in the builder's protocol, as #434 has it: a library's SystemExit text is an @@error line)
        self.assertEqual(stdout, "@@error authored toolchain failure\n")
        self.assertEqual(self.log.read_text(encoding="utf-8"), stdout + stderr)

    def test_unwritable_destination_stops_before_build(self):
        self.log = self.root / "missing parent" / "build.log"
        code, stdout, stderr, build = self.invoke(lambda args: None)
        self.assertEqual(code, 1)
        self.assertIn("@@error Could not open build log", stdout)
        build.assert_not_called()
        self.assertFalse(self.log.exists())

    def test_no_log_preserves_console_streams_and_creates_no_file(self):
        def build(args):
            self.assertIsNone(args.log)
            self.assertIsInstance(sys.stdout, io.StringIO)
            self.assertIsInstance(sys.stderr, io.StringIO)
            print("console only")

        code, stdout, stderr, _ = self.invoke(build, log=False)
        self.assertEqual((code, stdout, stderr), (0, "console only\n", ""))
        self.assertEqual(list(self.root.iterdir()), [])

    def test_real_cli_logs_argument_validation_failure(self):
        for jobs in ([], ["--jobs", "1"]):
            with self.subTest(jobs=jobs):
                ran = subprocess.run(make_game.PY + [str(SOURCE), "--log=" + str(self.log),
                                     "--work", str(self.root / "work")] + jobs,
                                     env=dict(os.environ, HOOKSHOT_JOBS=""),
                                     capture_output=True, text=True, encoding="utf-8", timeout=30)
                self.assertEqual(ran.returncode, 1, ran.stdout + ran.stderr)
                self.assertIn("needs --out", ran.stdout)
                if not jobs:
                    self.assertIn("compilers at once", ran.stdout)
                self.assertEqual(self.log.read_text(encoding="utf-8"), ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
