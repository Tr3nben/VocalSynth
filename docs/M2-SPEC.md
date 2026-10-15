# M2 Spec – Pitch → MIDI (binding input for the M2 workflow)

Status: approved by the user on 2026-10-15. Branch: `m2-pitch-to-midi`. Target version: **0.2.0**.
Refines docs/PLAN.md section 3 (M2). The design agent turns this into `docs/M2-DESIGN.md` and a compilable scaffold.

## Goal

The user sings or hums into the mic on the mixer insert where **VocalSynth FX** sits; the plugin sends clean
MIDI notes (+ optional pitch bend) through the FL MIDI output port to a synth (verified path from M1/T1).
No "spaghetti MIDI": stable notes, no notes from silence/breath, correct octave.

## User requirements (from the conversation)

- **Voice-range dropdown** like the "Input Type" selector of modern Auto-Tune plugins. The user is a tenor, but the
  range must be selectable. Choices and f0 limits (initial values, tune with tests):

  | Choice | f0 min | f0 max | Note |
  |---|---|---|---|
  | Bass | 65 Hz (C2) | 330 Hz (E4) | |
  | Baritone | 82 Hz (E2) | 392 Hz (G4) | |
  | **Tenor** (default) | 110 Hz (A2) | 523 Hz (C5) | |
  | Alto | 147 Hz (D3) | 698 Hz (F5) | |
  | Soprano | 196 Hz (G3) | 1047 Hz (C6) | |
  | Whistle | 500 Hz | 2500 Hz | for whistling |
  | Wide | 65 Hz | 1047 Hz | all voices, more latency, more octave risk |

  The analysis window follows f0 min (at least 2 periods), so lower ranges have more latency. Show the resulting
  latency in ms in the UI.
- The M1 test mode stays available (it is how routing is checked).
- Same Windows install package as before; FL Studio must keep sorting FX as effect and Instrument as generator.

## Parameters (all `juce::ParameterID {id, 1}`; IDs permanent; existing M1 parameters stay unchanged)

| ID | Type | Values / range | Default |
|---|---|---|---|
| `source` | choice | "Voice", "Test Pattern" | Voice |
| `voiceRange` | choice | Bass, Baritone, Tenor, Alto, Soprano, Whistle, Wide | Tenor |
| `pitchAlgorithm` | choice | "YIN", "MPM" | decided by the evaluation (start with YIN) |
| `key` | choice | C, C#, D, D#, E, F, F#, G, G#, A, A#, B | C |
| `scale` | choice | Chromatic, Major, Natural Minor, Harmonic Minor, Major Pentatonic, Minor Pentatonic, Blues, Dorian, Mixolydian | Chromatic |
| `stickiness` | float % | 0–100 | 50 |
| `gateThreshold` | float dB | -70 … -10 | -45 |
| `minNoteLength` | float ms | 20–250 | 60 |
| `octaveShift` | int | -3 … +3 | 0 |
| `bendMode` | choice | "Off", "Anchored", "True" | Off |
| `bendRange` | choice | "±2", "±12", "±24", "±48" semitones (must match the synth) | ±2 |
| `velocitySensitivity` | float % | 0 (fixed 100) – 100 | 70 |
| `legato` | bool | false = retrigger on every note change, true = legato (note-on before note-off) | false |

`testPattern` keeps its meaning but is only used when `source = Test Pattern`. `midiChannel`, `monitorTone`,
`virtualMidiOut`, `muteDry` apply to both sources (monitor tone follows the sung notes in Voice mode).

## Core modules (JUCE-free, `core/`, real-time safe, block-size invariant)

1. **InputConditioner**: mono downmix is done by the plugin; DC blocker + high-pass (~60 Hz, 2nd order).
2. **Decimator**: integer factor D = floor(sampleRate / 16000) (44.1k→2, 48k→3, 96k→6) with an anti-alias FIR;
   analysis runs at sampleRate / D. Must be exactly block-size invariant.
3. **Framer**: ring buffer at the analysis rate, fixed hop (≈2–3 ms), window length from the voice range
   (≥ 2 periods of f0 min), independent of host block size.
