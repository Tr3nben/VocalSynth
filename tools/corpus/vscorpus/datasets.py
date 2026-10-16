"""Dataset definitions and subset selection (docs/CORPUS.md, "Datasets" and "Subset").

Every dataset is one zip archive on Zenodo. select_members() maps the archive's member names to the files
we need and to their path below raw/<dataset>/ (independent of the archive's top-level folder).
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field

MANIFEST_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "manifests")


@dataclass
class Dataset:
    name: str
    title: str
    record: int                 # Zenodo record id (a fixed version)
    licence: str
    citation: str
    zip_name: str | None        # None: resolved via the Zenodo API (largest .zip whose name matches zip_pattern)
    zip_pattern: str = r".*\.zip$"
    full_download: bool = False  # small archive: always fetch it whole
    manifest: dict = field(default_factory=dict)


DATASETS = {
    "vocadito": Dataset(
        name="vocadito",
        title="vocadito: 40 excerpts of solo singing, f0 + 2 note annotations",
        record=5578807,
        licence="CC BY 4.0",
        citation="R. M. Bittner, K. Pasalo, J. J. Bosch, G. Meseguer-Brocal, D. Rubinstein: vocadito: A dataset of "
        "solo vocals with f0, note, and lyric annotations. arXiv:2110.05580, 2021",
        zip_name="vocadito.zip",
        full_download=True,
    ),
    "dcs": Dataset(
        name="dcs",
        title="Dagstuhl ChoirSet 1.2.3: SATB singers, close-up microphones, f0 per singer",
        record=4618287,
        licence="CC BY 4.0",
        citation="S. Rosenzweig, H. Cuesta, C. Weiss, F. Scherbaum, E. Gomez, M. Mueller: Dagstuhl ChoirSet: A "
        "Multitrack Dataset for MIR Research on Choral Singing. TISMIR 3(1), 2020, doi:10.5334/tismir.48",
        zip_name="DagstuhlChoirSet_V1.2.3.zip",
    ),
    "vocalset": Dataset(
        name="vocalset",
        title="VocalSet 1.2: 20 professional singers, scales / arpeggios / long tones (no f0: pYIN reference)",
        record=1442513,
        licence="CC BY 4.0",
        citation="J. Wilkins, P. Seetharaman, A. Wahl, B. Pardo: VocalSet: A Singing Voice Dataset. ISMIR 2018",
        zip_name=None,
        zip_pattern=r"(?i).*vocalset.*\.zip$",
    ),
}


def load_manifest(name: str, manifest_dir: str | None = None) -> dict:
    path = os.path.join(manifest_dir or MANIFEST_DIR, f"{name}.json")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# --- Subset selection ------------------------------------------------------------------------------------

_VOCADITO = re.compile(
    r"(?:^|/)((?:Audio/vocadito_\d+\.wav)|(?:Annotations/(?:F0|Notes)/vocadito_\d+_(?:f0|notesA1|notesA2)\.csv)"
    r"|(?:vocadito_metadata\.csv))$"
)

_VOCALSET = re.compile(r"^(?P<singer>[mf]\d{1,2})_(?P<context>scales|arpeggios)_(?P<rest>.+)\.wav$", re.IGNORECASE)
VOCALSET_VOWELS = ("a", "e", "i", "o", "u")


def _skip(name: str) -> bool:
    return "__MACOSX" in name or os.path.basename(name).startswith("._")


def select_members(dataset: str, names: list[str], manifest: dict, vocalset_per_context: int = 1) -> dict[str, str]:
    """Returns {archive member name: path below raw/<dataset>/}."""
    names = [n for n in names if not _skip(n)]

    if dataset == "vocadito":
        out = {}
        for n in names:
            m = _VOCADITO.search(n)
            if m:
                out[n] = m.group(1)
        return out

    if dataset == "dcs":
        out = {}
        wanted = set(manifest.get("members", {}))
        for n in names:
            for w in wanted:
                if n == w or n.endswith("/" + w):
                    out[n] = w
        return out

    if dataset == "vocalset":
        # One "straight" (plain) take per singer and context (scales, arpeggios), vowel a first
        candidates: dict[tuple[str, str], list[tuple[int, str, str]]] = {}
        seen_basenames = set()
        for n in sorted(names):
            base = os.path.basename(n)
            m = _VOCALSET.match(base)
            if not m or base.lower() in seen_basenames:
                continue
            tokens = m.group("rest").lower().split("_")
            if "straight" not in tokens:
                continue
            seen_basenames.add(base.lower())
            vowel = tokens[-1]
            rank = VOCALSET_VOWELS.index(vowel) if vowel in VOCALSET_VOWELS else len(VOCALSET_VOWELS)
            key = (m.group("singer").lower(), m.group("context").lower())
            candidates.setdefault(key, []).append((rank, base, n))
        out = {}
        for (singer, _), items in sorted(candidates.items()):
            for _, base, n in sorted(items)[:vocalset_per_context]:
                out[n] = f"{singer}/{base}"
        return out

    raise ValueError(f"unknown dataset {dataset}")
