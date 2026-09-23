#!/usr/bin/env python3
"""Compose the OutBun soundtrack into audio.json (packed by PRG32's
tools/prg32audio_pack.py).

All music is original. The PRG32 tracker binds channel N to instrument N, so
the eight instruments below double as the channel plan:

    0 lead (pulse)      1 harmony/arp (saw)    2 bass (triangle)
    3 hi-hat (noise)    4 kick/snare (noise)   5 bell - game SFX
    6 engine - game     7 bump / horn - game

Every instrument is a SID-like procedural voice (sample_id bit 15), so the
AUDIO block carries no PCM bytes at all.

Notation: "A4:4" = note A4 for 4 ticks; a tick is a 16th note at the track
tempo; "-:n" is a rest. Bars are separated by "|" (ignored, for reading).
"""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

NOTE_BASE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def midi(name: str) -> int:
    letter, rest = name[0], name[1:]
    acc = 0
    while rest and rest[0] in "#b":
        acc += 1 if rest[0] == "#" else -1
        rest = rest[1:]
    return 12 * (int(rest) + 1) + NOTE_BASE[letter] + acc


def synth(wave: int, pw: int, cutoff: int, res: int) -> int:
    return 0x8000 | ((res & 3) << 10) | ((cutoff & 15) << 6) | ((pw & 15) << 2) | (wave & 3)


TRI, SAW, PULSE, NOISE = 0, 1, 2, 3
INSTRUMENTS = [
    dict(sample_id=synth(PULSE, 6, 12, 1), default_volume=190, default_pan=-8, attack=2, decay=24, sustain=170, release=26),
    dict(sample_id=synth(SAW, 8, 8, 1), default_volume=120, default_pan=30, attack=4, decay=30, sustain=120, release=30),
    dict(sample_id=synth(TRI, 8, 15, 0), default_volume=235, default_pan=0, attack=1, decay=20, sustain=200, release=16),
    dict(sample_id=synth(NOISE, 8, 15, 0), default_volume=80, default_pan=24, attack=0, decay=5, sustain=0, release=4),
    dict(sample_id=synth(NOISE, 8, 5, 1), default_volume=170, default_pan=-16, attack=0, decay=14, sustain=0, release=8),
    dict(sample_id=synth(TRI, 8, 15, 0), default_volume=210, default_pan=0, attack=0, decay=34, sustain=0, release=30),
    dict(sample_id=synth(PULSE, 3, 5, 2), default_volume=150, default_pan=0, attack=0, decay=0, sustain=255, release=12),
    dict(sample_id=synth(PULSE, 8, 10, 1), default_volume=200, default_pan=0, attack=0, decay=18, sustain=140, release=20),
]


def parse(line: str):
    out = []
    for tok in line.replace("|", " ").split():
        name, dur = tok.split(":")
        out.append((None if name == "-" else midi(name), int(dur)))
    return out


def repeat_pattern(pattern: str, times: int) -> str:
    return " ".join([pattern] * times)


def arp(chords: list[tuple[str, ...]], steps_per_bar: int = 8, dur: int = 2) -> str:
    bars = []
    for chord in chords:
        seq = list(chord) + list(chord[1:-1][::-1])
        bars.append(" ".join(f"{seq[i % len(seq)]}:{dur}" for i in range(steps_per_bar)))
    return " | ".join(bars)


def bass_octaves(roots: list[str]) -> str:
    bars = []
    for r in roots:
        lo = r
        hi = r[:-1] + str(int(r[-1]) + 1)
        bars.append(" ".join(f"{lo if i % 2 == 0 else hi}:2" for i in range(8)))
    return " | ".join(bars)


def compile_track(tempo: int, channels: dict[int, str], loop: bool, hold_last: bool = True):
    """Merge channel lines into one tracker event list."""
    timeline = []  # (time, order, command, arg0, arg1)
    length = 0
    for ch, line in channels.items():
        t = 0
        notes = parse(line)
        for i, (note, dur) in enumerate(notes):
            if note is not None:
                timeline.append((t, 1, "NOTE_ON", ch, note))
                percussive = ch in (3, 4)
                last = i == len(notes) - 1
                if not percussive and not (last and hold_last and not loop):
                    nxt = notes[i + 1][0] if i + 1 < len(notes) else None
                    if nxt is None:  # release before a rest or the loop point
                        timeline.append((t + dur, 0, "NOTE_OFF", ch, 0))
            t += dur
        length = max(length, t)
    timeline.sort(key=lambda e: (e[0], e[1], e[3]))
    events = [{"delta": 0, "command": "SET_TEMPO", "arg0": tempo}]
    prev_t = 0
    for t, _, cmd, a0, a1 in timeline:
        gap = t - prev_t
        while gap > 255:
            events[-1]["delta"] += 255
            events.append({"delta": 0, "command": "SET_VOLUME", "arg0": 7, "arg1": 200})
            gap -= 255
        events[-1]["delta"] += gap
        ev = {"delta": 0, "command": cmd, "arg0": a0}
        if cmd == "NOTE_ON":
            ev["arg1"] = a1
        events.append(ev)
        prev_t = t
    events[-1]["delta"] += length - prev_t
    if loop:
        events.append({"delta": 0, "command": "JUMP", "arg0": 1, "arg1": 0})
    else:
        events.append({"delta": 0, "command": "END", "arg0": 0})
    return events


