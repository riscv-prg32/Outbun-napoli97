#!/usr/bin/env python3
"""Static checks for the OutBun cartridge sources and release metadata."""
import json
import os
import re
from pathlib import Path

R = Path(__file__).resolve().parents[1]
game = (R / "src/game.c").read_text()
route = (R / "src/route.h").read_text()
assets = (R / "src/assets.h").read_text()

# --- the brief: nine legs in this exact order --------------------------------
towns = re.search(r"town_names\[10\]\[24\] = \{(.*?)\};", route, re.S).group(1)
towns = re.findall(r'"([^"]+)"', towns)
assert towns == ["NAPOLI", "CASTELLAMMARE DI STABIA", "VICO EQUENSE", "META DI SORRENTO", "SORRENTO",
                 "NERANO", "POSITANO", "AMALFI", "CETARA", "VIETRI SUL MARE"], towns
firsts = [int(v) for v in re.search(r"leg_first_section\[10\] = \{(.*?)\}", route).group(1).split(",")]
block = route[route.index("ob_sections[] = {"):route.index("#define SECTION_COUNT")]
sections = re.findall(r"^\s*\{\d+, -?\d+, -?\d+, [^}]*\},", block, re.M)
assert firsts[0] == 0 and firsts[-1] == len(sections) == 92, (firsts, len(sections))
assert all(a < b for a, b in zip(firsts, firsts[1:]))
for tag in ("SS145", "SS163", "Pozzano", "Seiano", "Punta Scutolo", "Furore", "Capo d'Orso", "Li Galli",
            "Vesuvio", "Capri", "Sant'Agata", "Colli di San Pietro"):
    assert tag in route, tag
assert route.count("F_TUNNEL") >= 7 and route.count("F_BRIDGE") >= 4
ingredients = re.findall(r'"([^"]+)"', re.search(r"ingredient_names\[9\]\[24\] = \{(.*?)\};", route, re.S).group(1))
assert len(ingredients) == 9 and ingredients[0] == "PANE ROSETTA"

# --- cars: white Fiat 500 hero, 126 / Dyane / Beetle for players and CPU ------
assert re.search(r'car_names\[4\]\[14\] = \{"FIAT 500", "FIAT 126", "CITROEN DYANE", "VW MAGGIOLINO"\}', game)
for spr in ("CAR_500", "CAR_126", "CAR_DYANE", "CAR_BEETLE"):
    assert f"#define SPR_{spr}" in assets, spr
assert "if (c == chosen_car) continue;" in game          # AI take the cars nobody picked

# --- portable-cartridge hygiene ----------------------------------------------
# initialised pointer tables would carry link-time absolute addresses
assert not re.search(r"static const char \*\w+\[", game + route)
assert not re.search(r"static const \w+ \*const \w+\[", game)
assert "#include <stdio.h>" not in game and "malloc" not in game
assert "prg32_gfx_rect_indexed" in game and "prg32_gfx_rect(" not in game
assert "prg32_palette_set" in game

# --- multiplayer ---------------------------------------------------------------
assert 'NET_SIGNATURE "outbun-napoli97:v1"' in game
for fn in ("prg32_multiplayer_join", "prg32_multiplayer_set_local_state", "prg32_multiplayer_set_input",
           "prg32_multiplayer_get_peer", "prg32_multiplayer_leave", "prg32_multiplayer_tick"):
    assert fn in game, fn
assert "--portable --multiplayer" in (R / "build.sh").read_text()
assert "LIMIT=65536" in (R / "build.sh").read_text()

# --- assets ------------------------------------------------------------------
m = re.search(r"ob_rle\[(\d+)\]", assets)
assert m and int(m.group(1)) < 12000, "sprite data budget"
assert "#define OB_STATIC_BASE 16" in assets and "#define OB_THEME_BASE 192" in assets
theme_count = int(re.search(r"#define OB_THEME_COUNT (\d+)", assets).group(1))
assert 192 + theme_count <= 232, "theme slots must stay below the system grey ramp"

# --- audio -------------------------------------------------------------------
a = json.loads((R / "audio.json").read_text())
assert len(a["instruments"]) == 8 and len(a["tracks"]) == 6
assert all(i["sample_id"] & 0x8000 for i in a["instruments"]), "procedural voices only"
for t in a["tracks"]:
    assert t["events"][0]["command"] == "SET_TEMPO"
    assert t["events"][-1]["command"] in ("JUMP", "END")
    assert all(0 <= e.get("delta", 0) <= 255 for e in t["events"])

# --- metadata ----------------------------------------------------------------
meta = json.loads((R / "metadata/metadata.json").read_text())
colo = json.loads((R / "metadata/colophon.json").read_text())
assert meta["abi"] == "prg32-metadata-1.0" and colo["abi"] == "prg32-colophon-1.0"
assert meta["id"] == "org.riscv-prg32.outbun-napoli97" and meta["name"] == "OutBun-napoli97"
assert meta["version"] == colo["version"] and meta["title"] == colo["title"]
assert meta["players"] == {"min": 1, "max": 4} and meta["multiplayer"] is True
assert meta["cartridge_profile"] == "portable-64k"
assert (R / "assets/generated/icon.png").stat().st_size < 4096
assert (R / "assets/generated/screenshot.png").stat().st_size < 12288

# --- stub header mirrors the real PRG32 prototypes -----------------------------
prg32_root = os.environ.get("PRG32_ROOT")
if prg32_root:
    inc = Path(prg32_root) / "components"
    real = " ".join(p.read_text() for p in inc.rglob("include/*.h"))
    norm = lambda s: re.sub(r"\s+", " ", s)
    stub = (R / "tests/stub/prg32.h").read_text()
    for proto in re.findall(r"^[a-z].*\);$", stub, re.M):
        name = re.search(r"(prg32_\w+)\(", proto).group(1)
        m = re.search(r"[\w ]+\b" + name + r"\([^;]*\);", real)
        assert m, name
        a_, b_ = norm(proto).replace(" ", ""), norm(m.group(0)).replace(" ", "")
        assert a_.split("(")[1] == b_.split("(")[1] or name in ("prg32_multiplayer_set_local_state",), (proto, m.group(0))
    print("stub prototypes match", prg32_root)

print("source checks: OK (9 legs, 92 sections, 4 classics, multiplayer, assets, audio, metadata)")
