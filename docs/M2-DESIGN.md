# M2 design: pitch -> MIDI (v0.2.0)

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
neighbours exist, parabolic interpolation is always possible; the result is clamped to [f0min, f0max]
(it can only leave the range by less than one lag). `rmsDb` = RMS of the
newest `ceil(fa / f0min)` samples (one period of f0 min: reacts faster than the whole window, so onsets and
releases pass the gate sooner), floor -120 dB. Below `silenceFloorDb = -90 dB` the frame is unvoiced
without searching.

#### 3.5.1 YIN

Integration length `Wi = W - maxLag = maxLag`; `d(tau) = sum_{j<Wi} (x[j] - x[j+tau])^2`, tau = 1..maxLag;
cumulative mean normalised `d'` (`d'(0) = 1`). First tau in [tauMin, tauMax] with `d' < 0.15`, then down to
its local minimum; else the global minimum of `d'` in range; then the edge rule. Parabolic interpolation on
`d'`. `confidence = clamp(1 - d'(tau), 0, 1)`. Octave behaviour: the first-dip rule avoids sub-octave
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
  tone follows the sung notes"). Fix (owner D, section 7): add
  `void MonitorSynth::setPitchBendRangeSemitones (double semitones) noexcept` (default 2, clamp 1..48, M1
  behaviour unchanged) and have the processor set it from `bendRange` every block in Voice mode and back
  to 2 in Test Pattern mode (the M1 patterns assume +-2).

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

### 3.11 Latency budget (onset -> note-on, algorithmic)

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
| `pitchAlgorithm` | choice | YIN, MPM | YIN (re-decided by vs-eval) |
| `key` | choice | C .. B | C |
| `scale` | choice | Chromatic, Major, Natural Minor, Harmonic Minor, Major Pentatonic, Minor Pentatonic, Blues, Dorian, Mixolydian | Chromatic |
| `stickiness` | float | 0..100 % | 50 |
| `gateThreshold` | float | -70..-10 dB | -45 |
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
and core defaults cannot drift. Value texts carry no unit (stickiness "50", gate "-45.0", min note "60",
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
- Monitor tone in Voice mode plays the pipeline's events (same `MonitorSynth`), with
  `setPitchBendRangeSemitones (bendRangeSemitones (settings.bendRange))` (3.9); 2 in Test Pattern mode.
- Keep latency 0 (`setLatencySamples` is not called).
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
| **D** plugin + UI + docs | `plugin/PluginProcessor.*`, `plugin/PluginEditor.*`, `plugin/Parameters.h` (from now on), `core/*/MonitorSynth.*` + `tests/core/MonitorSynthTests.cpp` (bend range setter, 3.9), `tests/plugin/*` (incl. `ParameterTests.cpp`), `VERSION` (0.2.0), `docs/M2-TESTING.md` (German), `packaging/windows/LIESMICH.txt` (CRLF), `README.md` status, `docs/PLAN.md` M2 checkboxes, `.github/workflows/build_and_test.yml` (add docs/M2-TESTING.md to the Windows package) |
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
   follows the sung notes" for True bend with ranges > +-2. Assigned to D with a defined setter (3.9).
8. **Interface gaps**: `getFrameCentreDelayInputSamples()` for vs-eval; latency budget constants for the
   tests; NoteSegmenter octave-shift-to-out-of-range rule; plugin test file pre-registered;
   TestSignals.h frozen so A/B/C do not need each other's files.
9. **Real-time / robustness**: conditioner flushes denormal-range memories; CPU test uses min-of-3 runs
   per block so it does not fail while other agents build on the same 4 cores; Whistle accuracy above
   1047 Hz relaxed to +-25 cents (beyond the spec's 65-1047 Hz requirement).
