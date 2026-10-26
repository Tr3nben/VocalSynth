# M2 design: pitch -> MIDI (v0.2.0; v0.2.1 changes in section 11)

Status: design + compilable scaffold, revised after the design review (section 9 lists what changed and
why). All public interfaces in `core/include/vocalsynth/` are final for M2. Bodies are `TODO(A|B|C)` stubs where marked; the pure formulas (tables, latency, stickiness, velocity,
bend value, decimation factor, hop/window sizes) and the `InputConditioner` are already implemented,
because they define the design. Binding input: `docs/M2-SPEC.md`. Rules: `.claude/skills/juce-audio-plugin-dev/SKILL.md`
and `docs/M1-DESIGN.md` (the M1 parts stay valid unless changed here).

Notation: `sr` = host sample rate, `D` = decimation factor, `fa = sr / D` = analysis rate, `hop` and
`W` (window) in analysis samples, "frame" = one analysis of one window, produced once per hop.

## 1. Goal and scope

The user sings into the mic on the mixer insert where **VocalSynth FX** sits. In `source = Voice` the
plugin turns the voice into MIDI notes (+ optional pitch bend) on the selected MIDI channel and sends them
through the existing `MidiOutputStage` (stuck-note guard, sent counters, virtual-port mirror) and the FL
MIDI output port. `source = Test Pattern` is exactly the M1 behaviour.

Requirements that drive the design:

- clean notes: no notes from silence, noise or breath; stable notes under vibrato; correct octave
- note-on latency (onset -> note-on, algorithmic): Tenor <= 30 ms, Bass <= 40 ms (section 3.11)
- bit-exact block-size invariance: identical events at identical absolute sample positions for host blocks
  {1, 7, 64, 480, 512, 4096} at 44.1 / 48 / 96 kHz
- real-time safety: nothing allocates, locks, logs, throws or does I/O on the audio thread; every buffer is
  sized in `prepare()` for the worst case
- `core/` stays JUCE-free; warnings clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` and MSVC `/W4`

Out of scope for M2 (PLAN.md ideas, not in the spec): "scale snap only at onset", provisional note-on at the
onset with bend correction (RESEARCH 5.4), audio latency compensation (latency 0 is reported to the host).

## 2. Data flow

### 2.1 Processor (audio thread, `source = Voice`)

```
processBlock(buffer, midi)                                    (M1 order, docs/M1-DESIGN.md 2, unchanged parts elided)
 ├─ midi.clear() ; histogram ; n <= 0 -> return
 ├─ voiceSettings = getVoiceSettings()          VoiceSettings value built from the raw atomics (any thread)
 ├─ source changed since last block -> flushPending
 ├─ if flushPending.exchange(false): midiOutput.flushAllNotes(0, midi); generator.reset();
 │                                   voicePipeline.reset(); monitorSynth.releaseAll()
 ├─ input metering (before the audio is touched)
 ├─ for each chunk [start, start + len), len <= maxChunkSamples (4096):
 │      mono[0..len) = average of the input channels (FX: main bus, Instrument: sidechain bus)
 │                     (read BEFORE muteDry / Instrument clear the buffer)
 │      events.clear()
 │      voicePipeline.process(mono, len, voiceSettings, events)        VsEventBuffer<1024>
 │      midiOutput.write(events, len, midi, start)  ── mirror ──► VirtualMidiPort
 │      monitorSynth.process(events, channel pointers + start, ...)    (adds; every block)
 └─ audio: FX dry (or cleared if muteDry) + tone; Instrument: tone only
```

`mono` is a `std::vector<float>` of `maxChunkSamples` allocated in the constructor (not per block).

### 2.2 VoicePipeline (per input sample, block-size independent)

```
x[n] (host rate, mono)
 └─ InputConditioner      DC blocker + 2nd-order HPF 60 Hz                       host rate
     └─ Decimator         Kaiser FIR, emits y[k] when n % D == 0               -> fa = sr / D
         └─ Framer        ring at fa; every hop-th sample: frame = newest W samples
             └─ PitchDetector (YIN | MPM)  -> PitchFrame {frequencyHz, confidence, rmsDb}
                 │                         (lags tauMin..tauMax, edge rule, parabolic interpolation)
                 └─ PitchPostProcessor     median 3 (pitch, confidence) + octave guard
                     └─ NoteSegmenter      gate/hysteresis, onset confirm, ScaleQuantizer, stickiness,
                         │                 minNoteLength, octave shift, velocity, legato/retrigger
                         └─ BendGenerator  residual cents -> 14-bit bend, dead zone, 200 Hz limit
                              └─ VsEvents at the sample offset of the input sample that completed the hop
```

Everything runs per input sample inside `processToSink()`; the expensive part (detector) runs once per
hop. There are no internal scratch buffers that depend on the host block size, so `process()` accepts any
`numSamples` (the `maxBlockSize` passed to `prepare()` is only a hint).

## 3. Core modules (`namespace vocalsynth`, `core/include/vocalsynth/`, `core/src/`)

Common rules: `noexcept` on everything the audio thread calls; `prepare()` may allocate (not `noexcept`
where it allocates: `Decimator`, `Framer`, detectors, `VoicePipeline`); `reset()` never allocates; no
`<random>` distributions or other platform-dependent maths in decisions; double precision for filter state
and phase/time maths.

### 3.1 `VoiceSettings.h` (implemented)

- Enums whose values equal the plugin choice indices: `VoiceRange {bass, baritone, tenor, alto, soprano,
  whistle, wide}`, `PitchAlgorithm {yin, mpm}`, `Scale {chromatic, major, naturalMinor, harmonicMinor,
  majorPentatonic, minorPentatonic, blues, dorian, mixolydian}`, `BendMode {off, anchored, trueBend}`,
  `BendRange {semitones2, semitones12, semitones24, semitones48}` + `bendRangeSemitones()`.
- `voiceRangeTable` (spec values, tune with tests):

  | Range | f0 min | f0 max |
  |---|---|---|
  | Bass | 65 | 330 |
  | Baritone | 82 | 392 |
  | Tenor (default) | 110 | 523 |
  | Alto | 147 | 698 |
  | Soprano | 196 | 1047 |
  | Whistle | 500 | 2500 |
  | Wide | 65 | 1047 |

  `lowestF0MinHz = 65` sizes every buffer.
- `struct VoiceSettings` (value type, `operator==`): range, algorithm, key 0..11, scale, stickinessPercent,
  gateThresholdDb, minNoteLengthMs, octaveShift, bendMode, bendRange, velocitySensitivityPercent, legato,
  channel 1..16. Defaults = parameter defaults.
- `sanitise()` clamps every field (enums too, NaN -> default). `requiresFlush (a, b)`: range, algorithm,
  key, scale or channel differ.

### 3.2 `InputConditioner` (implemented in the scaffold, owner A for tests/tuning)

Host rate. DC blocker `y = x - x1 + R y1`, `R = exp(-2 pi 5 Hz / sr)`, then RBJ Butterworth high-pass
60 Hz, Q = 1/sqrt 2, transposed direct form II, double state. Non-finite input is treated as 0. Filter
memories below `1e-30` are flushed to 0 (tests and vs-eval run without FTZ/DAZ; per sample, so invariant).
At 65 Hz (Bass f0 min) the HPF attenuates the fundamental by about 2.4 dB; the detectors work on
periodicity, so this is accepted (tests check Bass sines at 65 Hz).

**Group delay** near the cutoff is not negligible and is part of the latency budget (3.11):
`InputConditioner::groupDelaySeconds (sr, f)` (exact digital transfer function, implemented) gives
3.62 ms at 65 Hz, 2.51 ms at 82 Hz, 1.40 ms at 110 Hz, 0.75 ms at 147 Hz, 0.06 ms at 500 Hz (DC blocker
included, any rate >= 22 kHz within 0.01 ms). A lower Q (0.5) would cut the 65 Hz value to about 2.5 ms
if the Bass margin is ever needed; the cutoff itself barely helps (the delay scales with fc / f^2).

### 3.3 `Decimator` (owner A)

- `D = max(1, floor(sr / 16000))`: 44.1k -> 2 (fa 22.05k), 48k -> 3 (16k), 88.2k -> 5 (17.64k), 96k -> 6
  (16k), 192k -> 12 (16k); rates below 32 kHz -> D = 1 (pass-through, no filter).
- FIR: linear-phase Kaiser-windowed sinc, cutoff fa/2, passband edge 0.35 fa, stopband edge 0.65 fa,
  60 dB (beta = 0.1102 (60 - 8.7) = 5.653), taps normalised to sum 1. Aliases from the transition band only
  land in (0.35 fa, 0.5 fa), above the pitch-relevant band (Whistle f0 max 2.5 kHz << 0.35 * 16 kHz = 5.6 kHz).
  Taps `numTapsFor(D)` = `ceil((A - 7.95) / (2.285 dw)) + 1`, odd, `dw = 2 pi 0.3 / D`:
  D = 2 -> 27, 3 -> 39, 5 -> 63, 6 -> 75, 12 -> 147. Group delay (N-1)/2 input samples ~= 0.3-0.4 ms.
- Timing (binding): input samples counted n = 0, 1, ... since `reset()`; output `y[k]` is produced by the
  push of input `n = k D`: `y[k] = sum_j h[j] x[kD - j]`. History is a doubled ring (each sample written at
  `pos` and `pos + N`) so the dot product is one contiguous loop; the FIR is evaluated only on output samples.
- Cost at 192 kHz: 147 MAC x 16k/s = 2.4 MMAC/s.

### 3.4 `Framer` (owner A)

- Hop `max(1, lround(fa * 0.002))`: 16k -> 32, 22.05k -> 44, 17.64k -> 35 (2 ms; spec: 2-3 ms). In input
  samples: 96 at 48 kHz, 88 at 44.1 kHz, 175 at 88.2 kHz, 192 at 96 kHz. Independent of the range. The
  short end of the spec range is chosen for the latency budget (3.11): each hop of the budget costs 2 ms
  instead of 2.5 ms; CPU stays far below the budget (3.5.3).
- `maxLag = ceil(fa / f0min) + 1`, window `W = 2 * maxLag` (>= 2 periods of f0 min, spec). The detectors
  evaluate their lag function up to `maxLag` and integrate over `W - maxLag = maxLag` samples. The `+ 1`
  guard lag makes the period of f0 min itself an interior lag (its neighbour exists), which the edge rule
  (3.5) needs to tell "f0 = f0 min" from "f0 below the range".

  | Range | W @16 kHz | ms | W @22.05 kHz | ms |
  |---|---|---|---|---|
  | Bass, Wide | 496 | 31.00 | 682 | 30.93 |
  | Baritone | 394 | 24.62 | 540 | 24.49 |
  | Tenor | 294 | 18.38 | 404 | 18.32 |
  | Alto | 220 | 13.75 | 302 | 13.70 |
  | Soprano | 166 | 10.38 | 228 | 10.34 |
  | Whistle | 66 | 4.12 | 92 | 4.17 |

- Ring: doubled (`2 * maxWindow`), zero-initialised, so `getWindow()` is always one contiguous span of the
  newest W samples, oldest first. A hop completes on push number `m * hop` (m = 1, 2, ...); early frames
  contain leading zeros, no frame is skipped.
- `setWindowSamples()` on a range change keeps ring contents and hop phase (the frame clock never moves).

### 3.5 `PitchDetector` interface, `YinPitchDetector`, `MpmPitchDetector` (owner A)

`PitchFrame {frequencyHz (0 = unvoiced), confidence 0..1, rmsDb}`. Interface: `prepare(fa, maxWindow)`
(allocates), `analyse(span window, f0min, f0max) noexcept` (stateless between frames). Lags:
`maxLag = W/2` (lag function evaluated for tau <= maxLag), candidates `tauMin = max(2, floor(fa / f0max))`
.. `tauMax = min(ceil(fa / f0min), maxLag - 1)`.

**Edge rule (binding).** The chosen YIN minimum / MPM peak must be a local extremum including its outer
neighbour (`tauMin - 1`, `tauMax + 1`, both always computed). A dip still falling at `tauMax` or a peak
still rising there means f0 < f0min; one that is better at `tauMin - 1` means f0 > f0max: the frame is
unvoiced. Without this rule a 60 Hz tone in Bass reads as a confident 65 Hz (YIN `d'` at the last lag is
already below the threshold) and a 1.1 kHz whistle in Soprano reads as its octave below. Because both
neighbours exist, parabolic interpolation is always possible; the result is reported as measured
within `rangeToleranceSemitones` (0.5) of [f0min, f0max], NOT clamped to the limits (see 10, item 2). `rmsDb` = RMS of the
newest `ceil(fa / f0min)` samples (one period of f0 min: reacts faster than the whole window, so onsets and
releases pass the gate sooner), floor -120 dB. Below `silenceFloorDb = -90 dB` the frame is unvoiced
without searching.

