#!/usr/bin/env python3
"""Render real game frames through the host harness.

Builds tests/harness/run_harness.c, runs it with OUTBUN_SHOTS pointing at a
temporary directory, and converts the dumped frames (exact 320x200 palette
output of src/game.c) into:

    assets/generated/screenshot.png           compact Store screenshot (embedded)
    release-artifacts/store-screenshots/*.png  2x previews for the Store page
    release-artifacts/OutBun-napoli97-contact-sheet.png
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
STORE_SHOT = "15-vietri-sunset"
GALLERY = ["01-title", "03-car-select", "04-intro-map", "05-start-grid", "06-pozzano-tunnel",
           "08-seiano-viaduct", "09-capri-from-massa", "10-checkpoint", "13-positano", "15-vietri-sunset",
           "16-vesuvio", "19-castellammare-shipyard", "20-amalfi-duomo", "21-maiori-beach",
           "11-panino", "17-lobby", "12-results"]


def main() -> int:
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="outbun-shots-") as tmp:
        tmp = Path(tmp)
        exe = tmp / "harness"
        subprocess.run([cc, "-std=c11", "-O2", f"-I{ROOT/'tests/stub'}", f"-I{ROOT/'src'}",
                        str(ROOT / "tests/harness/run_harness.c"), "-o", str(exe)], check=True)
        env = dict(os.environ, OUTBUN_SHOTS=str(tmp))
        subprocess.run([str(exe)], check=True, env=env, stdout=subprocess.DEVNULL)
        out = ROOT / "release-artifacts" / "store-screenshots"
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        frames = {}
        for ppm in sorted(tmp.glob("*.ppm")):
            frames[ppm.stem] = Image.open(ppm).convert("RGB")
        for name in GALLERY:
            frames[name].resize((640, 400), Image.Resampling.NEAREST).save(out / f"{name}.png", optimize=True)
        # Store screenshot: native 320x200, adaptive palette, lossless PNG
        shot = frames[STORE_SHOT].quantize(colors=64, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        shot.save(ROOT / "assets/generated/screenshot.png", optimize=True)
        sheet = Image.new("RGB", (320 * 4 + 5 * 6, 200 * 4 + 5 * 6), (16, 20, 40))
        for i, name in enumerate([g for g in GALLERY if g not in ("12-results", "17-lobby", "03-car-select", "04-intro-map")][:16]):
            x, y = 6 + (i % 4) * 326, 6 + (i // 4) * 206
            sheet.paste(frames[name], (x, y))
        sheet.save(ROOT / "release-artifacts/OutBun-napoli97-contact-sheet.png", optimize=True)
        print(f"screenshot: {(ROOT / 'assets/generated/screenshot.png').stat().st_size} bytes; "
              f"{len(GALLERY)} gallery frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
