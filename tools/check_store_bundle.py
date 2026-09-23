#!/usr/bin/env python3
"""Validate a Store bundle with the Cartridge Store's own intake code.

Usage: check_store_bundle.py BUNDLE.zip CARTRIDGE_STORE_ROOT
Fails if the Store would reject the bundle or if any cartridge it rebuilds
from the manifest exceeds the 64 KiB package ceiling.
"""
import sys
import zipfile
from pathlib import Path

bundle, store_root = Path(sys.argv[1]), Path(sys.argv[2])
sys.path.insert(0, str(store_root))
from cartridge_store.app import _prepare_bundle  # noqa: E402

with zipfile.ZipFile(bundle) as zf:
    prepared = _prepare_bundle(zf)
for item in prepared:
    size = len(item["image"])
    meta = item["parsed"].metadata
    print(f"store accepts {item['architecture']}: {meta['id']} {meta['version']} rebuilt={size} bytes")
    if size > 65536:
        sys.exit(f"{item['file']} rebuilt by the Store exceeds 64 KiB")
