"""The build's timer and countdown: make_game.py's build_times.json, and the estimates build.py makes from it.

Nothing here runs a compiler or needs the game.

python -I -B tests/test_build_times.py
"""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "nx2"), str(ROOT / "third_party" / "a64-kit" / "tools")]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


make_game = load("make_game_times", ROOT / "nx2" / "make_game.py")
build = load("build_times", ROOT / "nx2" / "build.py")


class TimesRecordTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hookshot-times-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)

    def record(self, files, compile_seconds=10.0, total_seconds=20.0, modules=3, kind="build"):
        return make_game.record_times(str(self.work), files, compile_seconds, total_seconds, modules=modules, kind=kind)

    def test_a_first_build_is_written_and_has_nothing_before_it(self):
        self.assertIsNone(self.record({"code_001.c": 120.4}))
        data = json.loads((self.work / "build_times.json").read_text(encoding="utf-8"))
        self.assertEqual(data["format"], 1)
        self.assertEqual(data["seconds"], {"code_001.c": 120.4})
        self.assertEqual(len(data["builds"]), 1)
        self.assertEqual(data["builds"][0]["compile_seconds"], 10.0)
        self.assertEqual(data["builds"][0]["total_seconds"], 20.0)
        self.assertEqual(data["builds"][0]["compiled"], 1)
        self.assertEqual(data["builds"][0]["modules"], 3)
        self.assertEqual(data["builds"][0]["kind"], "build")

    def test_the_next_build_sees_the_last_one_and_keeps_the_old_modules(self):
        self.record({"code_001.c": 120.4})
        self.assertEqual(self.record({"code_002.c": 6.0}, 30.0, 40.0), 20.0)
        data = make_game.load_times(str(self.work))
        self.assertEqual(data["seconds"], {"code_001.c": 120.4, "code_002.c": 6.0})
        self.assertEqual([b["total_seconds"] for b in data["builds"]], [20.0, 40.0])

    def test_a_module_a_cache_held_keeps_its_time(self):
        self.record({"code_001.c": 120.4})
        self.record({})   # (nothing compiled: an update that only had to link)
        self.assertEqual(make_game.load_times(str(self.work))["seconds"], {"code_001.c": 120.4})

    def test_a_zero_is_not_a_measurement(self):
        self.record({"code_001.c": 0.0, "code_002.c": 5.5})
        self.assertEqual(make_game.load_times(str(self.work))["seconds"], {"code_002.c": 5.5})

    def test_a_broken_or_foreign_file_is_started_again(self):
        (self.work / "build_times.json").write_text("{not json", encoding="utf-8")
        self.assertEqual(make_game.load_times(str(self.work))["builds"], [])
        (self.work / "build_times.json").write_text('{"format": 99, "seconds": {"a": 1}}', encoding="utf-8")
        self.assertEqual(make_game.load_times(str(self.work))["seconds"], {})
        self.assertIsNone(self.record({"code_001.c": 3.0}))

    def test_only_the_newest_builds_are_kept(self):
        for i in range(make_game.KEEP_BUILDS + 5):
            self.record({}, 1.0, float(i))
        builds = make_game.load_times(str(self.work))["builds"]
        self.assertEqual(len(builds), make_game.KEEP_BUILDS)
        self.assertEqual(builds[-1]["total_seconds"], float(make_game.KEEP_BUILDS + 4))


class EstimateTests(unittest.TestCase):
    def test_no_files_left_is_no_time(self):
        self.assertEqual(build.eta_left([], 8), 0.0)

    def test_the_work_spreads_over_the_compilers(self):
        self.assertEqual(build.eta_left([100.0, 100.0, 100.0, 100.0], 4), 100.0)
        self.assertEqual(build.eta_left([100.0, 100.0], 1), 200.0)

    def test_the_longest_file_sets_the_floor(self):
        self.assertEqual(build.eta_left([600.0, 60.0], 8), 600.0)

    def test_measured_seconds_are_used_as_they_are(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "code_003.c"
            src.write_bytes(b"x" * 1024)
            jobs = [("cc", str(src), str(Path(d) / "code_003.o"), [], b"")]
            self.assertEqual(build.per_file_seconds(jobs, {"code_003.c": 42.0}), [42.0])

    def test_an_unmeasured_module_follows_the_measured_rate(self):
        with tempfile.TemporaryDirectory() as d:
            known = Path(d) / "code_001.c"
            known.write_bytes(b"x" * 1024)
            new = Path(d) / "code_002.c"
            new.write_bytes(b"x" * 2048)
            jobs = [("cc", str(known), "", [], b""), ("cc", str(new), "", [], b"")]
            self.assertEqual(build.per_file_seconds(jobs, {"code_001.c": 10.0}), [10.0, 20.0])

    def test_a_first_build_still_has_an_estimate_for_every_module(self):
        with tempfile.TemporaryDirectory() as d:
            jobs = []
            for name in ("code_hot.c", "code_001.c", "host_main.c"):
                src = Path(d) / name
                src.write_bytes(b"x" * (1 << 20))
                jobs.append(("cc", str(src), "", [], b""))
            seconds = build.per_file_seconds(jobs, {})
            self.assertEqual(len(seconds), 3)
            self.assertTrue(all(seconds))
            self.assertGreater(seconds[0], seconds[1])   # (the hot file's rate is the highest)

    def test_a_broken_record_is_no_estimate(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "build_times.json"
            self.assertEqual(build.times_from(str(path)), {})
            path.write_text("{not json", encoding="utf-8")
            self.assertEqual(build.times_from(str(path)), {})
            path.write_text('{"seconds": {"code_001.c": 12.5, "odd": "x"}}', encoding="utf-8")
            self.assertEqual(build.times_from(str(path)), {"code_001.c": 12.5})

    def test_the_plan_leaves_out_what_a_cache_already_holds(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "code_001.c"
            src.write_text("int f(void) { return 0; }\n", encoding="utf-8")
            obj = Path(d) / "code_001.o"
            job = ("cc", str(src), str(obj), ["-O1"], b"headers")
            work, keys = build.plan_jobs([job])
            self.assertEqual(work, [job])
            key = keys[(str(src), str(obj))]
            obj.write_bytes(b"")
            Path(str(obj) + ".sha1").write_text(key, encoding="utf-8")
            self.assertEqual(build.plan_jobs([job])[0], [])


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
