# Auswertung mit echtem Gesang (öffentliche Datensätze)

**Kurz für dich:** Ja, öffentliche Aufnahmen von sauberem Sologesang lassen sich zum Abstimmen nutzen (Punkt 8
der Testanleitung). VocalSynth wird jetzt zusätzlich zum synthetischen Selbsttest mit drei frei lizenzierten
Datensätzen geprüft: **vocadito** (40 Sologesang-Ausschnitte, von Hand annotierte Tonhöhe und Noten),
**Dagstuhl ChoirSet** (Sopran, Alt, Tenor und Bass einzeln über Nahmikrofone, mit Tonhöhenkurve pro Sänger,
darunter Tonleitern) und **VocalSet** (20 professionelle Sänger, Tonleitern und Arpeggien). Alle drei stehen
unter CC BY 4.0. Die Dateien landen nicht im Repository, sie werden nur im GitHub-Runner geladen und dort
ausgewertet. Gestartet wird die Auswertung von Hand: GitHub → Actions → „Corpus evaluation“ → „Run workflow“
(etwa 15–30 Minuten, beim zweiten Lauf schneller, weil die Daten im Cache liegen). Das Ergebnis steht als
Tabelle direkt auf der Seite des Laufs: Trefferquote der Tonhöhe, Oktavfehler, erkannte Noten und
Einsatz-Verzögerung, jeweils für YIN und MPM und pro Stimmlage. Dazu kommt ein kleiner Parameter-Test
(Gate −45/−40/−35 dB, Stickiness 30/50/70 %). So sehen wir, ob die Standardwerte auch bei echten Stimmen gut
passen. Die Verzögerung in diesen Tabellen ist nur die Analyse im Plugin, ohne Audiopuffer und MIDI-Weg in FL
Studio.

The rest of this document is in English (developer documentation).

## Overview

```
Zenodo ──(HTTP range requests, checksums)──> raw/<dataset>/        tools/corpus/corpus.py download
raw/ ──(subset, mono, 44.1 kHz, level, reference f0, notes)──> eval/ tools/corpus/corpus.py convert
eval/ ──> vs-eval --algorithm both [--gate .. --stickiness ..] ──> JSON + Markdown
```

Everything runs in the manual GitHub Actions workflow `.github/workflows/corpus-eval.yml` (this container
cannot reach zenodo.org). The datasets are used for internal evaluation only: nothing derived from them
(audio, f0 or note files) is committed or uploaded as an artifact. The artifact holds only metrics.

## Datasets

