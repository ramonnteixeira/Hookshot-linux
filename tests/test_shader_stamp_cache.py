"""Run the actual Vulkan job construction and object keys over authored files, without a compiler or game.

An alternate --build-script permits the same regression to exercise an older production build.py.
"""
import argparse
import ast
import hashlib
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import warnings


ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / "nx2" / "build.py"


def production_code(path):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    names = {"tree_headers", "vk_sources", "flag_text", "object_key"}
    functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
    assert {n.name for n in functions} == names
    main = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "main")
    # Execute the real branch that hashes the translator, chooses per-source flags/deps and appends Vulkan jobs.
    branches = [n for n in ast.walk(main) if isinstance(n, ast.If) and any(
        isinstance(s, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "vflags" for t in s.targets)
        for s in n.body)]
    assert len(branches) == 1, "expected one Vulkan job-construction branch"
    initial = next(n for n in main.body if isinstance(n, ast.Assign) and any(
        isinstance(t, ast.Name) and t.id == "vk_jobs" for t in n.targets))
    return compile(ast.Module(body=functions + [initial] + branches[0].body, type_ignores=[]), str(path), "exec")


class ShaderStampCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hookshot-shader-stamp-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.files = {
            "gpu/vk/vk_shader.cpp": "int shader;\n",
            "gpu/vk/vk_bank.cpp": "int bank;\n",
            "gpu/vk/vk_draw.cpp": "int draw;\n",
            "gpu/vk/vk_menu.cpp": "int menu;\n",
            "gpu/vk/vk_state.h": "struct State {};\n",
            "gpu/vk/vk_api.h": "struct Api {};\n",
            "gpu/vk/eden/translate.cpp": "int translator;\n",
            "gpu/vk/eden/ir.h": "struct IR {};\n",
            "gpu/vk/sirit/src/sirit.cpp": "int sirit;\n",
            "gpu/vk/sirit/include/sirit.h": "struct Sirit {};\n",
            "gpu/vk/compat/compat.h": "struct Compat {};\n",
            "gpu/compat/common.h": "struct Common {};\n",
            "gpu/render/render.h": "struct Render {};\n",
            "gpu/third_party/suyu/video_core/textures/decoders.cpp": "int decoder;\n",
            "gpu/third_party/suyu/video_core/textures/astc.cpp": "int astc;\n",
            "gpu/third_party/suyu/video_core/textures/texture.h": "struct Texture {};\n",
            "host/posix/windows.h": "struct Posix {};\n",
            "host/posix/win32_posix.c": "int win32;\n",
            "host/posix/gdi_posix.c": "int gdi;\n",
        }
        for name, text in self.files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode())
        self.code = production_code(BUILD_SCRIPT)

    def jobs(self, linux, version="fixture", release=False):
        namespace = {
            "os": os, "hashlib": hashlib,
            "GPU": str(self.root / "gpu"), "POSIX": str(self.root / "host/posix"),
            "LINUX": linux, "CPU": ["-march=x86-64"],
            "kit": SimpleNamespace(ROOT=str(self.root), HERE=str(self.root / "nx2")),
            "toolchain": SimpleNamespace(find=lambda name: name, need=lambda name: name),
            "target": lambda: ["--target=x86_64-linux-gnu" if linux else "--target=x86_64-w64-mingw32"],
            "cpm_package": lambda name: str(self.root / "deps" / name),
            "spirv_headers": str(self.root / "deps/spirv-headers"),
            "vk_headers": str(self.root / "deps/vulkanheaders"),
            "obj_dir": str(self.root / "obj"), "cc": "clang", "jobs": [],
            "stamp_def": '-DHOOKSHOT_VERSION="%s"' % version, "logo_text": b"", "gen_dir": str(self.root / "gen"),
        }
        with mock.patch.dict(os.environ, {"RX_RELEASE": "1"} if release else {}, clear=True), warnings.catch_warnings():
            # The production key/header helpers use short-lived open(...).read() expressions.
            warnings.simplefilter("ignore", ResourceWarning)
            exec(self.code, namespace)
            return {str(Path(src).relative_to(self.root)).replace("\\", "/"):
                    {"key": namespace["object_key"](src, flags, deps), "flags": flags}
                    for _, src, _, flags, deps in namespace["vk_jobs"]}

    def changed(self, before, after):
        self.assertEqual(before.keys(), after.keys())
        return {name for name in before if before[name]["key"] != after[name]["key"]}

    def test_translator_source_changes_only_its_object_and_shader_consumer(self):
        path = self.root / "gpu/vk/eden/translate.cpp"
        for linux in (False, True):
            with self.subTest(linux=linux):
                path.write_bytes(b"int translator;\n")
                before = self.jobs(linux)
                path.write_bytes(b"int translator_changed;\n")
                after = self.jobs(linux)
                self.assertEqual(self.changed(before, after),
                                 {"gpu/vk/eden/translate.cpp", "gpu/vk/vk_shader.cpp"})

    def test_own_renderer_source_still_invalidates_its_object(self):
        path = self.root / "gpu/vk/vk_draw.cpp"
        for linux in (False, True):
            with self.subTest(linux=linux):
                path.write_bytes(b"int draw;\n")
                before = self.jobs(linux)
                path.write_bytes(b"int draw_changed;\n")
                self.assertEqual(self.changed(before, self.jobs(linux)), {"gpu/vk/vk_draw.cpp"})

    def test_bank_source_changes_only_its_object_and_shader_consumer(self):
        path = self.root / "gpu/vk/vk_bank.cpp"
        for linux in (False, True):
            with self.subTest(linux=linux):
                path.write_bytes(b"int bank;\n")
                before = self.jobs(linux)
                path.write_bytes(b"int bank_changed;\n")
                self.assertEqual(self.changed(before, self.jobs(linux)),
                                 {"gpu/vk/vk_bank.cpp", "gpu/vk/vk_shader.cpp"})

    def test_headers_still_invalidate_the_existing_dependency_groups(self):
        path = self.root / "gpu/vk/eden/ir.h"
        for linux in (False, True):
            with self.subTest(linux=linux):
                path.write_bytes(b"struct IR {};\n")
                before = self.jobs(linux)
                path.write_bytes(b"struct IR { int changed; };\n")
                expected = {name for name in before if not name.startswith("host/posix/")}
                self.assertEqual(self.changed(before, self.jobs(linux)), expected)

    def test_stamp_line_ending_normalization_is_unchanged(self):
        path = self.root / "gpu/vk/eden/translate.cpp"
        for linux in (False, True):
            with self.subTest(linux=linux):
                path.write_bytes(b"int translator;\n")
                before = self.jobs(linux)
                path.write_bytes(b"int translator;\r\n")
                self.assertEqual(self.changed(before, self.jobs(linux)), {"gpu/vk/eden/translate.cpp"})

    def test_stamp_version_and_release_flags_reach_their_consumers(self):
        for linux in (False, True):
            with self.subTest(linux=linux):
                before = self.jobs(linux, release=True)
                for name, job in before.items():
                    shader = [f for f in job["flags"] if f.startswith("-DHOOKSHOT_SHADER_STAMP=")]
                    version = [f for f in job["flags"] if f.startswith("-DHOOKSHOT_VERSION=")]
                    self.assertEqual(len(shader), int(name == "gpu/vk/vk_shader.cpp"))
                    self.assertEqual(len(version), int(name == "gpu/vk/vk_menu.cpp"))
                    if not name.startswith("host/posix/"):
                        self.assertIn("-DHOOKSHOT_RELEASE", job["flags"])
                self.assertEqual(self.changed(before, self.jobs(linux, version="new", release=True)),
                                 {"gpu/vk/vk_menu.cpp"})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-script", type=Path, default=BUILD_SCRIPT)
    args, rest = parser.parse_known_args()
    BUILD_SCRIPT = args.build_script
    unittest.main(argv=[sys.argv[0], *rest])
