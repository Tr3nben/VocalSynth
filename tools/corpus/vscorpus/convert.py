"""Converts raw/<dataset>/ into vs-eval inputs (docs/CORPUS.md, "Output"):

  <out>/<range>_<group>_<id>.wav      mono float32 at the target rate, level-normalised
  <out>/<range>_<group>_<id>.f0.csv   reference f0 (time_s,f0_hz; 0 unvoiced, -1 unknown)
  <out>/<range>_<group>_<id>.csv      note labels (onset_s,offset_s,midi_note)
  <out>/manifest.json, <out>/corpus.md
"""

from __future__ import annotations

import json
import math
import os
import re
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field

import numpy as np

from . import reference, segment
from .common import F0Track, db, hz_to_midi, log, read_csv_rows, write_f0_csv, write_json, write_notes_csv
from .datasets import DATASETS, load_manifest

DEFAULT_LEVEL_DB = -20.0  # RMS of the voiced parts after normalisation (dBFS)
PEAK_LIMIT_DB = -1.0
DCS_PARTS = {"S": "soprano", "A": "alto", "T": "tenor", "B": "bass"}
VOCALSET_PYIN_LIMITS = {"m": (60.0, 700.0), "f": (120.0, 1300.0)}


@dataclass
class Job:
    dataset: str
    group: str
    item_id: str
    audio: str
    range_name: str | None          # None = choose from the reference f0
    reference_kind: str             # "annotation" | "manual" | "crepe+pyin" | "pyin"
    reference_files: dict = field(default_factory=dict)
    notes_kind: str = "f0"          # "f0" (segment_notes) | "a1" | "a2"
    notes_file: str | None = None
    extra: dict = field(default_factory=dict)


def _load_audio(path: str) -> tuple[np.ndarray, int]:
    import soundfile as sf

    y, sr = sf.read(path, dtype="float64", always_2d=True)
    return y.mean(axis=1), int(sr)


def _resample(y: np.ndarray, sr: int, rate: int) -> np.ndarray:
    if sr == rate:
        return y
    import soxr

    return soxr.resample(y, sr, rate, quality="VHQ")


def _normalise(y: np.ndarray, rate: int, track: F0Track, level_db: float) -> tuple[np.ndarray, float, float]:
    """Scales so the RMS of the reference-voiced parts is level_db (peak <= PEAK_LIMIT_DB)."""
    mask = np.zeros(len(y), dtype=bool)
    half = track.hop / 2
    for t in track.times[track.f0 > 0]:
        mask[max(0, int((t - half) * rate)) : int((t + half) * rate) + 1] = True
    voiced = y[mask] if mask.any() else y
    rms = float(np.sqrt(np.mean(voiced ** 2))) if len(voiced) else 0.0
    if rms <= 0:
        return y, 0.0, -math.inf
    gain_db = level_db - db(rms)
    peak = float(np.max(np.abs(y)))
    gain_db = min(gain_db, PEAK_LIMIT_DB - db(peak)) if peak > 0 else gain_db
    return y * 10 ** (gain_db / 20), gain_db, db(rms) + gain_db


def _notes_from_annotation(path: str) -> list[tuple[float, float, int]]:
    """vocadito notes: onset_s, pitch_hz, duration_s."""
    notes = []
    for row in read_csv_rows(path):
        if len(row) >= 3 and row[1] > 0 and row[2] > 0:
            midi = int(round(float(hz_to_midi(np.array([row[1]]))[0])))
            notes.append((row[0], row[0] + row[2], midi))
    return sorted(notes)


