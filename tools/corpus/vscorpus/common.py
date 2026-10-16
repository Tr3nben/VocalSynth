"""Shared helpers of the corpus tooling (docs/CORPUS.md). Standard library + numpy only."""

from __future__ import annotations

import hashlib
import json
import math
import os
import sys
from dataclasses import dataclass

import numpy as np

# Must match core/include/vocalsynth/VoiceSettings.h (voiceRangeTable); tests/tools/corpus checks it.
VOICE_RANGES = {
    "bass": (65.0, 330.0),
    "baritone": (82.0, 392.0),
    "tenor": (110.0, 523.0),
    "alto": (147.0, 698.0),
    "soprano": (196.0, 1047.0),
    "whistle": (500.0, 2500.0),
    "wide": (65.0, 1047.0),
}

# Ranges a singer can be mapped to automatically (Whistle is for whistling, Wide is the fallback)
SINGING_RANGES = ("bass", "baritone", "tenor", "alto", "soprano")

UNKNOWN = -1.0  # reference f0 value "not scored" (vs-eval: f0 < 0)


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def md5_file(path: str, chunk: int = 1 << 20) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        while True:
            block = f.read(chunk)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def hz_to_midi(f0: np.ndarray) -> np.ndarray:
    """Fractional MIDI note numbers; NaN where f0 <= 0."""
    f0 = np.asarray(f0, dtype=np.float64)
    out = np.full(f0.shape, np.nan)
    voiced = f0 > 0
    out[voiced] = 69.0 + 12.0 * np.log2(f0[voiced] / 440.0)
    return out


def midi_to_hz(midi: float) -> float:
    return 440.0 * 2.0 ** ((midi - 69.0) / 12.0)


@dataclass
class F0Track:
    """A reference f0 track: f0 > 0 voiced, 0 unvoiced, UNKNOWN (< 0) not scored."""

    times: np.ndarray
    f0: np.ndarray

    @property
    def hop(self) -> float:
        if len(self.times) < 2:
            return 0.01
        return float(np.median(np.diff(self.times)))


def write_f0_csv(path: str, track: F0Track, comment: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# vs-eval reference f0 (docs/CORPUS.md): f0 > 0 voiced, 0 unvoiced, -1 unknown (not scored)\n")
        for line in comment.splitlines():
            f.write(f"# {line}\n")
        f.write("time_s,f0_hz\n")
        for t, v in zip(track.times, track.f0):
            if v > 0:
                f.write(f"{t:.4f},{v:.3f}\n")
            else:
                f.write(f"{t:.4f},{0 if v == 0 else -1}\n")


def write_notes_csv(path: str, notes: list[tuple[float, float, int]], comment: str) -> None:
    """Labels in the format vs-eval reads (tools/vs-eval/Metrics.h readLabelsCsv)."""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for line in comment.splitlines():
            f.write(f"# {line}\n")
        f.write("onset_s,offset_s,midi_note\n")
        for onset, offset, midi in notes:
            f.write(f"{onset:.6f},{offset:.6f},{int(midi)}\n")


def read_csv_rows(path: str) -> list[list[float]]:
    """Numeric rows of a CSV (',', ';' or whitespace separated); header, comments and empty lines skipped."""
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.replace(";", ",").replace("\t", ",").replace(" ", ",").split(",")
            parts = [p for p in parts if p != ""]
            try:
                rows.append([float(p) for p in parts])
            except ValueError:
                if rows:
                    raise ValueError(f"{path}: non-numeric row after data: {line!r}")
    return rows


def write_json(path: str, data) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1, sort_keys=False)
        f.write("\n")
    os.replace(tmp, path)


def db(x: float) -> float:
    return 20.0 * math.log10(max(x, 1e-12))