def title_track():
    lead = ("G4:6 C5:2 E5:4 D5:4 | C5:6 A4:2 E5:8 | F5:6 E5:2 D5:4 C5:4 | D5:12 G4:4 | "
            "E5:6 G5:2 C6:4 B5:4 | A5:6 E5:2 C5:8 | F5:4 E5:4 D5:4 F5:4 | E5:4 D5:4 G5:8")
    pad = "E4:16 | C4:16 | A4:16 | B4:16 | G4:16 | E4:16 | A4:16 | B4:16"
    bass = ("C3:8 G2:8 | A2:8 E2:8 | F2:8 C3:8 | G2:8 D3:8 | C3:8 G2:8 | A2:8 E3:8 | D3:8 A2:8 | G2:8 B2:8")
    hats = repeat_pattern("-:4 C7:4 -:4 C7:4", 8)
    return compile_track(116, {0: lead, 1: pad, 2: bass, 3: hats}, loop=True)


def race_track():
    lead = ("E5:4 A5:4 G5:2 E5:2 C5:4 | D5:2 C5:2 A4:4 F5:6 E5:2 | D5:4 G5:4 F5:2 D5:2 B4:4 | C5:2 D5:2 E5:4 G5:8 | "
            "F5:4 A5:4 G5:2 F5:2 D5:4 | F5:2 D5:2 Bb4:4 D5:6 F5:2 | E5:4 G#5:4 B5:4 G#5:4 | A5:2 G#5:2 F5:2 E5:2 D5:4 B4:4")
    chords = [("A4", "C5", "E5"), ("F4", "A4", "C5"), ("G4", "B4", "D5"), ("C5", "E5", "G5"),
              ("D4", "F4", "A4"), ("Bb4", "D5", "F5"), ("E4", "G#4", "B4"), ("E4", "G#4", "D5")]
    harmony = arp(chords)
    bass = bass_octaves(["A2", "F2", "G2", "C3", "D2", "Bb2", "E2", "E2"])
    hats = repeat_pattern("-:2 C7:2", 32)
    drums = repeat_pattern("C2:4 E4:4 C2:2 C2:2 E4:4", 8)
    return compile_track(150, {0: lead, 1: harmony, 2: bass, 3: hats, 4: drums}, loop=True)


def tarantella_track():
    """6/8 race theme for the Amalfi coast: bars of 12 ticks (six quavers)."""
    lead = ("A4:2 D5:2 F5:2 A5:4 F5:2 | G5:2 F5:2 E5:2 D5:4 A4:2 | C#5:2 E5:2 G5:2 A5:4 G5:2 | F5:2 E5:2 D5:2 A4:6 | "
            "Bb4:2 D5:2 G5:2 Bb5:4 A5:2 | A5:2 G5:2 F5:2 D5:4 F5:2 | E5:2 F5:2 G5:2 A5:2 C#5:2 E5:2 | D5:6 A4:2 C#5:2 E5:2")
    chords = [("D4", "F4", "A4"), ("D4", "F4", "A4"), ("C#4", "E4", "A4"), ("D4", "F4", "A4"),
              ("D4", "G4", "Bb4"), ("D4", "F4", "A4"), ("C#4", "E4", "G4"), ("D4", "F4", "A4")]
    harmony = arp(chords, steps_per_bar=6)
    roots = [("D2", "A2"), ("D2", "A2"), ("A2", "E2"), ("D2", "A2"), ("G2", "D2"), ("D2", "A2"), ("A2", "E2"), ("D2", "A2")]
    bass = " | ".join(f"{r}:2 {f}:2 {r[:-1]}3:2 {f}:2 {r}:2 {f}:2" for r, f in roots)
    hats = repeat_pattern("-:2 C7:2 C7:2 -:2 C7:2 C7:2", 8)
    drums = repeat_pattern("C2:6 E4:6", 8)
    return compile_track(150, {0: lead, 1: harmony, 2: bass, 3: hats, 4: drums}, loop=True)


def checkpoint_jingle():
    return compile_track(160, {0: "G5:1 C6:1 E6:1 G6:5", 1: "E5:1 G5:1 C6:1 E6:5", 2: "C3:2 G3:2 C4:4",
                               4: "C2:2 E4:2 E4:2 E4:2"}, loop=False)


def goal_fanfare():
    lead = "C5:2 E5:2 G5:2 C6:6 | A5:2 C6:2 E6:4 D6:4 B5:4 | C6:16"
    harm = arp([("C5", "E5", "G5"), ("F5", "A5", "C6"), ("C5", "E5", "G5")], steps_per_bar=8)
    bass = "C3:8 G2:8 | F2:8 G2:8 | C3:16"
    drums = "C2:4 E4:4 C2:4 E4:4 | C2:4 E4:4 C2:2 E4:2 E4:2 E4:2 | C2:16"
    return compile_track(132, {0: lead, 1: harm, 2: bass, 4: drums}, loop=False)


def time_up():
    return compile_track(90, {0: "E5:4 D#5:4 D5:4 C#5:12", 1: "C5:4 B4:4 Bb4:4 A4:12", 2: "A2:8 F2:8 E2:8"}, loop=False)


def main():
    tracks = [title_track(), race_track(), checkpoint_jingle(), goal_fanfare(), time_up(), tarantella_track()]
    data = {"instruments": INSTRUMENTS, "tracks": [{"events": t} for t in tracks]}
    out = ROOT / "audio.json"
    out.write_text(json.dumps(data, indent=1) + "\n")
    n = sum(len(t) for t in tracks)
    print(f"audio: {len(tracks)} tracks, {n} events (~{n * 4 + 8 * 8 + 64} bytes packed)")


if __name__ == "__main__":
    main()