def run_job(job: Job, out_dir: str, rate: int, level_db: float) -> dict:
    y, sr = _load_audio(job.audio)
    duration = len(y) / sr

    if job.reference_kind == "annotation":
        track = reference.annotated_reference(read_csv_rows(job.reference_files["f0"]), duration)
    elif job.reference_kind == "manual":
        track = reference.manual_reference(read_csv_rows(job.reference_files["manual"]), duration)
    elif job.reference_kind == "crepe+pyin":
        track = reference.combine_crepe_pyin(read_csv_rows(job.reference_files["crepe"]),
                                             read_csv_rows(job.reference_files["pyin"]), duration)
    elif job.reference_kind == "pyin":
        fmin, fmax = job.extra["pyin_limits"]
        track = reference.pyin_reference(y, sr, fmin, fmax, duration)
    else:
        raise ValueError(job.reference_kind)

    chosen, coverage = segment.choose_range(track)
    range_name = job.range_name or chosen

    if job.notes_kind in ("a1", "a2") and job.notes_file:
        notes = _notes_from_annotation(job.notes_file)
        notes_source = f"vocadito annotator {job.notes_kind.upper()}"
    else:
        notes = segment.segment_notes(track)
        notes_source = "segment_notes() of the reference f0 (tools/corpus/vscorpus/segment.py)"

    y = _resample(y, sr, rate)
    y, gain_db, level = _normalise(y, rate, track, level_db)

    name = f"{range_name}_{job.group}_{job.item_id}"
    base = os.path.join(out_dir, name)
    import soundfile as sf

    sf.write(base + ".wav", y.astype(np.float32), rate, subtype="FLOAT")
    ds = DATASETS[job.dataset]
    comment = (f"{ds.title}\nsource: {os.path.basename(job.audio)}, licence {ds.licence}\n"
               f"reference: {job.reference_kind}")
    write_f0_csv(base + ".f0.csv", track, comment)
    write_notes_csv(base + ".csv", notes, comment + f"\nnotes: {notes_source}")

    voiced = track.f0 > 0
    return {
        "name": name,
        "dataset": job.dataset,
        "group": job.group,
        "id": job.item_id,
        "source": os.path.relpath(job.audio, os.path.dirname(os.path.dirname(job.audio))),
        "licence": ds.licence,
        "range": range_name,
        "rangeRule": "given" if job.range_name else "coverage",
        "rangeCoverage": round(coverage.get(range_name, 0.0), 4),
        "suggestedRange": chosen,
        "seconds": round(duration, 3),
        "voicedSeconds": round(float(voiced.sum()) * track.hop, 3),
        "unknownSeconds": round(float((track.f0 < 0).sum()) * track.hop, 3),
        "medianF0": round(float(np.median(track.f0[voiced])), 2) if voiced.any() else 0.0,
        "notes": len(notes),
        "reference": job.reference_kind,
        "notesSource": notes_source,
        "gainDb": round(gain_db, 2),
        "voicedLevelDb": round(level, 2),
        **job.extra.get("manifest", {}),
    }


# --- Jobs per dataset ------------------------------------------------------------------------------------


def jobs_vocadito(raw: str, notes: str) -> list[Job]:
    jobs = []
    audio_dir = os.path.join(raw, "Audio")
    for fname in sorted(os.listdir(audio_dir), key=lambda s: int(re.sub(r"\D", "", s) or 0)):
        m = re.match(r"vocadito_(\d+)\.wav$", fname)
        if not m:
            continue
        n = m.group(1)
        f0 = os.path.join(raw, "Annotations", "F0", f"vocadito_{n}_f0.csv")
        notes_file = os.path.join(raw, "Annotations", "Notes", f"vocadito_{n}_notes{notes.upper()}.csv")
        if not os.path.exists(f0):
            log(f"  vocadito {n}: no f0 annotation, skipped")
            continue
        jobs.append(Job("vocadito", "vocadito", f"{int(n):02d}", os.path.join(audio_dir, fname), None, "annotation",
                        {"f0": f0}, notes if notes in ("a1", "a2") and os.path.exists(notes_file) else "f0",
                        notes_file if os.path.exists(notes_file) else None))
    return jobs


def jobs_dcs(raw: str, manifest: dict) -> list[Job]:
    jobs = []
    for t in manifest["tracks"]:
        audio = os.path.join(raw, t["audio"])
        if not os.path.exists(audio):
            continue
        part = t["id"].split("_")[-1][0]
        item_id = t["id"].removeprefix("DCS_").replace("_", "-") + "-" + t["mic"]
        if "f0_manual_lrx" in t and os.path.exists(os.path.join(raw, t["f0_manual_lrx"])):
            kind, files = "manual", {"manual": os.path.join(raw, t["f0_manual_lrx"])}
        elif all(os.path.exists(os.path.join(raw, t.get(k, ""))) for k in ("f0_crepe_lrx", "f0_pyin_lrx")):
            kind, files = "crepe+pyin", {"crepe": os.path.join(raw, t["f0_crepe_lrx"]), "pyin": os.path.join(raw, t["f0_pyin_lrx"])}
        else:
            log(f"  dcs {t['id']}: no reference f0, skipped")
            continue
        jobs.append(Job("dcs", f"dcs-{t['category']}", item_id, audio, DCS_PARTS[part], kind, files, "f0", None,
                        {"manifest": {"mic": t["mic"], "part": part}}))
    return jobs


