"""Tiny fake versions of the three datasets (same archive layout and annotation formats, synthetic
singing-like audio with a known f0) for the tests and dry runs (docs/CORPUS.md, "Testing").

make_fake_sources(dir) writes <dir>/{vocadito,dcs,vocalset}.zip and <dir>/manifests/*.json (with the md5s of
the fake files), so that `corpus.py download --source NAME=<dir>/NAME.zip --manifest-dir <dir>/manifests`
runs exactly the code path of a real download.
"""

from __future__ import annotations

import io
import json
import os
import zipfile

import numpy as np

from .common import md5_file, midi_to_hz
from .datasets import load_manifest


def synth(sr: int, steps: list[tuple[float | None, float]], seed: int = 1, vibrato_cents: float = 30.0,
          vibrato_hz: float = 5.5, glide: float = 0.03, noise_db: float = -55.0) -> tuple[np.ndarray, np.ndarray]:
    """Singing-like tone sequence. steps: (midi or None for a rest, seconds); consecutive notes are legato
    (log glide of `glide` s). Returns (audio, f0 per sample, 0 in rests)."""
    rng = np.random.default_rng(seed)
    n_total = int(round(sum(s for _, s in steps) * sr))
    f0 = np.zeros(n_total)
    env = np.zeros(n_total)
    pos = 0
    prev_midi = None
    for k, (midi, seconds) in enumerate(steps):
        n = int(round(seconds * sr))
        if midi is None:
            prev_midi = None
            pos += n
            continue
        t = np.arange(n) / sr
        target = np.full(n, float(midi))
        if prev_midi is not None:
            g = min(n, int(glide * sr))
            target[:g] = prev_midi + (midi - prev_midi) * (0.5 - 0.5 * np.cos(np.pi * np.arange(g) / max(1, g)))
        depth = np.clip((t - 0.15) / 0.2, 0, 1) * vibrato_cents / 100.0
        target = target + depth * np.sin(2 * np.pi * vibrato_hz * t)
        f0[pos : pos + n] = 440.0 * 2 ** ((target - 69) / 12)
        e = np.ones(n)
        legato_in = prev_midi is not None
        legato_out = k + 1 < len(steps) and steps[k + 1][0] is not None
        if not legato_in:
            a = min(n, int(0.02 * sr))
            e[:a] = np.linspace(0, 1, a)
        if not legato_out:
            r = min(n, int(0.03 * sr))
            e[n - r :] = np.linspace(1, 0, r)
        env[pos : pos + n] = e
        prev_midi = float(midi)
        pos += n

    phase = 2 * np.pi * np.cumsum(f0) / sr
    y = np.zeros(n_total)
    for h in range(1, 13):
        fh = f0 * h
        amp = (1.0 / h ** 1.1) * (1.0 + 1.5 * np.exp(-((fh - 700.0) / 300.0) ** 2) + 0.8 * np.exp(-((fh - 2400.0) / 500.0) ** 2))
        amp = np.where(fh < 0.45 * sr, amp, 0.0)
        y += amp * np.sin(h * phase)
    y = 0.1 * y * env + 10 ** (noise_db / 20) * rng.standard_normal(n_total)
    return y.astype(np.float32), f0


def _wav_bytes(y: np.ndarray, sr: int) -> bytes:
    import soundfile as sf

    buf = io.BytesIO()
    sf.write(buf, y, sr, format="WAV", subtype="PCM_16")
    return buf.getvalue()


def _f0_rows(f0: np.ndarray, sr: int, hop_s: float, duration: float):
    times = np.arange(0.0, duration, hop_s)
    return times, f0[np.minimum((times * sr).astype(int), len(f0) - 1)]


TENOR_MELODY = [(None, 0.3), (57, 0.5), (59, 0.4), (60, 0.6), (None, 0.3), (62, 0.5), (64, 0.5), (None, 0.3), (60, 0.8), (None, 0.4)]
BASS_MELODY = [(None, 0.3), (43, 0.6), (45, 0.5), (None, 0.3), (48, 0.6), (47, 0.5), (None, 0.4)]
SOPRANO_MELODY = [(None, 0.3), (72, 0.5), (74, 0.5), (76, 0.5), (None, 0.3), (79, 0.6), (77, 0.5), (None, 0.4)]
SCALE = [(None, 0.3)] + [(m, 0.35) for m in (48, 50, 52, 53, 55, 57, 59, 60, 59, 57, 55, 53, 52, 50, 48)] + [(None, 0.4)]


def _vocadito(zf: zipfile.ZipFile) -> None:
    sr = 44100
    hop = 256 / sr
    meta = "track_id,singer_id,average_pitch,language\n"
    for n, (melody, seed) in enumerate([(TENOR_MELODY, 1), (SOPRANO_MELODY, 2)], start=1):
        y, f0 = synth(sr, melody, seed)
        duration = len(y) / sr
        zf.writestr(f"vocadito/Audio/vocadito_{n}.wav", _wav_bytes(y, sr))
        t, f = _f0_rows(f0, sr, hop, duration)
        zf.writestr(f"vocadito/Annotations/F0/vocadito_{n}_f0.csv", "".join(f"{a},{b:.3f}\n" for a, b in zip(t, f)))
        rows, onset = [], 0.0
        for midi, seconds in melody:
            if midi is not None:
                rows.append(f"{onset:.9f},{midi_to_hz(midi):.3f},{seconds:.9f}\n")
            onset += seconds
        zf.writestr(f"vocadito/Annotations/Notes/vocadito_{n}_notesA1.csv", "".join(rows))
        zf.writestr(f"vocadito/Annotations/Notes/vocadito_{n}_notesA2.csv", "".join(rows))
        meta += f"{n},S{n},{int(np.median([m for m, _ in melody if m]))},English\n"
    zf.writestr("vocadito/vocadito_metadata.csv", meta)


