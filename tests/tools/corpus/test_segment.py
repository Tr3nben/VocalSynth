"""Offline note segmentation and voice-range choice (tools/corpus/vscorpus/segment.py)."""

from __future__ import annotations

import numpy as np

from vscorpus.common import F0Track, midi_to_hz
from vscorpus.segment import choose_range, range_coverage, segment_notes

HOP = 0.005


def track(steps, vibrato_cents=0.0, vibrato_hz=5.5):
    """steps: (midi or None, seconds). Returns an F0Track on a 5 ms grid."""
    f0 = []
    for midi, seconds in steps:
        n = int(round(seconds / HOP))
        if midi is None:
            f0 += [0.0] * n
            continue
        t = np.arange(n) * HOP
        cents = vibrato_cents * np.sin(2 * np.pi * vibrato_hz * t)
        f0 += list(midi_to_hz(midi) * 2 ** (cents / 1200.0))
    f0 = np.array(f0)
    return F0Track(np.arange(len(f0)) * HOP, f0)


def test_separate_notes_after_rests():
    notes = segment_notes(track([(None, 0.2), (60, 0.5), (None, 0.2), (64, 0.4), (None, 0.1)]))
    assert [n[2] for n in notes] == [60, 64]
    assert abs(notes[0][0] - 0.2) < 1e-9 and abs(notes[0][1] - 0.7) < 1e-9
    assert abs(notes[1][0] - 0.9) < 1e-9 and abs(notes[1][1] - 1.3) < 1e-9


def test_legato_change_at_the_midpoint():
    notes = segment_notes(track([(None, 0.1), (60, 0.4), (62, 0.4), (None, 0.1)]))
    assert [n[2] for n in notes] == [60, 62]
    # the change sits where the (median-smoothed) pitch gets closer to 62 than to 60, i.e. at 0.5 s
    assert abs(notes[0][1] - 0.5) <= 0.03
    assert notes[0][1] == notes[1][0]


def test_vibrato_does_not_split_a_note():
    notes = segment_notes(track([(None, 0.1), (57, 1.0), (None, 0.1)], vibrato_cents=60.0))
    assert [n[2] for n in notes] == [57]


def test_short_blips_are_merged_or_dropped():
    # a 30 ms excursion inside a long note is not a note of its own
    notes = segment_notes(track([(None, 0.1), (60, 0.3), (65, 0.03), (60, 0.3), (None, 0.1)]))
    assert [n[2] for n in notes] == [60]
    # an isolated 40 ms voiced island is shorter than the minimum note
    assert segment_notes(track([(None, 0.1), (60, 0.04), (None, 0.1)])) == []


def test_short_gap_is_bridged():
    notes = segment_notes(track([(None, 0.1), (60, 0.3), (None, 0.02), (60, 0.3), (None, 0.1)]))
    assert len(notes) == 1


def test_scale_gives_every_step():
    steps = [(None, 0.2)] + [(m, 0.3) for m in (48, 50, 52, 53, 55, 57, 59, 60)] + [(None, 0.2)]
    assert [n[2] for n in segment_notes(track(steps))] == [48, 50, 52, 53, 55, 57, 59, 60]


def test_choose_range():
    tenor = track([(m, 0.3) for m in (52, 55, 57, 60, 64, 67)])
    assert choose_range(tenor)[0] == "tenor"
    bass = track([(m, 0.3) for m in (40, 43, 45, 48, 52)])
    assert choose_range(bass)[0] == "bass"
    soprano = track([(m, 0.3) for m in (67, 72, 76, 79, 84)])
    assert choose_range(soprano)[0] == "soprano"
    # C2 .. C6: no singing range covers it
    wide = track([(36, 0.3), (84, 0.3)])
    assert choose_range(wide)[0] == "wide"
    assert range_coverage(F0Track(np.arange(3) * HOP, np.zeros(3)))["tenor"] == 0.0
