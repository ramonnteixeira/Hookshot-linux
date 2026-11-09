"""Execute the production stamp block on tiny authored files, without importing/building the renderer."""
import argparse
import ast
import hashlib
import os
from pathlib import Path
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--build-script", type=Path, default=Path(__file__).resolve().parents[1] / "nx2/build.py")
a = p.parse_args()
tree = ast.parse(a.build_script.read_text(encoding="utf-8"))
assigns = lambda n, name: isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in n.targets)
branches = [n for n in ast.walk(tree) if isinstance(n, ast.If) and any(assigns(s, "translator") for s in n.body)]
assert len(branches) == 1
body = [n for n in branches[0].body if assigns(n, "stamp") or assigns(n, "translator") or
        isinstance(n, ast.For) and isinstance(n.iter, ast.BinOp) and any(
            isinstance(x, ast.Name) and x.id == "translator" for x in ast.walk(n.iter))]
assert len(body) == 3, "expected actual hash initialization, translator list and file loop"
code = compile(ast.Module(body=body, type_ignores=[]), str(a.build_script), "exec")
with tempfile.TemporaryDirectory(prefix="shader-stamp-bank-") as tmp:
    vk = Path(tmp) / "gpu/vk"
    for rel in ("eden/a.cpp", "sirit/a.cpp", "compat/a.h", "vk_shader.cpp", "vk_bank.cpp", "vk_state.h", "vk_api.h", "vk_draw.cpp"):
        f = vk / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(b"authored input\n")
    def stamp():
        ns = dict(os=os, hashlib=hashlib, vk=str(vk))
        exec(code, ns)
        return ns["stamp"].hexdigest()[:16]
    first = stamp()
    (vk / "vk_draw.cpp").write_bytes(b"unrelated renderer edit\n")
    assert stamp() == first, "unrelated renderer source entered shader stamp"
    (vk / "vk_bank.cpp").write_bytes(b"changed bank semantics\n")
    changed = stamp()
    assert changed != first, "bank-only edit did not invalidate shader stamp"
    (vk / "vk_bank.cpp").write_bytes(b"changed bank semantics\r\n")
    assert stamp() == changed, "existing CRLF normalization changed"
print("PASS: bank-only edits change actual shader stamp; unrelated renderer source stays excluded")
