"""End-to-end dry run on the fake datasets: fetch -> convert (-> vs-eval with VS_EVAL=<path>)."""

from __future__ import annotations

import json
import os
import subprocess

import numpy as np
import pytest

from vscorpus import common, convert, fake, fetch


@pytest.fixture(scope="module")
def corpus(tmp_path_factory):
    work = tmp_path_factory.mktemp("corpus")
    paths = fake.make_fake_sources(str(work / "src"))
    for name, path in paths.items():
        fetch.fetch(name, str(work / "raw"), path, str(work / "src" / "manifests"))
    entries = convert.convert(str(work / "raw"), str(work / "eval"), list(paths), 44100, -20.0, "a1", 2, 0,
                              str(work / "src" / "manifests"))
    return work, entries


def test_outputs_and_names(corpus):
    work, entries = corpus
    out = work / "eval"
    names = {e["name"] for e in entries}
    assert {"tenor_vocadito_01", "soprano_vocadito_02"} <= names
    assert len(entries) == 7
    for e in entries:
        rng = e["name"].split("_")[0]
        assert rng in common.VOICE_RANGES and rng == e["range"]
        assert e["name"].split("_")[1] == e["group"]
        for suffix in (".wav", ".f0.csv", ".csv"):
            assert (out / (e["name"] + suffix)).exists()
    # DCS: range from the part letter; VocalSet: from the reference f0
    dcs = {e["part"]: e["range"] for e in entries if e["dataset"] == "dcs"}
    assert all(common.VOICE_RANGES[r] for r in dcs.values())
    assert {"B": "bass", "T": "tenor", "A": "alto", "S": "soprano"}.get(next(iter(dcs))) == dcs[next(iter(dcs))]
    manifest = json.loads((out / "manifest.json").read_text())
    assert manifest["sampleRate"] == 44100 and len(manifest["items"]) == 7
    assert "dcs" in manifest["sources"] and "files" not in manifest["sources"]["dcs"]
    assert "| Group | Range |" in (out / "corpus.md").read_text()


def test_audio_level_and_rate(corpus):
    import soundfile as sf

    work, entries = corpus
    for e in entries:
        y, sr = sf.read(str(work / "eval" / (e["name"] + ".wav")))
        assert sr == 44100 and y.ndim == 1
        assert np.max(np.abs(y)) <= 10 ** (-1 / 20) + 1e-6
        assert abs(e["voicedLevelDb"] - (-20.0)) < 0.5 or e["gainDb"] < 0  # peak limit may win
        assert abs(len(y) / sr - e["seconds"]) < 0.01


def test_reference_matches_the_synthetic_f0(corpus):
    work, entries = corpus
    e = next(e for e in entries if e["name"] == "tenor_vocadito_01")
    rows = np.array(common.read_csv_rows(str(work / "eval" / (e["name"] + ".f0.csv"))))
    voiced = rows[rows[:, 1] > 0]
    # TENOR_MELODY: A3 .. E4
    assert 215 < np.median(voiced[:, 1]) < 335
    labels = common.read_csv_rows(str(work / "eval" / (e["name"] + ".csv")))
    assert [int(r[2]) for r in labels] == [m for m, _ in fake.TENOR_MELODY if m is not None]


@pytest.mark.skipif(not os.environ.get("VS_EVAL"), reason="set VS_EVAL=<path to vs-eval> to run")
def test_vs_eval_on_the_fake_corpus(corpus):
    work, _ = corpus
    result = subprocess.run([os.environ["VS_EVAL"], "--summary-only", "--algorithm", "both", str(work / "eval")],
                            capture_output=True, text=True, check=True)
    summary = json.loads(result.stdout)["summary"]
    for alg in ("yin", "mpm"):
        assert summary[alg]["f0Files"] == 7
        assert summary[alg]["f0"]["rawPitchAccuracy"] > 0.95
        assert summary[alg]["f0"]["octaveErrorRate"] < 0.01
        assert summary[alg]["notes"]["fromRest"]["f1"] > 0.9