def _dcs(zf: zipfile.ZipFile, real_manifest: dict) -> dict:
    """Three tracks of the real selection (solo, quartet with manual f0, scale)."""
    sr = 22050
    picks = {}
    for t in real_manifest["tracks"]:
        if t["category"] not in picks and (t["category"] != "quartet" or "f0_manual_lrx" in t):
            picks[t["category"]] = t
    root = "DagstuhlChoirSet_V1.2.3/"
    tracks = []
    melodies = {"B": BASS_MELODY, "T": TENOR_MELODY, "A": TENOR_MELODY, "S": SOPRANO_MELODY}
    for k, (category, t) in enumerate(sorted(picks.items())):
        part = t["id"].split("_")[-1][0]
        melody = SCALE if category == "scale" else melodies[part]
        if part in "AS" and category == "scale":
            melody = [(m + 12 if m else m, s) for m, s in melody]
        if part == "B" and category == "scale":
            melody = [(m - 12 if m else m, s) for m, s in melody]
        y, f0 = synth(sr, melody, 10 + k)
        duration = len(y) / sr
        zf.writestr(root + t["audio"], _wav_bytes(y, sr), compress_type=zipfile.ZIP_STORED if k == 0 else zipfile.ZIP_DEFLATED)
        # LRX recording itself is not used; the annotations of it are
        ct, cf = _f0_rows(f0, sr, 0.01, duration)
        conf = np.where(cf > 0, 0.9, 0.05)
        cf = np.where(cf > 0, cf, 150.0 + 20 * np.sin(ct))  # CREPE always reports a pitch
        zf.writestr(root + t["f0_crepe_lrx"], "".join(f"{a},{b},{c}\n" for a, b, c in zip(ct, cf, conf)))
        pt, pf = _f0_rows(f0, sr, 256 / sr, duration)
        prob = np.where(pf > 0, 0.8, 0.01)
        pf = np.where(pf > 0, pf, -130.8)
        zf.writestr(root + t["f0_pyin_lrx"], "".join(f"{a},{b},{c}\n" for a, b, c in zip(pt, pf, prob)))
        if "f0_manual_lrx" in t:
            mt, mf = _f0_rows(f0, sr, 256 / 44100, duration)
            zf.writestr(root + t["f0_manual_lrx"], "".join(f"{a:.9f},{b:.3f}\n" for a, b in zip(mt, mf) if b > 0))
        tracks.append(t)
    zf.writestr(root + "README.txt", "fake Dagstuhl ChoirSet for tests\n")
    return {"tracks": tracks}


def _vocalset(zf: zipfile.ZipFile) -> None:
    sr = 44100
    files = {
        "FULL/m3/scales/straight/m3_scales_straight_a.wav": (SCALE, 0),
        "FULL/m3/scales/straight/m3_scales_straight_e.wav": (SCALE, 0),  # not chosen: vowel a first
        "FULL/f2/arpeggios/straight/f2_arpeggios_straight_i.wav": ([(m + 12 if m else m, s) for m, s in SCALE[:9]], 12),
        "FULL/f2/arpeggios/belt/f2_arpeggios_belt_a.wav": (SCALE, 12),  # not chosen: not "straight"
    }
    for k, (path, (melody, _)) in enumerate(files.items()):
        y, _ = synth(sr, melody, 20 + k, vibrato_cents=15)
        zf.writestr(path, _wav_bytes(y, sr))
    zf.writestr("__MACOSX/FULL/m3/scales/straight/._m3_scales_straight_a.wav", b"junk")


def make_fake_sources(out_dir: str) -> dict[str, str]:
    """Writes the fake archives + manifests; returns {dataset: archive path}."""
    os.makedirs(os.path.join(out_dir, "manifests"), exist_ok=True)
    real_dcs = load_manifest("dcs")
    paths = {}
    for name in ("vocadito", "dcs", "vocalset"):
        path = os.path.join(out_dir, f"{name}.zip")
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            if name == "vocadito":
                _vocadito(zf)
                extra = {}
            elif name == "dcs":
                extra = _dcs(zf, real_dcs)
            else:
                _vocalset(zf)
                extra = {}
        members = {}
        with zipfile.ZipFile(path) as zf:
            for info in zf.infolist():
                data = zf.read(info)
                import hashlib

                rel = info.filename.split("/", 1)[1] if name in ("vocadito", "dcs") else info.filename
                members[rel] = hashlib.md5(data).hexdigest()
        manifest = {"dataset": name, "version": "fake", "zip_md5": md5_file(path),
                    "members": {} if name == "vocalset" else members, **extra}
        if name == "vocadito":
            manifest["full_download"] = True
        with open(os.path.join(out_dir, "manifests", f"{name}.json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=1)
        paths[name] = path
    return paths
