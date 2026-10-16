"""Offline note segmentation of a reference f0 and voice-range choice (docs/CORPUS.md, "Note labels").

This is NOT the plugin's algorithm: it is non-causal (looks ahead), places a note change at the moment the
smoothed pitch crosses the midpoint between the two notes, and has no stickiness delay. vs-eval then shows
how late the causal pipeline is.
"""

from __future__ import annotations

import numpy as np

from .common import SINGING_RANGES, VOICE_RANGES, F0Track, hz_to_midi

SMOOTH_SECONDS = 0.05     # running median of the fractional MIDI pitch
HYSTERESIS = 0.25         # semitones beyond the half-way point before the quantised note changes
HOLD_SECONDS = 0.08       # a quantised note must last this long to count as a note (vibrato, glides)
MIN_NOTE_SECONDS = 0.06   # shorter notes are merged into a neighbour (or dropped if alone)
BRIDGE_SECONDS = 0.03     # unvoiced / unknown gaps up to this long inside a note do not end it


def _running_nanmedian(x: np.ndarray, width: int) -> np.ndarray:
    if width <= 1:
        return x.copy()
    half = width // 2
    padded = np.pad(x, (half, half), constant_values=np.nan)
    windows = np.lib.stride_tricks.sliding_window_view(padded, 2 * half + 1)
    with np.errstate(all="ignore"):
        import warnings

        with warnings.catch_warnings():
            warnings.simplefilter("ignore", RuntimeWarning)
            return np.nanmedian(windows, axis=1)


def _voiced_runs(voiced: np.ndarray, bridge_frames: int) -> list[tuple[int, int]]:
    """[(first, last)] of voiced frames, gaps of <= bridge_frames bridged."""
    runs = []
    idx = np.flatnonzero(voiced)
    if len(idx) == 0:
        return runs
    start = prev = int(idx[0])
    for i in idx[1:]:
        i = int(i)
        if i - prev - 1 > bridge_frames:
            runs.append((start, prev))
            start = i
        prev = i
    runs.append((start, prev))
    return runs


def segment_notes(track: F0Track, smooth: float = SMOOTH_SECONDS, hysteresis: float = HYSTERESIS,
                  hold: float = HOLD_SECONDS, min_note: float = MIN_NOTE_SECONDS,
                  bridge: float = BRIDGE_SECONDS) -> list[tuple[float, float, int]]:
    """Notes (onset_s, offset_s, midi) of a reference f0 track.

    1. Voiced runs (f0 > 0), gaps <= `bridge` bridged.
    2. Fractional MIDI pitch, running median over `smooth`.
    3. Schmitt quantiser: the note changes only when the pitch is more than 0.5 + `hysteresis` semitones
       from the current note (vibrato of +-50 cents around a slightly sharp note does not flip it).
    4. Quantised stretches shorter than `hold` are transitions (glide, scoop, vibrato peak); stable
       stretches of the same note separated only by transitions merge into one note.
    5. Between two different stable notes the change is placed where the smoothed pitch first gets closer
       to the new note than to the old one; the previous note ends exactly there (legato).
    6. Notes shorter than `min_note` merge into the previous note of the run (or the next one); a run
       without a stable stretch becomes one note (rounded median) if it lasts >= min_note.
    Onset = time of the first voiced frame, offset = last voiced frame + one hop."""
    times, f0 = track.times, track.f0
    if len(times) == 0:
        return []
    hop = track.hop
    midi = hz_to_midi(f0)
    voiced = f0 > 0
    notes: list[tuple[float, float, int]] = []

    for first, last in _voiced_runs(voiced, int(round(bridge / hop))):
        p = midi[first : last + 1]
        ps = _running_nanmedian(p, max(1, int(round(smooth / hop)) | 1))
        valid = ~np.isnan(ps)
        if not valid.any():
            continue

        # 3. Schmitt quantiser
        q = np.full(len(ps), -1, dtype=np.int64)
        current = int(np.round(ps[valid][0]))
        for i in range(len(ps)):
            if valid[i] and abs(ps[i] - current) > 0.5 + hysteresis:
                current = int(np.round(ps[i]))
            q[i] = current

        # 4. stretches of constant q
        stretches = []  # [start, end_inclusive, note]
        s = 0
        for i in range(1, len(q) + 1):
            if i == len(q) or q[i] != q[s]:
                stretches.append([s, i - 1, int(q[s])])
                s = i
        hold_frames = hold / hop
        stable = [st for st in stretches if (st[1] - st[0] + 1) >= hold_frames]

        run_onset = times[first]
        run_offset = times[last] + hop

        if not stable:
            if run_offset - run_onset >= min_note:
                notes.append((run_onset, run_offset, int(np.round(np.nanmedian(ps)))))
            continue

        merged = [list(stable[0])]
        for st in stable[1:]:
            if st[2] == merged[-1][2]:
                merged[-1][1] = st[1]
            else:
                merged.append(list(st))

        # 5. change points between different notes
        run_notes = []
        onset = run_onset
        for k, (_, end, note) in enumerate(merged):
            if k + 1 < len(merged):
                nxt_start, _, nxt = merged[k + 1]
                change = nxt_start
                for i in range(end + 1, nxt_start + 1):
                    if valid[i] and abs(ps[i] - nxt) < abs(ps[i] - note):
                        change = i
                        break
                offset = times[first + change]
            else:
                offset = run_offset
            run_notes.append([onset, offset, note])
            onset = offset

        # 6. short notes merge into a neighbour
        k = 0
        while k < len(run_notes) and len(run_notes) > 1:
            if run_notes[k][1] - run_notes[k][0] < min_note:
                if k > 0:
                    run_notes[k - 1][1] = run_notes[k][1]
                else:
                    run_notes[k + 1][0] = run_notes[k][0]
                del run_notes[k]
                continue
            k += 1
        for onset, offset, note in run_notes:
            if offset - onset >= min_note:
                notes.append((float(onset), float(offset), int(note)))

    return notes


def range_coverage(track: F0Track) -> dict[str, float]:
    """Share of the voiced reference frames inside each range's [f0 min, f0 max]."""
    f = track.f0[track.f0 > 0]
    if len(f) == 0:
        return {name: 0.0 for name in VOICE_RANGES}
    return {name: float(np.mean((f >= lo) & (f <= hi))) for name, (lo, hi) in VOICE_RANGES.items()}


def choose_range(track: F0Track, tie: float = 0.01, minimum: float = 0.97) -> tuple[str, dict[str, float]]:
    """The singing range (bass .. soprano) a singer of this track would pick: among the ranges whose
    coverage is within `tie` of the best, the one whose centre (geometric mean of f0 min and max) is
    closest to the median voiced f0 (log distance). Below `minimum` coverage everywhere: "wide"."""
    coverage = range_coverage(track)
    best = max(coverage[r] for r in SINGING_RANGES)
    if best < minimum:
        return "wide", coverage
    median = float(np.median(track.f0[track.f0 > 0]))
    candidates = [r for r in SINGING_RANGES if coverage[r] >= best - tie]
    return min(candidates, key=lambda r: abs(np.log2(median / np.sqrt(VOICE_RANGES[r][0] * VOICE_RANGES[r][1])))), coverage