#### 3.5.1 YIN

Integration length `Wi = W - maxLag = maxLag`; `d(tau) = sum_{j<Wi} (x[j] - x[j+tau])^2`, tau = 1..maxLag;
cumulative mean normalised `d'` (`d'(0) = 1`). First tau in [tauMin, tauMax] with `d' < 0.15`, then down to
its local minimum; else the global minimum of `d'` in range; then the edge rule. Parabolic interpolation on
the raw `d` (10, item 2). `confidence = clamp(1 - d'(tau), 0, 1)`. Octave behaviour: the first-dip rule avoids sub-octave
errors; an octave-UP error needs `d'(T/2) < 0.15`, i.e. for `sin f0 + a sin 2 f0` roughly `a > 3.6`
(`d'(T/2) ~ 2 / (1 + a^2)`), so the "3x 2nd harmonic" test has margin (0.2 vs 0.15) but not much. Pink
noise is harmless for YIN: its `d` grows monotonically with tau, so `d' >= 1`.

#### 3.5.2 MPM

NSDF `n(tau) = 2 r(tau) / m(tau)` over the whole window (`r = sum_{j<W-tau} x[j] x[j+tau]`, `m` = sum of
squares of both overlapping parts, computed incrementally), tau = 0..maxLag. The initial lobe around
tau = 0 (up to the first negative-going zero crossing) is skipped; every later lobe runs from a
positive-going to the next negative-going zero crossing (a lobe still open at maxLag ends there); key
maximum = highest n in a lobe. Among the key maxima in [tauMin, tauMax] (edge rule), the first one with
`n >= 0.9 * (highest of them)` wins; none = unvoiced. Parabolic interpolation.
`confidence = clamp(n(peak), 0, 1)`. Skipping the tau = 0 lobe is what keeps pink/low-passed noise
(high NSDF at small lags, often no zero crossing at all) unvoiced. Octave-up margin for
`sin f0 + a sin 2 f0`: `n(T/2) ~ (a^2 - 1) / (a^2 + 1)` = 0.8 for a = 3, below 0.9.

#### 3.5.3 CPU

Direct (no FFT). Worst case Wide/Bass at 22.05 kHz: YIN `maxLag * Wi = 341 * 341 ~= 116k` MAC per frame,
500 frames/s -> 58 MMAC/s; at 48 kHz (496 window, 248 lags): 62k MAC per frame (MPM: about 93k). A
128-sample block at 48 kHz contains at most 2 hop completions (hop = 96 input samples) -> <= ~190k MAC
~= 50-100 us scalar, 2-4 % of the 2.67 ms block, far below the 20 % budget. Inner loops must be plain `float` loops over contiguous
spans (auto-vectorisable). MPM over tau <= tauMax is similar. If a CPU spike ever matters, the detector can
be split across hops later; not needed for M2.

### 3.6 `PitchPostProcessor` (owner A)

1. **Median** of the last 3 frames (5 allowed, not the default): frequency median with unvoiced = 0 Hz
   (also a voicing majority vote), confidence median separately, `rmsDb` of the newest frame unfiltered.
   Delay: 1 frame for length 3. Missing history counts as unvoiced.
2. **Octave guard** on the median output. Reference = last accepted voiced pitch, forgotten after
   `octaveMemoryMs = 60` without voiced frames (a new phrase may start in any octave). A voiced frame
   `12 +- 1` semitones above/below the reference is *suspect*: output `f/2` (or `f*2`), reference kept.
   After suspect frames in the same direction persisted for `octaveConfirmMs = 25` (13 frames at 2 ms)
   the new octave is accepted (output unchanged, becomes the reference). Other voiced frames update the
   reference. Genuine octave leaps therefore cost 25 ms extra, octave glitches shorter than that vanish.
   An onset has no reference, so the guard never delays a note-on. Consequence: if the detector is in the
   wrong octave at the onset, that octave becomes the reference; the correct octave is then accepted
   after 25 ms and the NoteSegmenter changes the note (T_min + minNoteLength). The detector rules (first
   dip / key threshold, edge rule) are the first line of defence; the onset is the weak spot, which the
   missing-fundamental and strong-2nd-harmonic tests cover.

### 3.7 `ScaleQuantizer` (owner B)

`setScale(key, scale)`, `isAllowed(note)`, `quantize(midiPitch, currentNote)`. Scale masks are in the
header (12-bit, relative to the key). `quantize` returns the nearest allowed note in [0, 127] (search
outward from `lround(p)`, at most +-12). **Tie rule:** when two allowed notes are equally near
(`|da - db| <= 1e-9`), the current note wins if it is one of them, otherwise the lower one.

### 3.8 `NoteSegmenter` (owner B)

Called once per frame with the cleaned frame, `absoluteSample` (host-rate index of the input sample that
completed the hop, since `reset()`), its `sampleOffset` in the block, the `BendGenerator` and the sink.
`setSettings()` once per block (never emits). All durations are converted to host samples and compared on
`absoluteSample`, so they are exact and rate-independent.

#### 3.8.1 Voicing gate (hysteresis)

- strong voiced: `f > 0 && confidence >= 0.80 && rmsDb >= gate`
- weak voiced: `f > 0 && confidence >= 0.60 && rmsDb >= gate - 3 dB`
- **Onset:** `onsetConfirmFrames = 1` strong-voiced frame while idle -> note-on at that frame. The
  confirmation is the median of 3 in front of the segmenter: a strong median frame needs 2 of the 3
  newest detector frames voiced with confidence >= 0.80 (frequency and confidence medians). A further
  confirmation frame would cost 2 ms of onset latency that the Bass budget does not have (3.11), and
  adds little robustness anyway: consecutive frames share ~93 % of their window (hop 2 ms vs 18-31 ms).
  Noise and breath are rejected by the confidence threshold and the gate; tune those first.
  **Release:** 3 consecutive not-weak-voiced frames (`releaseFrames`, 6 ms) while a note is on ->
  note-off (subject to minNoteLength). Constants are `static constexpr` in the header, tune with the
  noise/breath tests; `VoicePipeline::latencyBudgetHops` is derived from them.

#### 3.8.2 Stickiness (h, T_min)

`u = stickiness / 100`; `h = 0.4 u` semitones; `T_min = 10 + 50 u` ms (default 50 %: h = 0.2, T_min = 35 ms).
With the current quantised note `q` and the candidate `n = quantize(p, q)` the **change condition** is
`n != q && |p - q| - |p - n| >= 2h`, i.e. the candidate must win by 2h. For chromatic neighbours this is
exactly the spec rule `|p - q| >= 0.5 + h`; in scales with gaps the decision boundary is the midpoint
between the two allowed notes. The condition must hold on consecutive frames for `>= T_min` (timer starts
at the first frame, restarts when the condition fails; the candidate may change while it runs, so a slow
glissando steps through notes); then the note changes to `quantize(p, q)` of that frame. Vibrato +-50 cents
never reaches the default threshold of 70 cents -> one note.

#### 3.8.3 minNoteLength (never delays a note-on)

`minNoteLength` bounds note **duration**, it is not an onset delay:

- a note change is executed only when the current note has lasted `>= minNoteLength` (and T_min is met);
- a note-off is deferred until the note has lasted `>= minNoteLength`; if voicing returns before that,
  the note simply continues;
- each new note (also a legato change) restarts the age.

Short blips (consonants, clicks, breath) never become notes because the onset needs a strong-voiced
median frame (= 2 of 3 frames of periodic, confident, loud input over a window of >= 2 periods), not
because of minNoteLength.
So the onset latency budget (3.11) does not contain minNoteLength.

#### 3.8.4 Octave shift

Output note = `q + 12 * octaveShift`; a note outside 0..127 is not played (treated as unvoiced). A shift
change while a note is on is an immediate note change at the next frame (same `q`, new output note,
legato/retrigger rules, no T_min, no minNoteLength); if the new output note is outside 0..127, an
immediate note-off (+ bend centre) instead.

#### 3.8.5 Velocity

`s = sensitivity / 100`, `x = clamp((onsetRmsDb - gate) / 36 dB, 0, 1)`, `full = 20 + 107 x`,
`velocity = clamp(lround((1 - s) * 100 + s * full), 1, 127)`. `onsetRmsDb` = max rmsDb of the confirming
frames (with the default of 1: the onset frame); for a note change, the rmsDb of that frame. 0 % = fixed 100; 70 % (default) at gate + 18 dB gives 81.

#### 3.8.6 Legato / retrigger and event order (binding)

At one sample offset (all from the same frame):

| Situation | Events in order |
|---|---|
| note start | bend (noteStarted, only if != last), note-on |
| change, `legato = false` (retrigger) | note-off old, bend (new q), note-on new |
| change, `legato = true` | bend (new q), note-on new, note-off old |
| note end / flush | note-off, bend centre (only if last bend != 0) |

At most 3 events per frame. Legato puts the note-on first so mono synths in legato mode glide instead of
retriggering their envelope; the `MidiOutputStage` duplicate-note-on guard never fires because old and new
note numbers differ.

### 3.9 `BendGenerator` (owner B)

- Residual `r = 100 (p - q)` cents relative to the quantised note before the octave shift.
- Off: target 0 (a non-centred bend is sent back to 0 once, e.g. after switching the mode to Off).
  Anchored: dead zone `|r| <= 30` -> 0, else `r - sign(r) 30` (continuous), then one-pole smoothing per frame,
  tau = 30 ms (`a = exp(-hopSeconds / 0.030)`), smoother set to the target on `noteStarted`. True: `c = r`.
- Value: `x = clamp(c / (100 range), -1, 1)`, `bend = x >= 0 ? lround(8191 x) : lround(8192 x)`
  (14-bit signed, -8192..8191, M1 convention); range from `BendRange` (+-2/12/24/48, must match the synth).
- **Rate limit ~200 Hz on absolute samples:** `update()` emits only if the value changed and
  `absoluteSample - lastEmitSample >= max(1, lround(sr / 200))`. With hops every 2 ms this means at most
  every third hop (6 ms, ~167 Hz effective, still "about 200 Hz" and plenty for 5.5 Hz vibrato). Compare
  with the last EMITTED value, so a change that was rate-limited is sent at the next allowed frame.
  `noteStarted()` / `noteEnded()` are not limited (they must be exact) and restart the interval. Block-size invariant because only absolute sample positions are compared.