def jobs_vocalset(raw: str) -> list[Job]:
    jobs = []
    for singer in sorted(os.listdir(raw)):
        d = os.path.join(raw, singer)
        if not os.path.isdir(d):
            continue
        for fname in sorted(os.listdir(d)):
            if not fname.lower().endswith(".wav"):
                continue
            gender = singer[0].lower()
            item_id = os.path.splitext(fname)[0].replace("_", "-")
            jobs.append(Job("vocalset", "vocalset", item_id, os.path.join(d, fname), None, "pyin", {}, "f0", None,
                            {"pyin_limits": VOCALSET_PYIN_LIMITS[gender], "manifest": {"gender": gender}}))
    return jobs


def convert(raw_dir: str, out_dir: str, datasets: list[str], rate: int = 44100, level_db: float = DEFAULT_LEVEL_DB,
            vocadito_notes: str = "a1", jobs: int = 1, limit: int = 0, manifest_dir: str | None = None) -> list[dict]:
    os.makedirs(out_dir, exist_ok=True)
    all_jobs: list[Job] = []
    for name in datasets:
        raw = os.path.join(raw_dir, name)
        if not os.path.isdir(raw):
            log(f"[{name}] not downloaded ({raw}), skipped")
            continue
        if name == "vocadito":
            js = jobs_vocadito(raw, vocadito_notes)
        elif name == "dcs":
            js = jobs_dcs(raw, load_manifest("dcs", manifest_dir))
        elif name == "vocalset":
            js = jobs_vocalset(raw)
        else:
            raise ValueError(name)
        if limit:
            js = js[:limit]
        log(f"[{name}] {len(js)} files")
        all_jobs += js

    entries: list[dict] = []
    if jobs > 1 and len(all_jobs) > 1:
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(run_job, j, out_dir, rate, level_db) for j in all_jobs]
            for j, f in zip(all_jobs, futures):
                entries.append(f.result())
    else:
        entries = [run_job(j, out_dir, rate, level_db) for j in all_jobs]

    sources = {}
    for name in datasets:
        src = os.path.join(raw_dir, name, "SOURCE.json")
        if os.path.exists(src):
            with open(src, encoding="utf-8") as f:
                info = json.load(f)
            info.pop("files", None)
            sources[name] = info

    write_json(os.path.join(out_dir, "manifest.json"), {"sampleRate": rate, "levelDb": level_db,
                                                        "vocaditoNotes": vocadito_notes, "sources": sources,
                                                        "items": entries})
    with open(os.path.join(out_dir, "corpus.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(corpus_markdown(entries, sources, rate))
    log(f"converted {len(entries)} files into {out_dir}")
    return entries


def corpus_markdown(entries: list[dict], sources: dict, rate: int) -> str:
    md = f"## Corpus\n\n{len(entries)} files, {sum(e['seconds'] for e in entries) / 60:.1f} min, {rate} Hz.\n\n"
    md += "| Group | Range | Files | Min | Voiced min | Notes | Reference |\n|---|---|---:|---:|---:|---:|---|\n"
    groups: dict[tuple[str, str], list[dict]] = {}
    for e in entries:
        groups.setdefault((e["group"], e["range"]), []).append(e)
    for (group, rng), items in sorted(groups.items()):
        refs = ", ".join(sorted({e["reference"] for e in items}))
        md += (f"| {group} | {rng} | {len(items)} | {sum(e['seconds'] for e in items) / 60:.1f} | "
               f"{sum(e['voicedSeconds'] for e in items) / 60:.1f} | {sum(e['notes'] for e in items)} | {refs} |\n")
    md += "\nSources (internal evaluation only, not redistributed):\n\n"
    for name, info in sources.items():
        md += (f"- **{name}**: {info.get('title')} - Zenodo record {info.get('zenodo_record')}, {info.get('archive')} "
               f"(md5 {info.get('archive_md5')}{', pinned' if info.get('archive_md5_pinned') else ''}), "
               f"{info.get('licence')}, fetched via {info.get('method')}. {info.get('citation')}\n")
    return md