| | vocadito | Dagstuhl ChoirSet (DCS) | VocalSet |
|---|---|---|---|
| Zenodo record (fixed version) | [5578807](https://zenodo.org/records/5578807) (v3) | [4618287](https://zenodo.org/records/4618287) (1.2.3) | [1442513](https://zenodo.org/records/1442513) (1.2) |
| Archive | `vocadito.zip`, ~59 MB | `DagstuhlChoirSet_V1.2.3.zip`, several GB (V1.2: 5.1 GB) | `VocalSet1-2.zip` (record: 2 archives, 2.1 + 6.0 GB, same recordings in different folder layouts) |
| Checksum | zip md5 `dea40fd18f14d899643c4ba221b33a46` (pinned) | zip md5 `82b95faa634d0c9fc05c81e0868f0217` + md5 of all 92 selected members (pinned) | zip md5 from the Zenodo API at download time; CRC32 of every member |
| Licence | CC BY 4.0 | CC BY 4.0 | CC BY 4.0 |
| Content | 40 solo excerpts (various singers, languages), 44.1 kHz | SATB choir, every singer on headset (HSM), dynamic (DYN) and larynx (LRX) microphones, 22.05 kHz mono | 20 professional singers (9 m, 11 f), 17 techniques, 44.1 kHz |
| Reference f0 | manual annotation, 256-sample hop (5.8 ms) | manual (quartet takes) or automatic: CREPE + pYIN on the LRX microphone (see below) | none in the dataset: librosa pYIN on the audio (see below) |
| Notes | two annotators (A1, A2); default A1 | none: segmented from the reference f0 | none: segmented from the reference f0 |
| Subset used | all 40 | 28 tracks (below), audio from HSM (DYN for sopranos) | `straight` scales and arpeggios, 1 take per singer and context, vowel `a` first: <= 40 files |
| Voice range | from the reference f0 (coverage rule) | from the part letter: S soprano, A alto, T tenor, B bass | from the reference f0 |

Checksum sources: vocadito zip md5 and member md5s of track 1: mirdata 1.0.0 (`REMOTES` and
`tests/indexes/vocadito_index_1.0_sample.json`). DCS: mirdata 0.3.8
`mirdata/datasets/indexes/dagstuhl_choirset_index_1.2.3.json` (md5 of that index file
`e55ac958f4d6a0bdaff1c7acbd7268db`); all 92 member md5s in `tools/corpus/manifests/dcs.json` were
cross-checked against it. The archive sizes come from the Zenodo record pages (not reachable from the
development container, so they are not pinned; the download does not need them).

DCS subset (`tools/corpus/manifests/dcs.json`): 12 solo takes (FullChoir Solo01/03/05/07, QuartetA
Solo01-04, QuartetB Solo01-04: one singer each of B, T, A, S), the 8 singers of the two quartet takes
`QuartetA_Take04` / `QuartetB_Take04` (manual f0 annotation), and the 8 HSM/DYN tracks of
`SE_FullChoir_ScaleUpDown01/02` (scales, one singer per part). The headset microphones of the quartet takes
carry some crosstalk from the other singers.

Citations (CC BY requires them in any publication of results):

- R. M. Bittner, K. Pasalo, J. J. Bosch, G. Meseguer-Brocal, D. Rubinstein: *vocadito: A dataset of solo
  vocals with f0, note, and lyric annotations.* arXiv:2110.05580, 2021.
- S. Rosenzweig, H. Cuesta, C. Weiß, F. Scherbaum, E. Gómez, M. Müller: *Dagstuhl ChoirSet: A Multitrack
  Dataset for MIR Research on Choral Singing.* TISMIR 3(1), 2020, doi:10.5334/tismir.48.
- J. Wilkins, P. Seetharaman, A. Wahl, B. Pardo: *VocalSet: A Singing Voice Dataset.* ISMIR 2018.

### Candidates not used

| Dataset | Why not |
|---|---|
| MDB-stem-synth | CC BY-NC-SA 4.0; exact f0, but resynthesised stems of mixed instruments, few solo vocals |
| MIR-1K | karaoke excerpts (voice in one channel, accompaniment in the other), 16 kHz, mostly amateur, no clear licence for reuse |
| PTDB-TUG | speech, not singing |
| VocalSet without a reference | used, but only with an automatic pYIN reference (its pitch metrics compare two detectors, see "Caveats") |

## Download (`corpus.py download`)

Per dataset, in this order:

1. `raw/<dataset>/SOURCE.json` exists and every file in it still has its md5: nothing to do (cache hit).
2. HTTP range requests (`vscorpus/remotezip.py`): the archive's central directory, then exactly the bytes of
   each selected member. A 6 GB archive costs a few hundred MB. Stored and deflated members, ZIP64.
3. If the server refuses ranges: the whole archive is streamed to `--work` (md5 checked), the members are
   read from it and the archive is deleted again (one dataset at a time, so the runner needs at most the
   largest archive + the subset on disk).

Every extracted member is checked against its CRC32 and, where the manifest has one, its md5. Retries with
backoff on 429/5xx/network errors. `--source NAME=path-or-url` replaces Zenodo (local copies, the tests).
A failing dataset does not stop the others; `--strict` makes the exit code 1 in that case.

## Conversion (`corpus.py convert`)

Output per file, in `eval/` (names as vs-eval expects them, `<range>_<group>_<id>`):

| File | Content |
|---|---|
| `<range>_<group>_<id>.wav` | mono float32, 44.1 kHz (`--rate`), soxr VHQ resampling; scaled so the RMS of the reference-voiced parts is -20 dBFS (`--level-db`), peak <= -1 dBFS |
| `<range>_<group>_<id>.f0.csv` | reference f0: `time_s,f0_hz` on a 5 ms grid; > 0 voiced, 0 unvoiced, -1 unknown (not scored) |
| `<range>_<group>_<id>.csv` | note labels `onset_s,offset_s,midi_note` |
| `manifest.json`, `corpus.md` | per-file metadata (source, licence, range rule, gain, median f0, reference kind) and a summary table |

`<range>` is the voice-range prefix vs-eval reads (`bass`, `baritone`, `tenor`, `alto`, `soprano`, `wide`;
`tools/vs-eval/main.cpp`, `rangeForFile`), `<group>` is `vocadito`, `dcs-solo`, `dcs-quartet`, `dcs-scale` or
`vocalset` (vs-eval's `byDataset`).

The level matters for the gate: the plugin's gate compares the RMS of the conditioned input with the
threshold, so a -45/-40/-35 dB gate at a -20 dBFS singing level corresponds to 25/20/15 dB of headroom.

### Reference f0 (`vscorpus/reference.py`)

All references are resampled to a uniform 5 ms grid (linear in log frequency between voiced samples at most
1.5 source hops apart, otherwise the nearest sample within 0.75 hop, else unknown).

- **vocadito:** the manual annotation (0 = unvoiced).
- **DCS, manual:** the manual annotation lists only voiced frames: gaps inside the annotated span are unvoiced,
  everything outside it is unknown.
- **DCS, automatic** (solo and scale takes): CREPE and pYIN, both computed by the dataset authors on the
  larynx microphone (no crosstalk). On CREPE's 10 ms grid: *voiced* if pYIN is voiced, CREPE confidence
  >= 0.5 and both agree within 50 cents (CREPE's f0 is used); *unvoiced* if pYIN is unvoiced and CREPE
  confidence < 0.3; *unknown* otherwise. Voiced islands shorter than 30 ms become unknown.
- **VocalSet:** `librosa.pyin` on the original audio (frame 2048, hop 256 at 44.1 kHz; 60-700 Hz for male,
  120-1300 Hz for female singers): voiced if pYIN's flag is set and its voiced probability >= 0.5, unvoiced
  if not set and < 0.2, else unknown; islands < 30 ms unknown.

### Note labels (`vscorpus/segment.py`)

vocadito uses the annotators' notes (`--vocadito-notes a1|a2|f0`; the vocadito paper reports low agreement
between the two annotators, so note scores on vocadito have a lower ceiling). Elsewhere the notes come from
an offline, non-causal segmentation of the reference f0:

1. voiced runs of the reference (gaps <= 30 ms bridged);
2. fractional MIDI pitch, running median over 50 ms;
3. Schmitt quantiser: the note changes only when the pitch is more than 0.5 + 0.25 semitones away from the
   current note (vibrato around a slightly sharp note does not flip it);
4. quantised stretches shorter than 80 ms are transitions (scoops, glides, vibrato peaks); stable stretches
   of the same note separated only by transitions merge;
5. between two different stable notes, the change is placed where the smoothed pitch first gets closer to
   the new note than to the old one (legato: previous offset = next onset);
6. notes shorter than 60 ms merge into a neighbour; a run without a stable stretch becomes one note (rounded
   median) if it lasts >= 60 ms.

This deliberately has no stickiness delay and looks ahead, so vs-eval shows how late the causal plugin is.

### Voice range (`segment.choose_range`)

DCS takes the part letter. Otherwise: the share of voiced reference frames inside each singing range
(`bass`..`soprano`, limits as in `core/include/vocalsynth/VoiceSettings.h`; a test checks they match); among
the ranges within 1 % of the best coverage, the one whose centre (geometric mean of its limits) is closest to
the median f0. Below 97 % coverage everywhere: `wide`.

## vs-eval with a reference f0

`vs-eval` reads `<file>.f0.csv` next to a WAV (or `--f0-ref`) and adds, per file, per algorithm and pooled
(`summary`, `byRange`, `byDataset`):

| Key | Metric |
|---|---|
| `f0.rawPitchAccuracy` | reference-voiced frames whose estimate is within +-50 cents |
| `f0.rawChromaAccuracy` | the same, octave errors forgiven |
| `f0.octaveErrorRate` | chroma - pitch accuracy (estimate whole octaves off) |
| `f0.voicingRecall` / `f0.voicingFalseAlarm` | reference-voiced frames called voiced / reference-unvoiced frames called voiced |
| `f0.medianAbsCents` | median error of the correct frames |
| `processedF0` | the same after the median + octave guard |
| `notes` | precision / recall / F1 (same MIDI note, onset within +-50 ms, one-to-one), split into `fromRest` and legato `change`; onset latency mean / median / p95 of the matched notes; `restLatency`: first note-on (any pitch) -50..+200 ms around each reference note after a rest; `changeDelay` |

Frames are timed at the analysis window centre; unknown reference frames and frames within the collar (half
an analysis window + 1 hop) of a reference voicing change are not scored.

Options for sweeps: `--algorithm yin|mpm|both`, `--gate dB[,..]`, `--stickiness %[,..]`, `--min-note ms[,..]`
(comma lists span a grid), `--jobs n`, `--json file`, `--markdown file|-`, `--summary-only`, `--notes` (the
detected notes per file). Diagnostics for one file (v0.3.1, no sweep): `--frames <prefix>` writes every analysis
frame to `<prefix><algorithm>.csv` (time, raw and processed f0 / confidence / RMS, active note, voiced) and the
notes to `<prefix><algorithm>.notes.csv`; this is how the split notes of docs/M2-DESIGN.md 12 were traced. The per-file
results, `byRange` and `byDataset` are for the base point (the defaults where the lists contain them);
`sweep` has the pooled summary of every grid point. `--self-test` and `--unit-test` are unchanged (ctest
`vs-eval.self-test`, `vs-eval.unit-test`).

## Workflow (`.github/workflows/corpus-eval.yml`)

Manual only (`workflow_dispatch`), `ubuntu-24.04`, 60 min timeout. Inputs: `datasets` (default all three),
`sweep` (default on), `gates` (`-45,-40,-35`), `stickiness` (`30,50,70`), `min_note` (`60`), `limit`
(files per dataset, 0 = all).

Steps: checkout without submodules → Python 3.12 + `tools/corpus/requirements.txt` → build vs-eval with the
JUCE-free standalone project `tools/vs-eval/standalone` (clang, Ninja, Release) and run its two ctests →
pytest of the corpus tooling (`tests/tools/corpus`, including an end-to-end run through vs-eval on fake
datasets) → restore `raw/` from `actions/cache` (key: cache version, dataset list, hash of the manifests and
the selection code; a prefix match restores the newest entry) → free disk space, only if a requested dataset
has no `raw/<dataset>/SOURCE.json` yet → download (always runs, `continue-on-error`: datasets cached with
their md5 are skipped, the others are fetched; a failed dataset does not stop the run) → save the cache
whenever the download changed any `SOURCE.json`, also after a partial failure (`if: always()`), so the next
run fetches only what is still missing → convert → vs-eval with the defaults (both algorithms) → sweep (2 algorithms x 9 grid points) →
Markdown summary to the job log and `$GITHUB_STEP_SUMMARY` → artifact `corpus-eval-<run>` with
`eval.json`, `sweep.json`, the Markdown files and the corpus manifest (no audio).

Budget: the subset is a few hundred MB (whole-archive fallback: one archive of up to ~6 GB at a time, after
removing preinstalled SDKs); vs-eval runs ~50x real time per core, so the ~1 h of audio x 18 sweep runs
take a few minutes on 4 cores; pYIN for VocalSet a few minutes.

To start it: Actions → "Corpus evaluation" → Run workflow, or `gh workflow run corpus-eval.yml --ref <branch>`
and `gh run watch`. GitHub offers a `workflow_dispatch` workflow only once a file of that name exists on the
default branch (`master`); after that, any branch can be chosen as `--ref` and runs its own version of the
file.

## Testing (local, no network)

```bash
python -m pip install -r tools/corpus/requirements.txt
python -I -m pytest tests/tools/corpus -q                       # 30 tests, ~20 s
VS_EVAL=build/tools/vs-eval/vs-eval python -I -m pytest tests/tools/corpus -q   # + end to end through vs-eval
python -I tools/corpus/corpus.py selfcheck --vs-eval build/tools/vs-eval/vs-eval  # without pytest
```

`corpus.py fake --out DIR` writes fake versions of the three archives (same layout and annotation formats,
synthetic voice-like audio with a known f0) plus manifests with their md5s, so `download --source
NAME=DIR/NAME.zip --manifest-dir DIR/manifests` runs the real code path. The tests also serve the archives
through a local HTTP server with and without range support.

## Caveats

- The DCS solo/scale and VocalSet references are automatic. Where both CREPE and pYIN agree the reference is
  very likely right, but frames they disagree on (often the hard ones: onsets, breathy endings) are not
  scored, so accuracies on these sets are upper bounds. vocadito and the DCS quartet takes have manual f0.
- DCS audio is 22.05 kHz (upsampled to 44.1 kHz): no content above 11 kHz.
- Latency numbers are the plugin's analysis latency relative to the reference onset, offline; the host's
  audio buffer (e.g. 2048 samples = 46 ms at 44.1 kHz) and MIDI routing come on top.
- Note F1 depends on the labels: vocadito's annotators disagree, and segmented labels follow the rules
  above. Use it to compare settings and algorithms, not as an absolute score.