- Reset to centre on note-off (`noteEnded`) and on every flush.
- Monitor tone: `MonitorSynth` (M1) interprets bends as a fixed +-2 semitones, so with True bend and a
  +-12/24/48 range the monitor tone would not follow the singing that the MIDI describes (spec: "monitor
  tone follows the sung notes"). As implemented (owner D, section 10 item 9): `MonitorSynth` keeps its
  fixed +-2 semitones (M1 behaviour unchanged, no setter); in Voice mode the processor rescales every
  bend it passes to the monitor with `monitorBendFor (bend, bendRangeSemitones (settings.bendRange))`
  (value * range / 2), so monitor bends up to +-2 semitones follow the MIDI exactly and larger ones are
  clamped at +-2 semitones (MIDI output unaffected). Test Pattern mode passes bends unchanged.

### 3.10 `VoicePipeline` (owner C; interface frozen for D)

```cpp
VoicePipeline();                                              // prepares at 48 kHz (allocates)
void prepare (double sampleRate, int maxBlockSize);           // allocates worst case, calls reset()
void reset() noexcept;                                        // silent, sample clock -> 0, forget settings
template <size_t N> void process (const float* mono, int numSamples, const VoiceSettings&, VsEventBuffer<N>&) noexcept;
template <size_t N> void flush (int sampleOffset, VsEventBuffer<N>&) noexcept;
VoiceAnalysisSnapshot getSnapshot() const noexcept;           // any thread (seqlock)
static double computeAlgorithmicLatencyMs (double sampleRate, VoiceRange) noexcept;   // any thread
double getAlgorithmicLatencyMs() const noexcept;
int maxEventsForSamples (int numSamples) const noexcept;
int getActiveNote() const noexcept;
void setFrameObserver (FrameObserver, void* context) noexcept; // tests / vs-eval only
double getFrameCentreDelayInputSamples() const noexcept;      // vs-eval frame time stamps
static constexpr int latencyBudgetHops;                       // derived from the tuning constants
static constexpr double tenorLatencyBudgetMs = 30, bassLatencyBudgetMs = 40;
```

**Sample-offset mapping (binding).** The pipeline keeps an absolute input sample counter `samplePosition`
(int64, since `reset()`). Inside a call, input sample `i` has absolute index `samplePosition + i`. When the
push of sample `i` completes a hop (decimator output at `n % D == 0` *and* framer hop), the frame is
analysed immediately and all its events get `sampleOffset = i`. Hop completions therefore fall on the
absolute samples `n = (m hop - 1) D`, a fixed grid with period `hop * D` input samples (96 at 48 kHz, 88
at 44.1 kHz, 192 at 96 kHz); they depend only on n, never on the block split. The event lands at the first host sample at
which the information exists (causal; `0 <= offset < numSamples`, non-decreasing). The only other offset
is 0 for a settings flush.

**Settings (lock-free).** The processor builds a `VoiceSettings` value from the APVTS raw atomics at the
start of each block and passes it with every `process()` call; no locks, no shared mutable config. The
pipeline sanitises it and compares it with the previous call's settings:

- `requiresFlush` (range, algorithm, key, scale, channel): `segmenter.flush` at offset 0 (note-off on the
  old channel + bend centre), `PitchPostProcessor::reset()`, then the new window/detector/scale apply. The
  signal path (conditioner, decimator, ring) keeps running, so a new onset is confirmed after 2 frames if
  the user keeps singing.
- everything else (gate, stickiness, minNoteLength, octave shift, bend mode/range, velocity, legato):
  takes effect at the next frame, no flush.
- the first call after `reset()` never flushes; a call with `numSamples <= 0` does nothing.

**Preallocation.** `prepare()` sizes everything for the worst case at the prepared rate: window for
`lowestF0MinHz` (Bass/Wide) for the ring, both detectors (YIN and MPM work buffers), decimator history.
The worst case of the usual rates is 44.1 kHz (fa = 22.05 kHz, W = 682, ring 1364 floats, lag buffers
343 floats); hosts just below 32 kHz (D = 1, fa ~ 32 kHz, W = 2 * (ceil(fa / 65) + 1) ~ 986) are handled
the same way because every size is computed from fa in `prepare()`. Detector buffers: `maxWindow / 2 + 2`
(lags 0..maxLag plus one spare). The decimator history is `2 N` floats (N <= 147 taps at 192 kHz). Range and
algorithm changes then only change lengths/pointers.

**Event capacity.** `maxEventsForSamples(n) = 2 + 3 ceil(n / (hop D))`; for n = 4096 at 44.1 kHz: 2 + 3 *
47 = 143; at 8 kHz (D = 1, hop 16): 770 < 1024. The processor's `VsEventBuffer<1024>` with 4096-sample
chunks can never fill (hosts below 8 kHz are not supported; a test asserts the bound at 8 kHz).

**UI snapshot.** `VoiceAnalysisSnapshot {frequencyHz, centsOffset (vs nearestNote), nearestNote,
activeNote, confidence, rmsDb, voiced, latencyMs, frameCounter}`, published once per `process()` call
through `VoiceSnapshotPublisher`: a single-writer seqlock whose fields are relaxed atomics (no data race):
writer `seq+1 (relaxed) / release fence / fields / seq+2 (release)`; reader `seq (acquire) / fields /
acquire fence / seq again`, max 8 attempts, then one unchecked copy (each field valid, never a default
snapshot that would make the tuner flicker). The writer is wait-free; the UI never blocks the audio
thread. Published at the end of each `process()` call that analysed a frame or flushed (not per call:
a 1-sample host block would otherwise publish 48000 times a second), and by `reset()`. "Single writer"
means one thread at a time: `prepare()`/`reset()` on the message thread only while the host does not
process. The UI timer (30 Hz) reads it via a processor accessor.

**Frame observer.** Optional C function pointer + context, called once per hop with
`AnalysisFrameInfo {hopSample, raw, processed, activeNote, voiced}`. Used by tests and vs-eval for frame
metrics; the plugin leaves it null. `getFrameCentreDelayInputSamples()` = FIR group delay +
`D (W - 1) / 2` (implemented) gives the time stamp of a frame's window centre.

### 3.11 Latency budget (onset -> note-on, algorithmic; v0.2.1: this is the confirmed path, see 11.3)

Worst case for an abrupt voiced onset at input sample t0, all in the formula of
`computeAlgorithmicLatencyMs()`:

| Component | Amount |
|---|---|
| Decimator FIR group delay | (N - 1) / 2 input samples (0.30-0.44 ms) |
| Decimation phase | D - 1 input samples (<= 0.06 ms; slightly conservative, see below) |
| Conditioner group delay at f0 min | 3.62 ms (Bass/Wide), 2.51 (Baritone), 1.40 (Tenor), 0.75, 0.40, 0.06 |
| Window fill (whole window must contain voice, worst case) | W analysis samples |
| Frame clock quantisation | 1 hop |
| Median of 3 (2 of 3 frames voiced) | 1 hop |
| Onset confirmation (`onsetConfirmFrames - 1` extra frames) | 0 hops |
| Stickiness T_min, minNoteLength, octave guard | 0 (they never act on an onset) |

`L = 1000 ((N-1)/2 + D-1) / sr + 1000 g(f0min) + 1000 (W + latencyBudgetHops hop) / fa` ms with
`latencyBudgetHops = 1 + medianLength/2 + (onsetConfirmFrames - 1) = 2` (derived in the header from the
constants, so it cannot go stale):

| Range | 44.1 kHz | 48 kHz | 88.2 kHz | 96 kHz | Budget |
|---|---|---|---|---|---|
| Bass | 38.86 | 39.05 | 38.93 | 39.05 | <= 40 |
| Baritone | 31.31 | 31.58 | 31.48 | 31.58 | |
| **Tenor** | **24.03** | **24.21** | 24.13 | 24.21 | <= 30 |
| Alto | 18.75 | 18.93 | 18.83 | 18.93 | |
| Soprano | 15.05 | 15.21 | 15.08 | 15.21 | |
| Whistle | 8.54 | 8.62 | 8.62 | 8.62 | |
| Wide | 38.86 | 39.05 | 38.93 | 39.05 | (<= 40) |

(22.05 / 24 / 32 / 176.4 / 192 kHz: Bass 38.5-39.1 ms, Tenor 23.7-24.2 ms.)

**Review of the first draft.** The first draft (2.5 ms hop, 2 onset frames, `W = 2 ceil(fa/f0min)`) gave
Bass 38.81 ms but left out the conditioner: a 2nd-order 60 Hz high-pass delays the envelope of a 65 Hz
fundamental by ~3.4 ms (+ 0.2 ms DC blocker), which made the true Bass worst case ~42.4 ms > 40 ms.
Changes that make the budget hold: hop 2.5 -> 2 ms (-1.5 ms; spec allows 2-3 ms), onset confirmation 2 ->
1 median frame (-2 ms, see 3.8.1), guard lag (+0.125 ms). Bass now has ~0.95 ms margin in the model:
**nothing may add delay to the Bass/Wide path** (no longer window, no extra hop, no lower hop rate, no
extra onset frame, no extra filtering). If the margin is ever needed: HPF Q 0.5 (-1.1 ms, 3.2).

How exact the bound is: with the frame grid `n = (m hop - 1) D` the exact worst case is
`g + (D - 1) + (W + hop - 2) D + 1 hop D` input samples, so the formula overestimates by `2 D` input
samples (0.13 ms); and "whole window voiced" is pessimistic (YIN/MPM usually become confident when about
2/3 of the window holds the voice; the silent part adds nothing to `d(tau)`). Measured latencies are
therefore lower. The UI shows `L` for the selected range (from `computeAlgorithmicLatencyMs(currentSampleRate,
range)`, so it is right even while the host is stopped). Tests assert `measured <= L` and the spec budgets
(Tenor <= 30 ms, Bass <= 40 ms) with the onset tone at f0 min of the range (worst case for the HPF) and at
a typical pitch. Stickiness adds T_min (10-60 ms) to note **changes** only; the release (note-off) takes
about one f0-min period (rmsDb window) plus 3 frames.

### 3.12 Why it is block-size invariant

Every stage is a per-sample state machine driven by absolute counters: the conditioner has only filter
memories, the decimator emits on `n % D == 0`, the framer on push count `% hop == 0`, the detector is
stateless per frame, post-processor and segmenter advance per frame, and all durations (T_min,
minNoteLength, bend rate limit) are compared on absolute host samples. Settings are applied per call, so
invariance is required (and tested) with constant settings. No floating-point operation depends on how
many samples a call contains.

## 4. Plugin integration (owner D)

### 4.1 Parameters (`plugin/Parameters.h`, done in the scaffold)

All `juce::ParameterID {id, 1}`, appended **after** the M1 parameters (M1 IDs, order and ranges unchanged):

| ID | Type | Values / range | Default |
|---|---|---|---|
| `source` | choice | Voice, Test Pattern | Voice |
| `voiceRange` | choice | Bass, Baritone, Tenor, Alto, Soprano, Whistle, Wide | Tenor |
| `pitchAlgorithm` | choice | YIN, MPM | MPM (decided by vs-eval, section 10) |
| `key` | choice | C .. B | C |
| `scale` | choice | Chromatic, Major, Natural Minor, Harmonic Minor, Major Pentatonic, Minor Pentatonic, Blues, Dorian, Mixolydian | Chromatic |
| `stickiness` | float | 0..100 % | 50 |
| `gateThreshold` | float | -70..-10 dB | -40 (v0.2.0: -45, see 11.1) |
| `minNoteLength` | float | 20..250 ms | 60 |
| `octaveShift` | int | -3..+3 | 0 |
| `bendMode` | choice | Off, Anchored, True | Off |
| `bendRange` | choice | ±2, ±12, ±24, ±48 | ±2 |
| `velocitySensitivity` | float | 0..100 % | 70 |
| `legato` | bool | | false |

Choice index == core enum value (`static_cast<int>`). `params::Source {voice, testPattern}` lives in
Parameters.h (plugin concept). The choice names are `constexpr std::array<const char*, numXxx>` sized by
the core enum counts (a missing or extra name is a compile error; `static_assert` against nullptr
entries), and every default is a named constant `static_assert`ed against `VoiceSettings {}`, so plugin
and core defaults cannot drift. Value texts carry no unit (stickiness "50", gate "-40.0", min note "60",
velocity "70"); the unit is the parameter label ("%", "dB", "ms"), as M1's testTempo ("120.0" + "BPM"),
so hosts that print both do not show "50 % %". `tests/plugin/ParameterTests.cpp` (registered) checks IDs,
version hints, types, ranges, defaults, choice names, the enum mapping, texts and state save/restore.

### 4.2 Processor (`plugin/PluginProcessor.*`)

- Cache the new raw atomics in the constructor. `VoiceSettings getVoiceSettings() const noexcept`
  (any thread): `lround` the choice/int values, cast to the enums, `midiChannel` -> `channel`, then
  `sanitise()`. Add `Source getSource() const noexcept`.
- Members: `VoicePipeline voicePipeline`, `std::vector<float> monoBuffer` (maxChunkSamples, constructor),
  `Source lastSource` (audio thread).
- `prepareToPlay`: `voicePipeline.prepare (sr, maxBlock)` (allocates; allowed there) besides the M1 calls.
  `reset()` / `releaseResources()`: only `flushPending` as before.
- `processBlock`: section 2.1. Source change between blocks -> pending flush (note-offs of whatever the
  other source left, then both sources restart). In Test Pattern mode the pipeline is not processed. Voice
  mode does not run the test generator.
- `processBlockBypassed`: as M1 plus `voicePipeline.reset()`.
- Monitor tone in Voice mode plays the pipeline's events (same `MonitorSynth`), with each bend rescaled
  by `monitorBendFor (bend, bendRangeSemitones (settings.bendRange))` and clamped at +-2 semitones (3.9);
  unchanged in Test Pattern mode.
- Keep latency 0 (`setLatencySamples` is not called).
- State load (`setStateInformation`): parameters missing from the saved state get their defaults
  (JUCE's `replaceState()` alone keeps their current values, so the result depended on the instance's
  settings before the load). An M1 state (0.1.x, no `source`) selects Test Pattern if its `testPattern`
  is not Off (the saved routing test keeps playing), else Voice.
- UI accessors: `VoiceAnalysisSnapshot getVoiceSnapshot() const noexcept` (forwards to the pipeline's
  seqlock), `double getVoiceLatencyMs() const noexcept` =
  `VoicePipeline::computeAlgorithmicLatencyMs (getCurrentSampleRate(), current range)` (48 kHz before the
  first prepare).

### 4.3 Editor (`plugin/PluginEditor.*`, FX and Instrument)

- Header: product name, version text "0.2.0 – M2 pitch to MIDI" (from `VOCALSYNTH_VERSION_STRING`), and
  "Format: <x>" as in M1.
- Main panel: tuner (note name in FL convention C5 = 60, i.e. `names[n % 12] + String (n / 12)`, cents
  needle -50..+50, Hz, confidence bar + voiced LED), detected (sounding) note, latency in ms, dropdowns
  Source, Voice Range, Algorithm, Key, Scale, Bend Mode, Bend Range; sliders Stickiness, Gate, Min Note,
  Velocity Sens., Octave; Legato toggle; input meter with the gate line (snapshot rmsDb vs gateThreshold).
- M1 controls (pattern, tempo, CC, monitor tone, virtual MIDI, mute dry, counters, histogram, drag
  sources, restart) move into a compact "Test & Routing" section that stays reachable (e.g. a collapsible
  panel or a second tab). The timer reads only atomics/snapshots.

### 4.4 Docs and packaging (owner D)

`VERSION` 0.2.0; `docs/M2-TESTING.md` (German: singing -> Vital in FL, what each control does, what to
report); `packaging/windows/LIESMICH.txt` (keep CRLF); README status; PLAN.md M2 checkboxes; CI packages
`docs/M2-TESTING.md` into the Windows ZIP.

## 5. `tools/vs-eval` (owner C)

JUCE-free CLI (`VOCALSYNTH_BUILD_TOOLS`, default ON), links `VocalSynthCore`, includes `tests/core/TestSignals.h`.
Files: `main.cpp`, `WavFile.h/.cpp` (own RIFF reader: PCM 16/24/32 int, float32, EXTENSIBLE, mono downmix;
float32 writer), `Metrics.h/.cpp` (CSV labels `onset_s,offset_s,midi_note`, metrics).

- Runs the pipeline offline (block size option, default 512) per file and per algorithm (`--algorithm both`
  default), with a frame observer for frame metrics and the event list for note metrics.
- Frame time for the pitch metrics: `hopSample - getFrameCentreDelayInputSamples()` input samples (window
  centre).
- Metrics (JSON on stdout): raw pitch accuracy (voiced reference frames within +-50 cents), octave error
  rate (off by 12 +- 0.5 or 24 +- 0.5 semitones), voicing false alarm, note precision/recall/F1 (same note,
  onset +-50 ms, one-to-one), mean onset latency, per algorithm, plus a YIN-vs-MPM summary.
- `--self-test`: builds the synthetic corpus in memory (TestSignals: per-range sines/saws, vibrato,
  glissando, octave leap, missing fundamental, noise, breath bursts), prints the JSON and returns 1 if a
  gate fails (raw pitch accuracy >= 0.95, octave errors < 2 %, note F1 >= 0.85, 0 notes on noise).
  Registered as ctest `vs-eval.self-test`. The gates apply to BOTH algorithms (both are selectable in the
  plugin). `VocalSynthCoreTests` depends on `vs-eval` in CMake, so CI's explicit target list (which does
  not name vs-eval) still builds the executable that ctest runs.