4. **PitchDetector** interface → `Yin` and `Mpm` (NSDF) implementations. Output per frame:
   `{ frequencyHz (0 = unvoiced), confidence 0..1, rmsDb }`. Parabolic interpolation, search limited to the
   voice range. CPU: direct difference function at the decimated rate is acceptable; use an FFT only if needed.
5. **PitchPostProcessor**: median of 3–5 frames, octave-jump guard (reject sudden ×2/×0.5 jumps unless sustained).
6. **ScaleQuantizer**: key + scale → nearest allowed MIDI note (ties go to the current note).
7. **NoteSegmenter**: voicing gate with hysteresis (rmsDb vs gateThreshold, confidence), stickiness
   (switch note only when pitch is ≥ 0.5 + h semitones away for ≥ T_min; h and T_min from `stickiness`),
   minimum note length, octave shift, velocity from onset RMS (scaled by `velocitySensitivity`), legato/retrigger.
   Emits `VsEvent` note-on/off with sample offsets mapped from the analysis hop back to the host block.
8. **BendGenerator**: residual (sung pitch − note) in cents → 14-bit bend for the selected bend range.
   Anchored = dead zone (≈ ±30 cents) + smoothing; True = 1:1. Rate-limited (≈200 Hz), reset to centre on note-off.
9. **VoicePipeline**: composes 1–8. `prepare(sampleRate, maxBlock)`, `reset()`,
   `process(const float* mono, int numSamples, const VoiceSettings&, VsEventBuffer<N>&)`, plus a lock-free
   snapshot for the UI (current f0, cents offset, note, confidence, voiced flag) and
   `getAlgorithmicLatencyMs()`. Settings changes that alter range/key/scale/algorithm flush the active note.

## Plugin integration

- `source = Voice`: mono-downmix the input → VoicePipeline → existing `MidiOutputStage` (stuck-note guard) →
  host MIDI + optional virtual port; `MonitorSynth` plays the same events when Monitor Tone is on.
- `source = Test Pattern`: exactly the M1 behaviour.
- Switching source, bypass, reset/prepareToPlay/releaseResources → flush (existing mechanism).
- Report latency 0 to the host (analysis delay is not compensated in M2).
- UI (FX and Instrument): main panel = tuner (note name in FL convention C5 = 60, cents needle, Hz,
  confidence/voiced indicator), detected note, latency ms, dropdowns (Source, Voice Range, Algorithm, Key, Scale,
  Bend Mode, Bend Range), sliders (Stickiness, Gate, Min Note, Velocity Sens., Octave), Legato toggle, input meter
  with gate line. M1 test/diagnostic controls move into a compact "Test & Routing" section (still reachable).
  Version text "0.2.0 – M2 pitch to MIDI".

## Evaluation and tests

- Unit tests (Catch2) with synthetic signals: sines and saws 65–1047 Hz per range preset (±10 cents),
  vibrato (±50 cents, 5.5 Hz) → one note (+ bend in True mode), glissando, octave jumps, missing fundamental /
  strong 2nd harmonic (no octave error), white/pink noise and silence → 0 notes, breath-like noise bursts → 0 notes,
  scale quantization, legato vs retrigger, MIDI invariants (pairing, no duplicate note-on, offsets in range),
  block sizes {1, 7, 64, 480, 512, 4096} × 44.1/48/96 kHz identical events, latency per range preset
  (Tenor ≤ 30 ms, Bass ≤ 40 ms), CPU benchmark (worst-case < 20 % of a 128-sample block at 48 kHz in Release).
- `tools/vs-eval`: CLI that runs the pipeline on WAV files (+ optional CSV labels: onset, offset, midi note) and
  prints JSON metrics (raw pitch accuracy ±50 cents, octave error rate, note F1 ±50 ms, YIN vs MPM).
  A synthetic test corpus is generated by the tests; real recordings from the user may be added later under
  `tests/corpus/` (not available yet).
- Existing gates stay: pluginval strictness 10, Steinberg validator, clap-validator, auval, CI on 3 platforms.

## Docs

- `docs/M2-DESIGN.md` (English), `docs/M2-TESTING.md` (German: how to test singing → Vital in FL, what each
  control does, what to report back), LIESMICH.txt update (CRLF), README status, PLAN.md M2 checkboxes,
  CI package must include `docs/M2-TESTING.md`.
