"""Reference f0 tracks for vs-eval (docs/CORPUS.md, "Reference f0"). numpy only, except pyin_reference().

All tracks end up on a uniform GRID_HOP grid with f0 > 0 voiced, 0 unvoiced, UNKNOWN (-1) not scored.
"""

from __future__ import annotations

import numpy as np

from .common import UNKNOWN, F0Track

GRID_HOP = 0.005  # s

# CREPE + pYIN agreement rule (Dagstuhl ChoirSet, both computed on the larynx microphone)
CREPE_VOICED_CONFIDENCE = 0.5
CREPE_UNVOICED_CONFIDENCE = 0.3
AGREEMENT_CENTS = 50.0

# pYIN reference (VocalSet): voiced probability thresholds
PYIN_VOICED_PROBABILITY = 0.5
PYIN_UNVOICED_PROBABILITY = 0.2

MIN_VOICED_RUN = 0.03  # s: shorter voiced islands of an automatic reference become unknown


def to_grid(times, f0, duration: float, missing: float = UNKNOWN, span_only: bool = False, hop: float = GRID_HOP) -> F0Track:
    """Resamples a (time, f0) track onto the uniform grid 0, hop, ... < duration.

    Between two voiced samples at most 1.5 source hops apart: linear in log-frequency. Otherwise the
    nearest source sample if it is within 0.75 source hop, else `missing` (UNKNOWN for complete tracks,
    0 for tracks that list only voiced frames). span_only: grid points before the first or after the
    last source sample are UNKNOWN (partly annotated recordings)."""
    times = np.asarray(times, dtype=np.float64)
    f0 = np.asarray(f0, dtype=np.float64)
    order = np.argsort(times, kind="stable")
    times, f0 = times[order], f0[order]
    grid = np.arange(0.0, max(duration, 0.0), hop)
    out = np.full(grid.shape, missing, dtype=np.float64)
    if len(times) == 0:
        return F0Track(grid, out)

    src_hop = float(np.median(np.diff(times))) if len(times) > 1 else hop
    upper = np.searchsorted(times, grid, side="left")
    lo = np.clip(upper - 1, 0, len(times) - 1)
    hi = np.clip(upper, 0, len(times) - 1)

    d_lo = np.abs(grid - times[lo])
    d_hi = np.abs(times[hi] - grid)
    nearest = np.where(d_lo <= d_hi, lo, hi)
    near_ok = np.abs(times[nearest] - grid) <= 0.75 * src_hop
    out[near_ok] = f0[nearest[near_ok]]

    both = (upper > 0) & (upper < len(times)) & (f0[lo] > 0) & (f0[hi] > 0) & ((times[hi] - times[lo]) <= 1.5 * src_hop)
    w = np.zeros_like(grid)
    span = times[hi] - times[lo]
    np.divide(grid - times[lo], span, out=w, where=span > 0)
    interpolated = np.exp2((1 - w) * np.log2(np.where(f0[lo] > 0, f0[lo], 1.0)) + w * np.log2(np.where(f0[hi] > 0, f0[hi], 1.0)))
    out[both] = interpolated[both]
    out[(out < 0)] = UNKNOWN

    if span_only:
        out[(grid < times[0] - 0.75 * src_hop) | (grid > times[-1] + 0.75 * src_hop)] = UNKNOWN
    return F0Track(grid, out)


