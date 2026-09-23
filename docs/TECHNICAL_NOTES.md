# Technical notes

## Target

PRG32 `main`, portable ABI-table cartridge (`--portable --multiplayer`), 64 KiB cartridge RAM and a 64 KiB package limit. The entry points are `outbun_init`, `outbun_update` and `outbun_draw`. The source follows the portable-cartridge rules: no libc (`memcpy` and `memset` are defined locally because GCC may emit them for struct copies), no heap, no initialised pointer tables (all strings are fixed `char[N][M]` arrays, since a relocated image would carry stale absolute addresses) and no jump tables.

## Colour: a programmed palette, drawn as indexed spans

On the ESP32-C6 the game viewport is an 8-bit indexed framebuffer, and RGB565 drawing calls are quantized to a 6×6×6 cube. To keep the art exact, OutBun programs its own colours with `prg32_palette_set()` and draws only with `prg32_gfx_rect_indexed()`. QEMU maps indexed calls through the same palette, so both targets show identical colours.

- **16–191**: the static palette, fitted by weighted k-means over all sprite colours plus the named UI colours.
- **192–228**: 37 *theme* slots (sky ramp, sea, land, road, rumble, mountains, sun, clouds, tunnel, valley, town), with one set per leg plus a dusk set for the title. At a checkpoint the game blends from the old set to the new one over about a second. That gives the time-of-day progression, morning in Napoli to sunset at Vietri, for 740 bytes of tables.
- 0–15 and the grey ramp 232–255 are left alone for the firmware.

## Sprites: row-group nibble RLE

`tools/generate_assets.py` paints every sprite procedurally with Pillow at 4× resolution: the four classics seen from behind, traffic, scenery, landmarks and ingredients. The colour of each output pixel is the dominant colour of its 4×4 block, which gives crisp pixel-art edges. Each sprite is then reduced to at most 15 colours and encoded as

```
row group := [repeat count] run... ;  run := (colour << 4) | (length - 1)
```

with a 15-byte look-up table from local colours to palette indices. Identical consecutive rows share one group. Alternative tables recolour sprites without new pixels (three house colours; Vietri's blue-and-gold dome).

`spr()` draws a sprite at any width. Enlarged sprites emit one span per run with the height of the row group; reduced sprites are sampled at destination resolution, so a distant object never costs more spans than it has visible pixels. Adjacent spans of the same colour are merged, mirroring is free, and the car *lean* in bends is a shear applied in bands of equal offset. 35 sprites fit in about 9 KB.

## Road: integer pseudo-3D

The classic segment renderer (200 world units per segment, 100 segments of draw distance) uses only integer arithmetic:

- projection `x' = 160 + x·134/z`, `y' = 92 + y·84/z` (a 100° field of view); the car is drawn 1:1 at `z = 839`
- curvature accumulates in Q4 fixed point (`dx += curve`), ramping in and out over 12 segments per section
- hills use a smoothstep in Q8 between section heights; the road is drawn near to far with a shrinking clip line, so crests hide what lies behind them
- each segment paints its ground band in two rectangles (land side, sea side), then about five spans per scanline: rumble strips, asphalt, centre line, sea-side stone parapet and occasional sea glints
- **tunnels**: from outside, the portal is a rock face and the clip rectangle shrinks to the tunnel mouth, so everything beyond — ceiling bands with sodium lamps, walls — is drawn only inside it. From inside, the view is dark with a bright opening at the exit
- sprites are drawn far to near, each clipped to its segment's occlusion line and portal rectangle. Roadside objects are chosen deterministically from the segment index, the section flags and the section's feature; route landmarks come from a table of fixed places

The background is a 1024-pixel panorama: sky bands with scanline dithering, a sun (striped at sunset), parallax clouds, a far ridge with interpolated landmark silhouettes, near hills with village dots, and a coastline strip. The layers are rebuilt into three 512-byte tables at each checkpoint.

## Performance

Every span is one ABI call that takes and releases the firmware's graphics mutex, roughly 2–3 µs on the ESP32-C6. The host harness counts calls: about **3,900 per race frame on average and 6,700 at most** (dense towns), so drawing takes about 10–12 ms, less than the ~25 ms SPI transfer of a full frame. The simulation runs at a fixed 60 Hz (up to four steps per `update`), so the game keeps real time if the frame rate drops. In QEMU the game clock matches wall-clock time.

## Multiplayer

The room is `outbun-napoli97:v1`. Each console publishes its snapshot every step:

| field | content |
|---|---|
| `x` | lateral position (Q8, ±256 = road edges) |
| `y` | absolute route segment of the car (≤ 10,656) |
| `sprite` | car id (2 bits), lean + 4 (3 bits), brake (bit 5) |
| `flags` | ready, racing, finished, timed out |
| `input` | sub-segment fraction (8 bits), speed/16 (8), ingredients (7), leg (4) |

Peers are sorted by player id and bound to racer slots 1–3, which replace CPU slots. Positions glide towards the received value to hide jitter, or snap if more than 30 segments away. A peer silent for 4 s goes back to the CPU. The lobby starts when every visible player is ready or already racing, so a late ready never deadlocks. QEMU's offline stub has no peers, and B in the lobby falls back to a CPU race.

## Audio

`tools/generate_audio.py` composes six original tracks from a compact note notation and packs them with PRG32's `prg32audio_pack.py`: the title serenade, the A-minor race theme with a Neapolitan-sixth turn, the checkpoint jingle, the arrival fanfare, the time-up phrase, and the 6/8 D-minor *tarantella* played from Sorrento onward. Channel N uses instrument N, and all eight are SID-like procedural voices, so the block has **no PCM data** (2.5 KB in all). Channels 5–7 are left to the game: bell for pickups and menus, a pulse *engine* whose pitch follows speed and gear, and bump / horn.

## Testing

- `tests/source_checks.py` checks the brief (the nine legs in order, the four cars), portable-cartridge hygiene, asset and audio limits and metadata. With `PRG32_ROOT` set, it also checks that the stub prototypes match the real PRG32 headers.
- `tests/host_syntax.sh` compiles the cartridge for the host with `-Wall -Wextra -Werror`.
- `tests/harness/run_harness.c` includes `src/game.c` with a software PRG32 (indexed framebuffer, palette, clock, joystick, multiplayer relay, score store) under ASan and UBSan. It plays every screen, and a bot drives the whole route: all eight checkpoints, arrival at Vietri with time left. It also covers time-up, pause and quit, the lobby with a ready handshake, peer binding, CPU takeover of a vanished peer and the offline fallback. Every rectangle is bounds-checked, every palette index must be one the cartridge programs, and the draw-call budget is enforced.
- `tools/render_screens.py` runs the harness with frame dumps and produces the Store screenshot and the gallery, all real output of `src/game.c`.
- The release was also run in PRG32 QEMU: the firmware logs `loaded cartridge 'OutBun-napoli97' (… bytes code, … bytes memory, … bytes audio)`, the title and race tracks play, and the game clock keeps real time (captures in `release-artifacts/qemu/`).

## The 64 KiB adapter

PRG32 commit `596bcf9` raised the firmware's cartridge RAM and package limit to 64 KiB. The Python builder and QEMU stager still fall back to 32 KiB when there is no runtime to ask. `tools/prg32_cli_64.py` patches that constant for its own process only and then runs the normal CLI; it changes no firmware or package format. Remove it once the upstream fallback is updated.
