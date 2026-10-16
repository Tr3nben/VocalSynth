"""Reference f0 construction (tools/corpus/vscorpus/reference.py) and the CSV writers (common.py)."""

from __future__ import annotations

import os
import re

import numpy as np
import pytest

from conftest import REPO
from vscorpus import common, reference
from vscorpus.common import UNKNOWN, F0Track


def test_voice_ranges_match_the_core():
    with open(os.path.join(REPO, "core", "include", "vocalsynth", "VoiceSettings.h"), encoding="utf-8") as f:
        text = f.read()
    table = dict((n, (float(a), float(b))) for n, a, b in re.findall(r'\{\s*"(\w+)",\s*([\d.]+),\s*([\d.]+)\s*\}', text))
    assert table == common.VOICE_RANGES


def test_to_grid_interpolates_and_marks_gaps():
    times = np.array([0.0, 0.01, 0.02, 0.03, 0.08, 0.09])
    f0 = np.array([100.0, 200.0, 0.0, 0.0, 300.0, 300.0])
    tr = reference.to_grid(times, f0, 0.12)
    assert np.allclose(tr.times[:3], [0.0, 0.005, 0.01])
    # halfway between 100 and 200 Hz in log frequency
    assert abs(tr.f0[1] - np.sqrt(100 * 200)) < 1e-6
    assert tr.f0[4] == 0.0                     # 0.02: unvoiced sample
    assert tr.f0[11] == UNKNOWN                 # 0.055: more than 0.75 source hops from any sample
    assert tr.f0[16] == 300.0
    assert tr.f0[-1] == UNKNOWN                 # 0.115 > last sample + 0.75 hop


def test_manual_reference_span_only():
    rows = [[0.5 + 0.01 * k, 220.0] for k in range(20)] + [[1.0 + 0.01 * k, 330.0] for k in range(20)]
    tr = reference.manual_reference(rows, 2.0)
    at = lambda t: tr.f0[int(round(t / reference.GRID_HOP))]  # noqa: E731
    assert at(0.2) == UNKNOWN           # before the annotated span
    assert at(0.6) == pytest.approx(220.0)
    assert at(0.85) == 0.0              # gap inside the span: unvoiced
    assert at(1.1) == pytest.approx(330.0)
    assert at(1.6) == UNKNOWN           # after the span


def test_crepe_pyin_agreement_rule():
    t = np.arange(0, 1.0, 0.01)
    crepe_f0 = np.full(t.shape, 220.0)
    conf = np.full(t.shape, 0.9)
    pyin_f0 = np.full(t.shape, 220.0)
    conf[:20] = 0.1
    pyin_f0[:20] = -220.0                                        # both unvoiced -> 0
    conf[20:30] = 0.4                                            # CREPE unsure -> unknown
    pyin_f0[40:60] = 220.0 * 2 ** (100 / 1200)                   # 100 cents apart -> unknown
    pyin_f0[80:82] = -220.0                                      # pYIN unvoiced, CREPE sure -> unknown
    crepe_rows = np.stack([t, crepe_f0, conf], axis=1)
    pyin_rows = np.stack([t, pyin_f0, np.full(t.shape, 0.8)], axis=1)
    tr = reference.combine_crepe_pyin(crepe_rows, pyin_rows, 1.0)
    at = lambda s: tr.f0[int(round(s / reference.GRID_HOP))]  # noqa: E731
    assert at(0.1) == 0.0
    assert at(0.25) == UNKNOWN
    assert at(0.35) == pytest.approx(220.0)
    assert at(0.5) == UNKNOWN
    assert at(0.7) == pytest.approx(220.0)
    assert at(0.805) == UNKNOWN


def test_drop_short_voiced_runs():
    t = np.arange(0, 0.2, 0.01)
    f0 = np.zeros(t.shape)
    f0[5:7] = 200.0     # 20 ms island -> unknown
    f0[10:15] = 200.0   # 50 ms -> kept
    out = reference.drop_short_voiced_runs(t, f0)
    assert (out[5:7] == UNKNOWN).all()
    assert (out[10:15] == 200.0).all()


def test_csv_writers_round_trip(tmp_path):
    tr = F0Track(np.array([0.0, 0.005, 0.01]), np.array([0.0, 220.0, UNKNOWN]))
    common.write_f0_csv(str(tmp_path / "a.f0.csv"), tr, "test")
    rows = common.read_csv_rows(str(tmp_path / "a.f0.csv"))
    assert rows == [[0.0, 0.0], [0.005, 220.0], [0.01, -1.0]]
    common.write_notes_csv(str(tmp_path / "a.csv"), [(0.1, 0.5, 60)], "x\ny")
    text = (tmp_path / "a.csv").read_text()
    assert "onset_s,offset_s,midi_note" in text and "0.100000,0.500000,60" in text