def combine_crepe_pyin(crepe_rows, pyin_rows, duration: float) -> F0Track:
    """Dagstuhl ChoirSet automatic reference from the larynx microphone (no crosstalk from other singers).

    crepe_rows: (time, f0, confidence); pyin_rows: (time, f0, probability) with f0 < 0 = unvoiced (sign
    convention of the dataset). On CREPE's grid:
      voiced    pYIN voiced, CREPE confidence >= 0.5 and both within 50 cents -> CREPE's f0
      unvoiced  pYIN unvoiced and CREPE confidence < 0.3
      unknown   everything else (they disagree)
    Voiced islands shorter than 30 ms become unknown. Then resampled to the grid."""
    crepe = np.asarray(crepe_rows, dtype=np.float64)
    pyin = np.asarray(pyin_rows, dtype=np.float64)
    t = crepe[:, 0]
    cf0, conf = crepe[:, 1], crepe[:, 2]
    idx = np.clip(np.searchsorted(pyin[:, 0], t), 0, len(pyin) - 1)
    prev = np.clip(idx - 1, 0, len(pyin) - 1)
    idx = np.where(np.abs(pyin[prev, 0] - t) < np.abs(pyin[idx, 0] - t), prev, idx)
    pf0 = pyin[idx, 1]
    pyin_voiced = pf0 > 0

    out = np.full(t.shape, UNKNOWN)
    cents = np.full(t.shape, np.inf)
    ok = pyin_voiced & (cf0 > 0)
    cents[ok] = 1200.0 * np.abs(np.log2(cf0[ok] / pf0[ok]))
    voiced = pyin_voiced & (conf >= CREPE_VOICED_CONFIDENCE) & (cents <= AGREEMENT_CENTS)
    unvoiced = (~pyin_voiced) & (conf < CREPE_UNVOICED_CONFIDENCE)
    out[voiced] = cf0[voiced]
    out[unvoiced] = 0.0
    out = drop_short_voiced_runs(t, out)
    return to_grid(t, out, duration)


def manual_reference(rows, duration: float) -> F0Track:
    """Dagstuhl ChoirSet manual annotation (time, f0): lists the voiced frames; gaps inside the annotated
    span are unvoiced, outside it unknown."""
    rows = np.asarray(rows, dtype=np.float64)
    f0 = np.where(rows[:, 1] > 0, rows[:, 1], 0.0)
    return to_grid(rows[:, 0], f0, duration, missing=0.0, span_only=True)


def annotated_reference(rows, duration: float) -> F0Track:
    """vocadito f0 annotation (time, f0) on a complete grid, 0 = unvoiced."""
    rows = np.asarray(rows, dtype=np.float64)
    f0 = np.where(rows[:, 1] > 0, rows[:, 1], 0.0)
    return to_grid(rows[:, 0], f0, duration)


def drop_short_voiced_runs(times, f0, min_run: float = MIN_VOICED_RUN):
    f0 = np.array(f0, dtype=np.float64)
    hop = float(np.median(np.diff(times))) if len(times) > 1 else GRID_HOP
    voiced = f0 > 0
    i = 0
    n = len(f0)
    while i < n:
        if not voiced[i]:
            i += 1
            continue
        j = i
        while j + 1 < n and voiced[j + 1]:
            j += 1
        if (j - i + 1) * hop < min_run:
            f0[i : j + 1] = UNKNOWN
        i = j + 1
    return f0


def pyin_reference(y: np.ndarray, sr: int, fmin: float, fmax: float, duration: float) -> F0Track:
    """Automatic reference for recordings without annotations (VocalSet): librosa.pyin on the original
    audio (frame 2048 / hop 256 at 44.1 kHz, scaled for other rates). voiced: pYIN voiced flag and voiced
    probability >= 0.5; unvoiced: no voiced flag and probability < 0.2; else unknown."""
    import librosa  # heavy: only here

    scale = sr / 44100.0
    frame = int(2 ** round(np.log2(2048 * scale)))
    hop = max(64, int(round(256 * scale)))
    f0, flag, prob = librosa.pyin(y.astype(np.float32), fmin=fmin, fmax=fmax, sr=sr, frame_length=frame,
                                  hop_length=hop, center=True)
    t = librosa.times_like(f0, sr=sr, hop_length=hop)
    out = np.full(f0.shape, UNKNOWN)
    voiced = flag & (prob >= PYIN_VOICED_PROBABILITY) & np.isfinite(f0)
    out[voiced] = f0[voiced]
    out[(~flag) & (prob < PYIN_UNVOICED_PROBABILITY)] = 0.0
    out = drop_short_voiced_runs(t, out)
    return to_grid(t, out, duration)