- `--write-corpus <dir>`: the same corpus as WAV + CSV, for listening and for future real recordings in
  `tests/corpus/`.

## 6. Test plan (Catch2, `VocalSynthCoreTests`, one file per module, tags as given)

Signals from `tests/core/TestSignals.h` (header-only, deterministic xorshift64* noise; sine, saw, harmonic
stack with/without fundamental and boosted 2nd harmonic, vibrato, glissando, white/pink noise, breath-like
noise bursts, concat/mix helpers). Sample rates {44.1, 48, 96} kHz where relevant.

- `[conditioner]` DC step decays to 0; 60 Hz at -3 dB +- 0.5; >= 150 Hz within 0.5 dB; block-size
  invariance (bit-exact); NaN input -> finite output; reset; `groupDelaySeconds` against a measured
  envelope delay (tone burst at 65 and 110 Hz) within 0.3 ms; long silence after a tone gives exact zeros.
- `[decimator]` factors; tap counts; DC gain 1; attenuation >= 60 dB for tones above 0.65 fa (aliases);
  passband (<= 0.35 fa) ripple < 0.1 dB; outputs exactly at n % D == 0; block-size invariance; group delay.
- `[framer]` hop/window sizes per range (2 ms hop, `W = 2 (ceil(fa/f0min) + 1)`); window = newest W samples
  in order; hop clock independent of push pattern; range change keeps the hop phase; zero-filled start.
- `[pitchdetector]` YIN and MPM: sines and saws on a grid 65-1047 Hz inside each range preset within
  +-10 cents, including exactly f0min and f0max of each range at 16 / 17.64 / 22.05 kHz (the edge rule
  must not reject them); Whistle 500-2500 Hz within +-10 cents up to 1047 Hz and +-25 cents above (at
  2.5 kHz the period is only 6.4 analysis samples; the spec asks for 65-1047 Hz); missing fundamental
  (harmonics 2..8) and a 3x 2nd harmonic -> f0, not 2 f0; silence -> unvoiced; white/pink noise ->
  confidence < confidenceOn on >= 99 % of frames; edge rule: a tone 1.5 semitones below f0min / above
  f0max of the range -> unvoiced (not the range limit, not an octave); rmsDb of a -20 dB sine within 0.5 dB.
- `[postprocessor]` single outliers removed; voicing majority; octave glitch < 25 ms corrected, sustained
  leap accepted after 25 ms (13 frames at 2 ms); memory timeout; median length 5.
- `[scalequantizer]` every scale x key against the interval table; nearest note; tie -> current, else
  lower; 0..127 limits.
- `[notesegmenter]` (synthetic PitchFrames, no DSP) onset at the first strong frame (`onsetConfirmFrames`); gate hysteresis
  (3 dB, confidence 0.8/0.6); release after 3 frames; stickiness boundary (0.5 + h) and T_min; vibrato +-50
  cents -> one note; minNoteLength defers off/change but never the on; octave shift (incl. change while held,
  out-of-range notes skipped, shift into out-of-range while held -> note-off); velocity formula; legato vs retrigger order; flush; invariants (pairing, no
  duplicate on, offsets as given).
- `[bendgenerator]` value mapping for the 4 ranges incl. clamping; anchored dead zone + smoothing;
  true 1:1; rate limit on absolute samples (>= sr/200 apart, only on change); note start/end not limited;
  Off after a bend sends centre once.
