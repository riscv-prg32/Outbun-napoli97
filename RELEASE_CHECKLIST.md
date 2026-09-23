# Release checklist

- `make test` passes (source checks, strict host compile, harness under ASan and UBSan).
- `PRG32_ROOT=… CARTRIDGE_STORE_ROOT=… ./build.sh` passes, both `.prg32` files are ≤ 65,536 bytes, and the Store intake accepts both architectures.
- Version matches in `metadata/metadata.json`, `metadata/colophon.json` and `CHANGELOG.md`.
- QEMU: stage with `tools/prg32_cli_64.py qemu upload`, check the title music, menus, intro map, countdown, race and a checkpoint.
- ESP32-C6: upload, then check colours (custom palette), frame rate in Napoli town traffic, engine pitch and stereo music.
- Multiplayer: two, three and four boards on the MultiplayerServer; ready handshake, rival cars on the road, progress-bar dots, disconnect → CPU takeover, results.
- Drive all nine legs by hand: time bonuses are fair, the tunnels (Pozzano, Varano, Seiano, Alimuri, Positano, Furore) render correctly from outside and inside, and the landmarks appear.
- Panino screen: stale bread without a rosetta, star counts, verdicts; the score is saved and the best one shows on the title.
- Publish `dist/OutBun-napoli97-<version>-store.zip` with `dist/SHA256SUMS`.
