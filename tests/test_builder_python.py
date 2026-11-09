"""The builder calls the py launcher as `py -3 -I ...`, not `py -I -3 ...`.

The launcher's -3 has to come before Python's own -I: `py -I -3` hands -3 to Python, which stops with
"Unknown option: -3" and "Try 'python -h' for more information."
python -I -B tests/test_builder_python.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "builder" / "HookshotBuilder" / "Runner.cs"


def main():
    source = RUNNER.read_text(encoding="utf-8")
    info = source[source.index("static ProcessStartInfo Info("):]
    info = info[:info.index("return si;")]
    if 'si.ArgumentList.Add("-I")' not in info:
        raise SystemExit("Runner.Info does not run the tools isolated")
    if "tools.PythonArgs" not in info:
        raise SystemExit("Runner.Info drops the py launcher's -3")
    if info.index("tools.PythonArgs") > info.index('si.ArgumentList.Add("-I")'):
        raise SystemExit("py -I -3: Python sees -3 as one of its own options (Unknown option: -3)")
    if 'new[] { "-3" }' not in source or "IsStoreShortcut" not in source:
        raise SystemExit("the source-tree Python no longer avoids Windows' Store shortcut with py -3")
    print("PASS: the py launcher gets -3 before -I")


if __name__ == "__main__":
    main()