- `[pipeline]` end to end on synthetic audio:
  - sines/saws per range at 65..1047 Hz: one note, the right MIDI number (pitch within +-10 cents in the
    frame observer);
  - vibrato +-50 cents / 5.5 Hz -> exactly one note; in True mode bends follow (+-50 cents ~ +-2048 at +-2 st);
  - glissando -> monotone note sequence, each note >= minNoteLength;
  - octave leap 220 -> 440 Hz -> two notes, no octave flicker; missing fundamental -> right octave;
  - white/pink noise, silence, breath bursts (-20 dBFS) -> 0 notes;
  - scale quantisation (C major, a sung C# goes to C or D per tie/nearest rule); legato vs retrigger;
  - MIDI invariants: every on has an off after the input ends (+ flush), no duplicate note-on, offsets in
    [0, n), non-decreasing;
  - **block sizes {1, 7, 64, 480, 512, 4096}** at each of 44.1/48/96 kHz: identical event lists with
    identical absolute sample positions (bit-exact); across the three rates the same note sequence, onsets
    within 5 ms, velocities within +-3;
  - **latency**: abrupt onset (sine at f0 min of the range and at a typical pitch, -20 dBFS after silence)
    -> note-on within `computeAlgorithmicLatencyMs()` and within Tenor <= 30 ms / Bass <= 40 ms, at
    44.1 / 48 / 96 kHz;
  - event capacity: `maxEventsForSamples (4096) < 1024` at 8 kHz; a 4096-sample call never drops events;
  - settings change (range/key/scale/algorithm/channel) during a note -> note-off at offset 0, new onset;
    a gate/stickiness change does not flush;
  - `[pipeline][cpu]` Wide range, YIN and MPM, saw at 65 Hz + noise, 10 s at 48 kHz in 128-sample blocks:
    worst block < 20 % and mean < 5 % of 2.67 ms (assert only when `NDEBUG`, i.e. Release; report
    otherwise). Robust against a loaded machine (other agents build in parallel, ctest may run tests in
    parallel): run the same input 3 times through fresh pipelines, take per block the MINIMUM of the 3
    times, then the maximum over blocks (a preemption hits one run, not all three);
- `vs-eval.self-test` (ctest) as in section 5.

Plugin tests (`VocalSynthPluginTests`, owner D): the new parameters exist with IDs/ranges/defaults/version 1
and survive state save/restore (done: `tests/plugin/ParameterTests.cpp`); all M1 processor tests stay green with `source = Test Pattern` (they set
`testPattern`; switch them to `source = Test Pattern` where they expect pattern MIDI); Voice mode with a
synthetic sine produces paired notes across block sizes and a 10000-sample block (chunking); switching
source/bypass/prepare flushes; Monitor Tone follows the bend range in Voice mode; editor smoke tests with
the new controls. Note: with the default `source = Voice`, M1 tests that feed a sine (e.g. "Processing is
independent of sample rate and block size", 220 Hz) now produce notes; set `source = Test Pattern` there.

## 7. File ownership (next phase)

| Agent | Files |
|---|---|
| **A** core front-end | `core/*/InputConditioner.*`, `core/*/Decimator.*`, `core/*/Framer.*`, `core/*/PitchDetector.*` (Yin + Mpm), `core/*/PitchPostProcessor.*`, `tests/core/InputConditionerTests.cpp`, `DecimatorTests.cpp`, `FramerTests.cpp`, `PitchDetectorTests.cpp`, `PitchPostProcessorTests.cpp` |
| **B** core note logic | `core/*/ScaleQuantizer.*`, `core/*/NoteSegmenter.*`, `core/*/BendGenerator.*`, `tests/core/ScaleQuantizerTests.cpp`, `NoteSegmenterTests.cpp`, `BendGeneratorTests.cpp` |
| **C** pipeline + eval (after A and B) | `core/*/VoicePipeline.*`, `tests/core/VoicePipelineTests.cpp`, `tools/vs-eval/*` (except its CMakeLists.txt) |
| **D** plugin + UI + docs | `plugin/PluginProcessor.*`, `plugin/PluginEditor.*`, `plugin/Parameters.h` (from now on), `core/*/MonitorSynth.*` + `tests/core/MonitorSynthTests.cpp` (monitor bend range, 3.9), `tests/plugin/*` (incl. `ParameterTests.cpp`), `VERSION` (0.2.0), `docs/M2-TESTING.md` (German), `packaging/windows/LIESMICH.txt` (CRLF), `README.md` status, `docs/PLAN.md` M2 checkboxes, `.github/workflows/build_and_test.yml` (add docs/M2-TESTING.md to the Windows package) |
| design (done) | `core/*/VoiceSettings.h`, `tests/core/TestSignals.h` (complete and shared by A, B, C: frozen; extra helpers go into the test file that needs them), `docs/M2-DESIGN.md`, all CMake files (`CMakeLists.txt`, `core/`, `tests/`, `tools/vs-eval/`) |

Rules: everything is registered in CMake already; nobody edits CMake files. Public interfaces in the
headers are frozen (private members and private helpers of an agent's own classes may change freely); an agent that needs an interface change (or a change in `VoiceSettings.h`) reports it
instead of editing another agent's file. A and B can work in parallel (B tests with synthetic
`PitchFrame`s and needs nothing from A); C integrates both; D works against the `VoicePipeline` interface
in parallel (the scaffold pipeline runs, it just emits no notes until A and B are done). Build directories
are never shared. Implemented scaffold formulas (hop/window, latency, conditioner group delay, stickiness,
velocity, bend value) are binding; an agent that wants to change one reports it, because tables in this
document and other agents' tests depend on them.

## 8. Tuning knobs and risks

- Thresholds to tune with tests (all `static constexpr` in the headers): YIN 0.15, MPM 0.9,
  confidence on/off 0.80/0.60, gate hysteresis 3 dB, onset 1 / release 3 frames, octave guard 25/60 ms,
  bend dead zone/smoothing 30 cents / 30 ms, velocity range 36 dB, HPF Q. Onset frames and the median
  length feed `latencyBudgetHops` automatically; the latency test then shows whether Bass still fits.
- Bass at 40 ms has ~0.95 ms margin in the model (3.11); do not lengthen the window, add hops or filters
  for Bass/Wide. Measured latencies are lower than the model (whole-window assumption).
- YIN octave-up margin with a strong 2nd harmonic is small (3.5.1); MPM is more robust there. A real
  voice with a 2nd harmonic > ~11 dB above the fundamental could start a note an octave high; the octave
  guard fixes it after 25 ms + T_min. vs-eval decides the default algorithm.
- Pink noise at low lags can look periodic; the range-limited lag search and confidence >= 0.8 are the
  defence; the breath test is the arbiter.
- MonitorSynth assumes +-2 semitone bends (3.9); MIDI output is unaffected.
- Plugin parameters with `withStringFromValueFunction` round to the display precision; clap-validator's
  conversion tests passed with the same pattern in M1 (`testTempo`).

## 9. Design review (changes to the first draft)

Checked against docs/M2-SPEC.md. Fixed in the design, headers, stubs, CMake and Parameters.h:

1. **Latency budget was not achievable** for Bass/Wide: the conditioner's group delay at 65 Hz (3.6 ms)
   was missing; the true model value was ~42.4 ms > 40 ms. Now part of the formula
   (`InputConditioner::groupDelaySeconds`), hop 2 ms, onset confirmation 1 median frame,
   `latencyBudgetHops` derived from the constants; Bass 39.05 ms, Tenor 24.21 ms at 48 kHz (3.11).
2. **Range edges** (YIN/MPM): with `W = 2 ceil(fa/f0min)` and lags only up to `ceil(fa/f0min)`, a tone
   below f0 min was reported as a confident f0 min, and f0 min itself could sit on the last lag without
   a neighbour. Guard lag (`maxLag = ceil(fa/f0min) + 1`, `W = 2 maxLag`) + binding edge rule (3.5).
3. **MPM specifics**: the tau = 0 lobe must be skipped and an open last lobe counted; otherwise pink
   noise is "voiced" at small lags (3.5.2).
4. **Seqlock read fallback** returned a default snapshot after 8 raced attempts (tuner flicker); now one
   unchecked copy. Snapshot published once per call with a frame (not 48000/s with 1-sample blocks).
5. **CI**: `vs-eval.self-test` was registered, but CI builds only named targets that did not include
   vs-eval -> ctest would fail on every platform. `VocalSynthCoreTests` now depends on `vs-eval`.
   `${CMAKE_SOURCE_DIR}` -> `${PROJECT_SOURCE_DIR}` in tools/vs-eval.
6. **Parameters.h**: choice lists are compile-time checked against the core enum counts, defaults are
   `static_assert`ed against `VoiceSettings {}`; value texts no longer repeat the unit label
   ("50 %" + label "%"). IDs, types, ranges and defaults were already per spec and are unchanged.
   `tests/plugin/ParameterTests.cpp` added and registered (the open issue "choice counts not checked").
7. **Monitor tone and bend range**: was an "accepted limitation"; it contradicts the spec's "monitor tone
   follows the sung notes" for True bend with ranges > +-2. Assigned to D (3.9); implemented as a
   rescale in the processor with a clamp at +-2 semitones instead of a MonitorSynth setter (section 10,
   item 9).
8. **Interface gaps**: `getFrameCentreDelayInputSamples()` for vs-eval; latency budget constants for the
   tests; NoteSegmenter octave-shift-to-out-of-range rule; plugin test file pre-registered;
   TestSignals.h frozen so A/B/C do not need each other's files.
9. **Real-time / robustness**: conditioner flushes denormal-range memories; CPU test uses min-of-3 runs
   per block so it does not fail while other agents build on the same 4 cores; Whistle accuracy above
   1047 Hz relaxed to +-25 cents (beyond the spec's 65-1047 Hz requirement).

## 10. Implementation notes (changes made while implementing and integrating)

The headers are authoritative; this list records where the implementation departs from sections 3-6.
No interface that D or vs-eval use changed.

1. **Decimator (3.3):** taps designed for 62 dB with the cutoff at 0.49 fa (tap counts unchanged:
   27/39/63/75/147). The 60 dB / 0.5 fa design reached only 58.9-60.5 dB at 0.65 fa; now >= 60.7 dB,
   passband deviation -0.05..+0.01 dB.
2. **Detectors (3.5):**
   - The scan starts at `max(2, tauMin / 2)` so a tone above f0 max is rejected instead of being read an
     octave down; the chosen lag must still lie in [tauMin, tauMax]. The low edge reaches
     `rangeToleranceSemitones = 0.5` below f0 min (lags up to ~3 % beyond W/2; window and latency
     unchanged), so a tone a few cents flat on the lowest note is not cut by the edge rule. Results more
     than half a semitone outside the range are unvoiced, others are reported as measured. (They were
     clamped to [f0 min, f0 max] at first; with limits a few cents inside a note, Tenor 523 Hz < C5 =
     523.25 Hz, Alto 147 Hz > D3 = 146.83 Hz, that made A2 sung 10-40 cents flat read as exactly
     110 Hz and C5 always -0.8 cents: the tuner and True bend could not follow the edge notes.)
   - YIN never reads d'(maxLag + 1) (not computed for this frame): at maxLag the sampled d' is used, so
     the result does not depend on an earlier frame of a longer range.
   - YIN interpolates the period on the raw d(tau) of the three lags around the minimum (recomputed), not
     on d': the normalisation tilt biased short periods (Soprano/Wide at 1047 Hz read 10-14 cents sharp,
     which the clamp had hidden at f0 max). The confidence still uses d'.
   - The front-end accuracy test uses band-limited saws at all three test pitches; the naive saw (its
     host-rate aliases are in-band and inharmonic, +-17 cents near the limits at 44.1 kHz) only in the
     middle of the range.
   - Threshold and key-maximum comparisons use the interpolated peak values (matters for short Whistle
     periods).
   - Level step: a frame is unvoiced if the newest period's level and the whole window differ by more than
     `maxLevelStepDb = 6` (the window straddles an onset or a release; both lag functions are biased there).
   - YIN: the confidence uses the sampled d' when the parabola fit is implausible (vertex < 0); a
     sub-multiple check (`subMultipleRatio` 1.5, `subMultipleSlack` 0.02) prefers T/2 or T/3 over 2T or 3T;
     frames from the global-minimum fallback get at most `fallbackMaxConfidence = 0.75` (can hold a note,
     never start one). Partly filled onset windows otherwise started notes an octave low.
3. **Post-processor (3.6):** only strong frames (`guardMinConfidence = 0.8` = NoteSegmenter::confidenceOn)
   set or keep the octave reference; if the newest two strong frames differ by more than
   `maxSpreadCents = 50`, the confidence is capped at `inconsistentMaxConfidence = 0.75`. New
   `forgetOctaveReference()`, called by the VoicePipeline whenever a note ends (release, flush).
4. **NoteSegmenter / bend (3.8, 3.9):** a held note freezes the bend on weak frames (confidence below
   confidenceOn) and on frames more than `bendHoldDropDb = 3` below the note's level (level follower falling
   at `levelFallDbPerSecond = 20`); at releases the pitch drifts sharp and used to send a bend spike before
   every note-off. `static_assert`s tie the new thresholds to confidenceOn/Off.
5. **VoicePipeline (3.10):** settings are re-applied only when they change (1-sample host blocks).
6. **Measured onset -> note-on latency** (worst of 44.1/48/96 kHz x 8 onset phases x both algorithms,
   `tests/core/VoicePipelineTests.cpp`), all below the model of 3.11: Bass/Wide 34.4 ms (model 39.05),
   Baritone 28.4 (31.6), Tenor 21.9 (24.2), Alto 16.7 (18.9), Soprano 13.4 (15.2), Whistle 7.9 (8.6).
   CPU worst block 3.1 % (YIN) / 4.1 % (MPM) of a 128-sample block at 48 kHz, 6.6 % at 44.1 kHz.
7. **vs-eval (5):** reads 8/16/24/32-bit integer and 32/64-bit float WAV; the range is taken from the
   file-name prefix (`bass_...`); the synthetic corpus also has voice-like items (formants, vibrato,
   jitter, 10-30 dB SNR). The note F1 gate (>= 0.85) applies to notes after a rest only; legato changes
   arrive ~57 ms late (stickiness T_min 35 ms at 50 % plus analysis), so with +-50 ms most of them cannot
   match. Overall F1 is still reported. Results of the self-test corpus:

   | Metric | YIN | MPM |
   |---|---|---|
   | Raw pitch accuracy (+-50 cents) | 0.9928 | 0.9945 |
   | Octave error rate | 0.0000 | 0.0000 |
   | Note F1, all / after a rest / legato | 0.785 / 0.977 / 0.130 | 0.811 / 0.985 / 0.210 |
   | Mean onset latency | 25.0 ms | 20.7 ms |
   | Notes on noise / breath / silence | 0 | 0 |

   **Default algorithm: MPM** (better or equal on every metric, ~4 ms faster onsets, no passing notes in
   Bass legato glides). `VoiceSettings {}.algorithm` and `params::defaultPitchAlgorithmIndex` changed
   together.
   Since weak frames no longer change a held note (item 9), note changes wait for strong frames
   (~2 ms later on average, mean change delay 61 / 59 ms): note F1 all / after a rest / legato is now
   0.767 / 0.977 / 0.052 (YIN) and 0.799 / 0.985 / 0.158 (MPM), mean onset latency 24.5 / 20.4 ms; pitch
   metrics unchanged. MPM stays the default.
8. **Tests (6):** Whistle saws are tested up to 1047 Hz (spec limit); above it the 3rd/4th harmonics fall
   into the decimator transition band and YIN reads up to 29 cents off. `TestSignals.h` gained
   `toneSequence` and `midiToHz` (existing helpers unchanged).
9. **Known limitations:**
   - YIN at 44.1 kHz: a tone just below the range (59.6 Hz in Bass) that stops abruptly (< 10 ms) can give
     one short ghost note at the release; not with a >= 10 ms fade or with MPM. Below-range saws can also
     give weak in-range YIN frames (confidence ~0.65, below the 0.80 needed to start a note). Such weak
     frames hold a note but never change it (NoteSegmenter: a change needs confidence >= confidenceOn),
     so a held A2 followed by G2 / F#2 below the Tenor range stays A2 instead of jumping to G3 / F#3.
   - A tone further below the range with a 3x 2nd harmonic (F2 / E2 in Tenor, 87 / 82 Hz) can read as
     2 f0 with confidence >= 0.8 in MPM: its period does not fit the window, 2 f0 is the best in-range
     periodicity. Then a note an octave high can still start (or a held note change to it).
   - Denormal mode: the plugin runs with flush-to-zero, core tests and vs-eval do not. At 96 kHz around an
     onset after silence the raw f0 can differ by ~1 % in a few frames (note events identical, single bend
     values can differ). The plugin test compares against a reference run with `juce::ScopedNoDenormals`.
   - MonitorSynth keeps +-2 semitone bends; the processor rescales bends for it (`monitorBendFor()`), so
     monitor bends beyond +-2 semitones are clamped (MIDI output is unaffected; stated in M2-TESTING.md).

## 11. v0.2.1 changes (core: faster note-on, gate default)

User feedback on v0.2.0 (FL Studio 2026, Tenor, ASIO 2048 samples): note-on feels slow when the MIDI
goes to an external synth; gate -35..-40 dB works best. The headers are authoritative; this section
records what changed and why. Public API: only additions (source-compatible for the plugin and vs-eval).

### 11.1 Gate default -45 -> -40 dB

`VoiceSettings {}.gateThresholdDb`, the `sanitise()` fallback and `params::defaultGateThresholdDb`
(static_assert-tied) are -40 dB; the range stays -70..-10 dB. On the synthetic self-test corpus the gate
change alone changes nothing (all notes are -12..-22 dBFS; v0.2.0 core at -40 and -45 dB gave identical
metrics), so all metric changes below come from 11.2.

### 11.2 Provisional ("fast") onsets

Why the v0.2.0 onset was slow: YIN integrates over the oldest half of the window and MPM normalises over
the whole window, so both become confident only when most of the window (2 periods of **f0 min**, 18.4 ms
in Tenor) holds the voice, whatever pitch is sung. A C4 in Tenor waited as long as an A2.

- **`FastOnsetDetector`** (PitchDetector.h): newest-anchored normalised cross-correlation on the same
  Framer window: `n(tau) = <a, b> / (|a| |b|)` with `a` = the newest `Wi(tau) = max(floor, tau)` samples
  and `b` the same span `tau` earlier (floor = half a period of f0 min, >= 16 samples). A tone of period T
  is seen once the newest ~2 T samples hold it, i.e. the latency now follows the **sung** pitch. Less than
  one period of integration was tried and rejected: ramp-like waveforms (saw) gave confident wrong periods.
  Cosine instead of NSDF normalisation (level ramps pulled the NSDF peak to shorter lags), zero-phase
  [1 2 1]/4 pre-smoothing (narrow peaks at non-integer periods lost against 2 T: an octave-low start at
  653 Hz), MPM key-maximum selection, edge rule, range tolerance and a 6 dB energy-step check as the full
  detectors. Below the gate it returns after the RMS (no search). Stateless, deterministic, no allocation.
- **Pipeline**: while no note is on, the detector runs every `fastHopSamplesFor (fa) = hop / 2` analysis
  samples (1 ms) on the absolute push counter (block-size invariant like the full frames).
- **NoteSegmenter** (`processOnsetFrame`): while idle and armed, `fastOnsetConfirmFrames = 2` consecutive
  fast frames with confidence >= `fastConfidenceOn = 0.95`, RMS >= gate and pitches within
  `fastMaxSpreadCents = 20` start a **provisional** note (same events and order as a normal start). New in
  this round: **`fastNoteMarginSemitones = 0.15`**: the starting frame's pitch must quantise to the same note
  at p +- 0.15 st, else the run continues and the next clear frame starts it (or the full window decides).
  Without it, a vibrato trough at an onset (alto voice-like item, 63.42 while the note was 64) started a
  4-60 ms wrong-semitone note before the full window corrected it.
- **Confirmation** by the full window: ignored for one hop after the start (event bound). Until one
  window after the start (**settle time**, `setOnsetTiming (..., settle = window)`) the full window still
  holds audio from before the start (the onset, the start of a scoop) and lags behind the fast frames: a
  strong frame then changes the note at once only if its candidate wins by 2 h (the fast frames read
  another note or octave; no T_min / minNoteLength), otherwise the note stays provisional. The first strong
  frame after the settle time moves it to its **nearest** note (`provisionalSettleMarginSemitones = 0.02`: a
  near-tie within the detector noise keeps it, e.g. F# sung in G mixolydian, read 54.002 after a start on F; with
  0 that flipped to G after 19 ms, with 0.05 and 0.1 vocadito_1 F1 fell to 0.266 and 0.254) and ends the provisional state. Review finding (vocadito_1): with 2 h at the first strong frame,
  a mid-scoop reading kept a wrong-semitone start for T_min + minNoteLength (note F1 MPM 0.264 in v0.2.0 ->
  0.225); a small margin already during the settle time corrected towards the stale scoop start (57 -> 56
  for 60 ms) and flip-flopped (notes of 2-6 ms). Now: MPM 114 notes / 24 matched / F1 0.278, YIN 112 / 26 /
  0.304 (v0.2.0: 100 / 21 / 0.264 and 96 / 22 / 0.284; gate -40, Bass range). A scoop longer than about one
  window is still read mid-scoop and changes by the normal path, as in v0.2.0. If no strong frame comes
  within window + 3 hops, the note continues when that frame is weak voiced or a strong frame came before,
  else it ends (false start) and the fast path stays off until a fast frame below the **gate** or a note
  start by the full window (review: with gate - hysteresis, a noise floor just below the gate kept it off
  for the whole session after one short blip: onsets 17.9 instead of 11.9 ms). While it is off, the
  pipeline skips the fast search (`isFastPathBlocked()`, RMS only; same segmenter result). The fast path
  re-arms only after a non-strong fast frame, so it cannot loop against a disagreeing full window. Octave decisions therefore stay with the full-window detector + octave guard
  (where YIN reads a missing-fundamental tone an octave high, the fast note is corrected to YIN's reading,
  as v0.2.0 would have started it).
- **Event capacity**: `maxEventsForSamples()` adds `NoteSegmenter::maxEventsPerOnsetFrame = 2` per call
  (one provisional onset per call at most, see VoicePipeline.h).

Evaluated and **not** taken (gain too small for the risk / cost, numbers from the self-test corpus and the
model): fast hop 0.5 ms (`fastHopDivisor` 4: -0.5..-0.7 ms mean onset, doubles the fast-path CPU); one
fast confirmation frame (-0.7..-0.9 ms mean, but a single confident outlier frame could start a note);
high-pass Q 0.5 (model -0.4 ms Tenor, -1.1 ms Bass at f0 min, mean onset -0.05 ms, but ~3 dB more cut at
the lowest Bass notes); a shorter decimator FIR (at most -0.2 ms, costs alias rejection). A smaller median
delay on the confirmed path was not pursued: that path now only matters when the fast path does not fire.

### 11.3 Latency model (`computeAlgorithmicLatencyMs`, shown in the UI)

`computeAlgorithmicLatencyMs (sr, range) = min (computeFastOnsetLatencyMs (sr, range), computeConfirmedLatencyMs (sr, range))`,
the worst case (tone at f0 min). `computeConfirmedLatencyMs` is the v0.2.0 formula of 3.11 (used when the
fast path does not fire: weak, noisy or slowly rising onsets). The fast path for a tone at f:
`front end (FIR + decimation phase + high-pass group delay at f) + requiredSamplesFor (period of f) + 3 fast hops`
(1 frame-clock quantisation, 1 extra confirmation frame, 1 for the onset bias to settle).
`computeFastOnsetLatencyMs (sr, range, f0Hz)` gives the per-pitch value (for a pitch-dependent display).

| Range (48 kHz) | shown (worst, f0 min) | v0.2.0 shown | typical pitch | confirmed path |
|---|---|---|---|---|
| Bass | 38.05 | 39.05 | 18.81 (D3) | 39.05 |
| Baritone | 30.58 | 31.58 | 15.89 (F3) | 31.58 |
| **Tenor** | **23.21** | 24.21 | **12.65** (A#3) | 24.21 |
| Alto | 17.93 | 18.93 | 10.14 (E4) | 18.93 |
| Soprano | 14.21 | 15.21 | 8.32 (A#4) | 15.21 |
| Whistle | 7.62 | 8.62 | 5.45 | 8.62 |
| Wide | 38.05 | 39.05 | 15.34 (C4) | 39.05 |

44.1 kHz: 0.1-0.2 ms lower (Tenor 23.03), 96 kHz as 48 kHz.

### 11.4 Measured onset -> note-on latency

Sine at -20 dBFS after silence, worst of 44.1/48/96 kHz x 8 onset phases x YIN/MPM
(`tests/core/VoicePipelineTests.cpp`, limits = measured + ~0.8 ms):

| Range | f0 min v0.2.0 -> v0.2.1 | +1 semitone | typical pitch |
|---|---|---|---|
| Bass | 34.44 -> 32.94 | 34.44 -> 31.44 | 32.19 -> **17.19** |
| Baritone | 28.44 -> 27.44 | 28.19 -> 25.69 | 26.44 -> **15.44** |
| **Tenor** | 21.94 -> 21.44 | 21.69 -> 19.94 | 20.69 -> **10.69** |
| Alto | 16.44 -> 16.44 | 16.19 -> 15.69 | 16.69 -> **8.19** |
| Soprano | 13.44 -> 12.69 | 13.19 -> 11.94 | 13.44 -> **6.69** |
| Whistle | 7.94 -> 5.94 | 7.94 -> 5.69 | 7.94 -> **3.94** |
| Wide | 34.44 -> 32.94 | 34.44 -> 31.44 | 31.69 -> **12.94** |

Tenor by pitch (worst, YIN / MPM; v0.2.0 in brackets): A2 21.4 (21.9 / 21.7), C3 18.2 (20.7 / 19.9),
E3 14.9 (20.7 / 19.7), G3 12.7 (20.9 / 18.4), A3 11.4 (20.7 / 18.2), C4 10.2 (20.2 / 15.9), E4 9.2
(20.2 / 13.7), A4 8.7 (20.4 / 11.2), C5 8.2 (19.7 / 10.2). YIN and MPM are now equally fast.

**Limit at f0 min.** "Clearly below 15 ms" for every pitch is not reachable safely: a period cannot be
measured reliably from less than about two periods, and two periods of A2 are 18.2 ms. Below 15 ms in
Tenor from about E3 upward. For the user the much larger part is the host: an ASIO buffer of 2048
samples is ~43-46 ms per buffer (FL's MIDI-out routing likely adds about one more); 256 samples would be
~5-6 ms.

Randomised onset stress test (300 harmonic tones per range, random spectra incl. weak / missing
fundamental and strong 2nd harmonic, +-25 cents detuning, 0-30 ms attacks, 30 % with vibrato, 50 % with
noise at 15-35 dB SNR): Tenor mean / p90 / max 21.4 / 24.4 / 31.8 ms (YIN), 15.6 / 21.3 / 26.5 ms (MPM)
-> 12.5 / 18.1 / 25.5 ms (both). Tones whose first note is wrong (in v0.2.1 all of them octave cases), all ranges:
YIN 14 -> 4, MPM 22 -> 15; tones with more than one note: YIN 10 -> 18 (the fast note is right and the full
window later moves to YIN's octave reading, see 11.2), MPM 22 -> 22. Noise-only stress (white, pink,
burst, high band-limited noise at -45..-5 dBFS): 0 notes before and after; only low-pass noise at 30-230 Hz
(rumble, quasi-periodic) gives notes, as in v0.2.0 (242 -> 269 notes in 2100 runs).

### 11.5 vs-eval self-test (synthetic corpus, 43 files)

| Metric | v0.2.0 YIN | v0.2.1 YIN | v0.2.0 MPM | v0.2.1 MPM |
|---|---|---|---|---|
| Raw pitch accuracy (+-50 cents) | 0.9928 | 0.9928 | 0.9945 | 0.9945 |
| Octave error rate (raw / processed) | 0 / 0.0003 | 0 / 0.0003 | 0 / 0.0008 | 0 / 0.0008 |
| Note F1 all | 0.767 | 0.779 | 0.799 | 0.808 |
| Note F1 after a rest | 0.985 | **0.996** | 0.989 | **0.996** |
| Note F1 legato | 0.000 | 0.000 | 0.144 | 0.161 |
| Mean onset latency (matched notes) | 24.5 ms | **15.8 ms** | 20.4 ms | **17.2 ms** |
| Onset latency after a rest, mean / p95 | 25.0 / 41.9 ms | 15.5 / 30.9 ms | 19.0 / 35.9 ms | 15.4 / 30.9 ms |
| Notes on noise / breath / silence | 0 | 0 | 0 | 0 |

Same corpus and the same (current) vs-eval for both columns; v0.2.0 = the core of e49ce02. MPM stays the
default (equal pitch metrics, better F1 overall / legato; onset latency after a rest is now the same).

### 11.6 CPU

Worst / mean 128-sample block in % of the block time (`[.benchmark]`, min of 3 runs per block). While a
note is on, the fast path does not run (voiced numbers as v0.2.0). Idle with input **above** the gate
(white noise at -30 dBFS) is the new worst case; below the gate the fast detector stops after the RMS.

| 48 kHz | voiced worst / mean | idle (noise -30 dB) worst / mean |
|---|---|---|
| Wide YIN / MPM | 5.0 / 1.7, 6.4 / 1.9 | 6.3 / 4.0, 7.9 / 4.2 |
| Bass YIN / MPM | 4.9 / 1.7, 9.5 / 2.1 | 7.5 / 4.0, 9.0 / 4.3 |
| Tenor YIN / MPM | 3.2 / 1.6, 3.5 / 1.6 | 2.9 / 1.7, 3.6 / 1.8 |

44.1 kHz (analysis at 22.05 kHz, longer lags): Wide idle worst 16-20 %, mean 7.6-8.1 % (v0.2.0 idle:
worst 8.5-8.8 %, mean 3.4 %); Tenor idle worst 5-6 %, mean 2.9-3.1 %. 96 kHz: worst <= 12.5 %. All below
the 20 % budget at 48 kHz. (Measured with another build running on the 4-core container.)

### 11.7 Tests

New: `[fastonset]` cases for FastOnsetDetector (integration length, steady accuracy +-10 cents in every
range, noise never reaches 0.95, out-of-range tones, onset after ~2 periods, level steps, front-end
regressions) and the provisional NoteSegmenter logic (start, rejection, note-boundary margin, confirm /
correct, false start + gap rule, weak-voiced timeout, re-arming, octave shift, flush / reset); pipeline:
a tone the full window never confirms gives at most one short note; idle CPU case. Changed on purpose:
the latency test also checks the per-pitch fast model and the v0.2.1 limits; the model table test checks
the new values (shown = min (fast, confirmed)); `maxEventsForSamples` + 2; the velocity check uses the
default gate instead of a literal -45; the hysteresis-confirmation case starts at 64.3 instead of 64.45
(64.45 is now within the note-boundary margin and starts nothing).

After the review: segmenter cases for the settle time (2 h while settling, nearest note after it, a note
seen strong is no false start) and for the unblocking rule (noise between gate - 3 dB and the gate, a
full-window note, RMS-only frames while blocked give the same result); pipeline: scoops of 50-100 cents
over 15-40 ms into A3 (wrong start <= one window + 2 hops, never towards the scoop start) and a 12/14 ms
blip over noise just below the gate (later onsets stay on the fast path); the hysteresis section of the
confirmation case now runs with a settle time (the note stays provisional).

### 11.8 Plugin side (summary; details in the plugin headers)

- Calm tuner (`plugin/TunerDisplayModel.h`): the editor still polls at ~30 Hz, but the shown values are
  smoothed with time constants independent of the timer rate: note name only after 100 ms of stability
  (held 250 ms, fades over 200 ms), needle one-pole 120 ms, Hz / cents texts at most 5 changes per
  second, voiced LED hold 150 ms. MIDI output is unchanged by this.
- The latency display uses `computeAlgorithmicLatencyMs()` (worst case, 11.3); the host buffer is shown
  next to it, since at 2048 samples it dominates the total delay. Buffer = the largest host block of the
  last 1-2 s (`RecentBlockSize`, 1 s windows; the largest, not the most frequent, because FL without
  "fixed size buffers" splits its buffer and the largest block matches the ASIO buffer). Total =
  analysis + 2 x buffer (one buffer in, one out); the FL MIDI port is shown separately as about one more
  buffer (unconfirmed). Above 512 samples the Total line turns amber and a hint suggests 128 / 256.
- Own DSP load (`DspLoadMeter`, PluginProcessor.h): `processBlock` / `processBlockBypassed` timed with
  high-resolution ticks; average with a 1 s time constant independent of the block size, peak decaying
  with 3 s; plain members + relaxed atomics only (no allocation, no locks). Shown on both pages, text
  updated at most twice per second.
- Tuner model details: the note name is the sent note in the sung octave (without Octave shift), white
  while sending, grey when it is only the nearest note; confidence bar rises with 15 ms, falls with
  250 ms; only changed regions are repainted. After the review: the sung note comes from the snapshot
  (`VoiceAnalysisSnapshot::quantisedNote`, same publish as `activeNote`; the live Octave parameter flashed
  the name one octave off after an Octave change); a note change refreshes the Hz / cents texts from the
  new pitch at once (the lagged smoothed pitch showed "A4, 260 Hz" for 200 ms after A3 -> A4); the Voice
  page meter compares the pixel key from before the tick's update (a moving peak bar alone did not
  repaint).

## 12. v0.3.1 segmentation (core: extra notes inside held notes)

Corpus finding after v0.3.0 (docs/CORPUS.md, docs/STATUS.md): on real singing note F1 was only ~0.3 and
precision ~0.25, because held notes came out as several notes. The headers (`NoteSegmenter.h`) are
authoritative; this section records the evidence, what changed and what did not. Public API unchanged
(only new constants); the capture interfaces (`VsEventTiming`, the `VoicePipeline` timing overloads) are
untouched. All numbers at the new gate default -35 dB, stickiness 50 %, minNoteLength 60 ms.

### 12.1 Evidence: why held notes split

Inputs: vocadito_1 (bass range, 31 s, 59 notes, scored against annotator A1, annotator A2 and notes segmented
from the manual f0) and a semi-synthetic sung set (10 phrases, 5 min, 494 notes; scratch tool modelled on
vocadito_1: scoops, glides with overshoot, intonation offsets, drift, vibrato, consonant gaps between
repeated notes, breath noise, level variation; not in the repository). The Dagstuhl excerpts in the local
corpus are 1 s and have no note labels. `vs-eval --frames <prefix>` (new, single file) writes every
analysis frame (raw / processed f0, confidence, RMS, active note) and the notes as CSV for this.

Every detected note that matches no reference note, vocadito_1 A1, v0.3.0 core (MPM / YIN):

| Class | MPM | YIN |
|---|---|---|
| right pitch, onset > 50 ms after the reference (late change, or a scoop note before it) | 30 | 31 |
| short wrong-pitch note < 100 ms inside or at the edge of a reference note (passing note of a glide, scoop) | 29 | 27 |
| provisional start corrected within 35 ms (onset scoop read by the fast path) | 12 | 10 |
| wrong pitch >= 100 ms (intonation between two notes, sub-octave onset) | 14 | 12 |
| same-pitch re-attack after a gap < 80 ms (voicing dropout) | 1 | 1 |
| matched | 24 / 110 | 26 / 107 |

So on real singing the extra notes are pitch-driven, not voicing-driven: glides and scoops are stepped
through note by note (each passing note held for minNoteLength = 62 ms, then the next), and the changes
come late. Dropouts are rare (one same-pitch re-attack, after a 23 ms gap that is a new syllable). The
reference itself has same-pitch repeats after gaps of 12-104 ms (re-sung syllables), so bridging gaps of
60-80 ms (the original idea) would have merged real notes and delayed every release; it was not done.
Detected gaps between notes are 80-150 ms (real rests). The sung set shows the same picture (late changes
266 / 280, passing notes 167 / 167, re-attacks 2 / 2 of ~480 extra notes).

Synthetic audio does show a dropout mechanism that the corpus did not: an abrupt level drop of more than
`PitchDetector::maxLevelStepDb` within a window (a sudden decrescendo, a voiced consonant) makes both
detectors report frequency 0 for several frames (the level-step rule against release bend spikes), and the
rise after the dip gives a few frames with confidence 0.3-0.6. A held tone with an 80 ms -12 dB or a
60 ms -20 dB dip (0, 10, 20 ms ramps; Bass, Tenor, Soprano; MPM and YIN) gave 2-3 notes of the same pitch
in 22 of 36 runs.

### 12.2 Changes (NoteSegmenter)

- **Arrival** (`arrivalFraction` 0.5, `arrivalMaxDistanceSemitones` 0.2, `arrivalMaxSpreadSemitones` 0.3):
  a change also happens after T_min / 2 once every frame of that span lies within 0.2 st of the new note
  and their spread is <= 0.3 st (the voice has landed). A vibrato peak only gets there beyond about
  +-85 cents, where T_min alone flips as well. Pitch history = the consecutive frames of the note with
  confidence >= confidenceOn (32 frames).
- **Glide hold** (`glideHoldSemitonesPerSecond` 35 over `glideWindowMs` 8; within `onsetGlideHoldMs`
  120 of a start from rest `onsetGlideHoldSemitonesPerSecond` 8 over `onsetGlideWindowMs` 16): no change
  while the pitch slope (least squares over the history since the change timer started) is above the
  limit. The timer keeps running, so the change goes straight to the note where the glide ends instead
  of stepping through the notes it passes. A slow glissando still steps; after the onset window only fast
  glides are held.
- **Dropout hold** (`dropoutHoldMs` 60, `dropoutMaxDropDb` 24, `dropoutLevelWindowMs` 25,
  `dropoutMinLevelStepDb` = maxLevelStepDb 6, `dropoutPitchConfidence` 0.3): a frame without a
  weak-voiced pitch (frequency 0, confidence < 0.3, or confidence < 0.6 with a pitch that still reads the
  held note) whose level is >= the gate and >= the note level - 24 dB, while the frame levels of the last
  25 ms span >= 6 dB (a level step), does not count towards releaseFrames, for at most 60 ms from the start
  of the run. At a steady level the same frames are a pitch transition (big legato leaps: the octave guard
  holds the old octave, and release + new onset is the fast way there) or noise, and count as before.
  **Re-entry:** if the voice comes back after a held dropout at another note, the first strong frame
  whose candidate wins by 2 h changes the note at once (as the release + new onset did before), not
  after T_min.
- Unchanged on purpose: the onset paths (idle, provisional, settle and correction), the release rule at
  steady levels, the octave guard, the detectors, the post-processor. Note-on latency from rest is
  therefore unchanged (12.4).

### 12.3 Before / after

vs-eval, block 512, before = v0.3.0 core (3e1de2c NoteSegmenter, same vs-eval), after = v0.3.1. P / R / F1 =
note precision / recall / F1 (same MIDI note, onset within +-50 ms); rest / change = F1 of notes after a
rest / legato changes; delay = mean change delay (nearest same note 0-150 ms after a reference change;
vocadito A1/A2 have only 3 such changes, f0-segmented notes 20-21); onset = mean onset latency of the
matched notes (changes with the matched set); rest latency = first note-on of any pitch near a reference
note that does not abut the previous one, mean / p95.

| Input | Algo | P | R | F1 | F1 rest | F1 change | change delay ms | onset ms | rest latency ms |
|---|---|---|---|---|---|---|---|---|---|
| vocadito_1 A1 | MPM | 0.218 -> **0.248** | 0.407 -> 0.441 | 0.284 -> **0.317** | 0.445 -> 0.464 | 0.116 -> 0.139 | 88.1 -> 82.1 | 23.2 -> 21.8 | 20.7 / 53.8 -> 19.2 / 59.1 |
| vocadito_1 A2 | MPM | 0.245 -> 0.286 | 0.422 -> 0.469 | 0.310 -> **0.355** | 0.460 -> 0.484 | 0.123 -> 0.141 | 45.4 -> 39.4 | 23.1 -> 20.0 | 18.0 / 51.8 -> 16.6 / 51.8 |
| vocadito_1 f0 notes | MPM | 0.282 -> 0.343 | 0.477 -> 0.554 | 0.354 -> **0.423** | 0.575 -> 0.575 | 0.188 -> 0.308 | 63.7 -> 59.4 | 26.2 -> 28.2 | 12.6 / 29.6 -> 12.6 / 29.6 |
| vocadito_1 A1 | YIN | 0.243 -> 0.238 | 0.441 -> 0.424 | 0.313 -> 0.305 | 0.484 -> 0.468 | 0.127 -> 0.121 | 92.8 -> 89.5 | 18.8 -> 18.1 | 19.0 / 73.6 -> 21.7 / 73.6 |
| vocadito_1 A2 | YIN | 0.271 -> 0.276 | 0.453 -> 0.453 | 0.339 -> 0.343 | 0.497 -> 0.504 | 0.130 -> 0.127 | 50.0 -> 46.7 | 18.7 -> 19.3 | 15.1 / 44.0 -> 17.8 / 65.8 |
| vocadito_1 f0 notes | YIN | 0.262 -> 0.324 | 0.431 -> 0.523 | 0.326 -> **0.400** | 0.601 -> 0.592 | 0.111 -> 0.253 | 63.4 -> 59.4 | 22.5 -> 26.6 | 13.5 / 40.8 -> 13.1 / 40.8 |
| sung set (494 notes) | MPM | 0.314 -> **0.435** | 0.449 -> 0.581 | 0.370 -> **0.497** | 0.493 -> 0.493 | 0.307 -> 0.501 | 59.2 -> 52.7 | 30.6 -> 33.9 | 18.9 / 33.5 -> 18.9 / 33.5 |
| sung set (494 notes) | YIN | 0.297 -> 0.387 | 0.421 -> 0.514 | 0.348 -> **0.442** | 0.479 -> 0.479 | 0.279 -> 0.421 | 60.7 -> 54.5 | 31.2 -> 33.8 | 19.6 / 35.9 -> 19.6 / 35.9 |
| self-test (43 files) | MPM | 0.806 -> 0.829 | 0.811 -> 0.834 | 0.808 -> **0.832** | 0.996 -> 0.996 | 0.161 -> 0.269 | 58.1 -> 52.8 | 17.2 -> 17.4 | 15.4 / 30.9 -> 15.4 / 30.9 |
| self-test (43 files) | YIN | 0.776 -> 0.824 | 0.781 -> 0.828 | 0.779 -> **0.826** | 0.996 -> 1.000 | 0.000 -> 0.238 | 60.6 -> 56.3 | 15.8 -> 17.2 | 15.4 / 30.9 -> 15.4 / 30.9 |

For reference, v0.3.0 at its gate default -40 dB: vocadito_1 A1 F1 0.278 (MPM) / 0.304 (YIN); the gate
change alone gives 0.284 / 0.313. Extra-note classes after (vocadito_1 A1, MPM / YIN, cf. 12.1): late
right pitch 30 / 31, passing notes 22 / 23 (was 29 / 27), provisional corrections 12 / 10, wrong pitch
>= 100 ms 14 / 15, re-attacks 1 / 1; sung set (MPM): matched 222 -> 286, late 266 -> 202, passing notes
167 -> 103, wrong pitch >= 100 ms 20 -> 39 (a note held through a glide can sit on a neighbour longer),
detected notes 706 -> 660.

Dips and gaps (synthetic, 12.1): dips split 22 of 36 runs before, 0 after. Repeated notes after complete
silence: 20 and 40 ms gaps give two notes before and after in every range; 15 ms: one note in Bass with YIN;
10 ms: one note in Bass and Tenor (before two): the newest period of f0 min never falls below the gate.

YIN on vocadito_1 A1 is the one row that got worse (0.313 -> 0.305): a single note. At 5.87 s the voice falls
49 -> 46 st and the glide hold keeps 50 while it falls; v0.3.0 changed to 47 46 ms after the reference, now
four frames without pitch (steady level: no dropout) end the note and 47 starts again 71 ms after it, outside
the 50 ms tolerance. A2 and the f0 notes improve. The rest latency
of vocadito A1/A2 moves because that metric also counts notes that the annotators separate by 6 ms but that
are one legato change for the segmenter (their "first note-on" is a change, which now comes at other times).

### 12.4 Guarantees checked

- Note-on from rest: identical note-on positions of every note that starts from rest on vocadito_1 (MPM
  38 of 38; YIN 37 of 37 plus one former legato change that now starts after a release); self-test rest
  latency identical (15.4 / 30.9 ms both algorithms).
- Noise, breath, silence: 0 notes (self-test). Vibrato: one note (self-test vibrato files, unit tests up
  to +-80 cents). Octave errors: raw / processed rate unchanged (0 / 0.0003 YIN, 0 / 0.0008 MPM).
- Block-size invariance: unit and pipeline tests (incl. a new scoop + glide phrase with variable host
  blocks); vocadito_1 at blocks 32 / 100 / 512 / 2048 gives byte-identical note lists (both algorithms).
- Release cost of the dropout hold (with vs without, 108 runs over self-test, sung set and vocadito_1,
  1909 notes): note-offs later by mean 0.3 ms, p95 2 ms, max 20 ms (a tenor file at 10 dB SNR whose
  noise floor is above the gate); no note-on or pitch changed except one YIN transition note that got
  32 ms shorter.
- Real-time: fixed-size `std::array` histories (32 pitch, 16 level frames), no allocation, loops bounded
  by those sizes; per-frame state only, so the behaviour does not depend on the block size.

### 12.5 Tried and rejected (sweeps on the same inputs; avg = mean F1 of MPM/YIN x A1/A2/f0 notes)

| Variant | avg F1 | Note |
|---|---|---|
| v0.3.0 core at -35 dB | 0.321 | baseline |
| arrival only | 0.347 | sung change F1 0.39 / 0.43 |
| glide hold only | 0.329 | passing notes go, changes too late without arrival |
| arrival + glide hold (v0.3.1) | **0.357** | |
| T_min x 1.5 / 2 / 3 ("hover") | 0.358 / 0.351 / 0.348 | fewer notes, slower changes |
| longer T_min for young notes (150 ms) | 0.319 | |
| restart the timer when the candidate changes | 0.305 | |
| onset glide hold 200 ms instead of 120 | 0.353-0.359 | no gain, longer wrong-note holds |
| gap bridging 60-80 ms | not built | merges the reference's re-sung syllables, delays every release (12.1) |

### 12.6 Tests

`NoteSegmenterTests.cpp` (`[segmentation]`): arrival span vs T_min (on the note, beyond the distance, an
unsteady pitch, weak frames clear the history; 3 rates x 3 stickiness values), wide vibrato one note, a fast
glide goes straight to its end note, a scoop from rest goes straight to its target while the same glide later
in a note still steps, a clean step after the onset is not held, dropouts / confidence dips / level dips hold
one note and a clear gap re-attacks, and the dropout hold (no pitch, low confidence on the note and noise
pitch after a level drop hold the note; the same frames at a steady level, low-confidence frames on another
note, frames below the gate and frames more than 24 dB below the note release after releaseFrames as before;
the hold is bounded by dropoutHoldMs; re-entry at another note changes at the first strong frame, legato and
retrigger; re-entry at the same note continues). `VoicePipelineTests.cpp` (`[pipeline][segmentation]`, audio):
held notes through -12 / -20 dB dips, +-3 dB tremolo and breath noise at 6 / 0 dB SNR stay one note (Bass,
Tenor, Soprano, YIN and MPM, 3 rates); repeated notes after 20 / 40 / 120 ms of silence give two notes; a scoop
from rest plus a fast glide gives no passing notes and is block-size invariant (512, 37, variable blocks).
Changed on purpose: two change-delay checks in `VoicePipelineTests.cpp` (a clean step now waits the arrival
span T_min / 2, not T_min), the arrival/T_min expectations of two older change tests, and the default-gate
lines (-35).
