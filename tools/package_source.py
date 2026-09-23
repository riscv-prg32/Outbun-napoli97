#!/usr/bin/env python3
"""Zip the source tree (without build products) as a GitHub-ready archive."""
import hashlib
import json
import zipfile
from pathlib import Path

r = Path(__file__).resolve().parents[1]
version = json.loads((r / "metadata/metadata.json").read_text())["version"]
out = r / "dist" / f"OutBun-napoli97-{version}-source.zip"
out.parent.mkdir(exist_ok=True)
skip = {".git", "build", "dist", "__pycache__"}
files = [p for p in r.rglob("*") if p.is_file() and not any(x in skip for x in p.relative_to(r).parts)]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for p in sorted(files):
        z.write(p, Path(r.name) / p.relative_to(r))
h = hashlib.sha256(out.read_bytes()).hexdigest()
(r / "dist" / "SOURCE_SHA256SUMS").write_text(f"{h}  {out.name}\n")
print(out)
