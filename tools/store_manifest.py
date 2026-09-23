#!/usr/bin/env python3
"""Write the Cartridge Store bundle manifest from metadata/*.json.

Cartridge Store intake requires manifest.abi == prg32-metadata-1.0 and
rebuilds every architecture's cartridge from the manifest: metadata fields
from the manifest itself, the screenshot from assets.splash and the colophon
from an inline object.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist/store/manifest.json"
meta = json.loads((ROOT / "metadata/metadata.json").read_text())
colophon = json.loads((ROOT / "metadata/colophon.json").read_text())
assert meta["abi"] == "prg32-metadata-1.0"
assert colophon["version"] == meta["version"] and colophon["title"] == meta["title"]
manifest = dict(meta)
manifest["architectures"] = [{"id": a, "file": f"{meta['name']}-{a}.prg32"} for a in meta["runtime"]["architectures"]]
manifest["assets"] = {"icon": "icon.png", "splash": "screenshot.png"}
manifest["colophon"] = colophon
out.write_text(json.dumps(manifest, ensure_ascii=False, separators=(",", ":")) + "\n")
print(f"wrote {out}")
