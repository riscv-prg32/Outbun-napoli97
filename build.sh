#!/usr/bin/env bash
# Build OutBun-napoli97: assets -> tests -> portable cartridge -> Store bundle.
#   PRG32_ROOT=/path/to/PRG32 [CARTRIDGE_STORE_ROOT=/path/to/CartridgeStore] ./build.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${PRG32_ROOT:-}"
if [[ -z "$ROOT" || ! -d "$ROOT/prg32" ]]; then echo "Set PRG32_ROOT to a current PRG32 checkout" >&2; exit 2; fi
# The RISC-V toolchain comes from ESP-IDF; use its install if not on PATH yet.
if ! command -v riscv32-esp-elf-gcc >/dev/null; then
  TC="$(ls -d "$HOME"/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin 2>/dev/null | tail -1 || true)"
  [[ -n "$TC" ]] && export PATH="$TC:$PATH"
fi
NAME=OutBun-napoli97
LIMIT=65536
python3 "$HERE/tools/generate_assets.py"
python3 "$HERE/tools/generate_audio.py"
python3 "$HERE/tests/source_checks.py"
bash "$HERE/tests/host_syntax.sh"
bash "$HERE/tests/run_harness.sh"
python3 "$HERE/tools/render_screens.py"
VERSION="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["version"])' "$HERE/metadata/metadata.json")"
BUNDLE="$NAME-$VERSION-store.zip"
BUILD="$HERE/build"; DIST="$HERE/dist"; STORE="$DIST/store"
rm -rf "$BUILD" "$STORE"; mkdir -p "$BUILD" "$STORE"
cd "$ROOT"
python3 tools/prg32audio_pack.py "$HERE/audio.json" --out "$BUILD/outbun-audio.block"
python3 "$HERE/tools/prg32_cli_64.py" cartridge build "$HERE/src/game.c" --portable --multiplayer \
  --entry-prefix outbun --name "$NAME" --audio-block "$BUILD/outbun-audio.block" --out "$BUILD/outbun-base.prg32"
for arch in esp32c6 qemu; do
  python3 -m prg32 store attach-metadata "$BUILD/outbun-base.prg32" --metadata "$HERE/metadata/metadata.json" \
    --icon "$HERE/assets/generated/icon.png" --screenshot "$HERE/assets/generated/screenshot.png" \
    --colophon "$HERE/metadata/colophon.json" --architecture "$arch" --out "$STORE/$NAME-$arch.prg32"
done
python3 "$HERE/tools/store_manifest.py" "$STORE/manifest.json"
cp "$HERE/assets/generated/icon.png" "$HERE/assets/generated/screenshot.png" "$STORE/"
python3 -m prg32 store pack-bundle --manifest "$STORE/manifest.json" --out "$DIST/$BUNDLE"
for f in "$STORE"/*.prg32; do
  size=$(wc -c < "$f"); echo "$(basename "$f"): $size / $LIMIT bytes"
  test "$size" -le "$LIMIT" || { echo "$f exceeds 64 KiB" >&2; exit 3; }
done
if [[ -n "${CARTRIDGE_STORE_ROOT:-}" ]]; then
  python3 "$HERE/tools/check_store_bundle.py" "$DIST/$BUNDLE" "$CARTRIDGE_STORE_ROOT"
else
  echo "CARTRIDGE_STORE_ROOT not set: skipping Store intake check" >&2
fi
(cd "$DIST" && shasum -a 256 store/*.prg32 "$BUNDLE" > SHA256SUMS)
echo "Built portable 64 KiB profile: $DIST/$BUNDLE"
