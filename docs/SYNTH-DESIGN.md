# Synth design: release 0.4.0 "Eigener Klang"

Status: binding design for 0.4.0 (2026-11-03). Basis: `docs/ROADMAP-0.4.md` sections 3 and 4.1, decisions E1–E6
accepted with the recommended options (synth first, native JUCE UI, full scope, synth on for new instances,
Instrument = full synth, 24 presets / default Legato). The scaffold (all headers with frozen public interfaces,
compiling stubs, parameters, CMake entries, one test file per module, 0.3.1 state fixtures) is in the tree.
Reviewed by the critique agent on the same day: findings and the resulting changes are listed in 23.
Implemented in waves 1–3 and integrated on 2026-11-06: the measured numbers are in 18, the final preset names in
14.3, every departure from this text and every interface addition of the implementation in 24.

Rules for every implementer: `docs/ROADMAP-0.4.md` 3.3 (real time), this document, the doc comments of the
frozen headers. A public signature, a constant documented as binding, a parameter (ID, range, default, choice
list), a state property or a file format changes **only** through this document (ask the integration agent).
Private members and the bodies of the `.cpp` files are free for the owner (section 19).

Contents: 1 scope · 2 signal flow and threads · 3 control tick, block-size invariance · 4 SynthSettings ·
5 wavetables · 6 oscillators · 7 filter · 8 envelopes and LFOs · 9 mod matrix · 10 voices · 11 FX ·
12 wavetable store and worker · 13 state and migration · 14 presets · 15 parameters · 16 expression ·
17 processor integration · 18 CPU budget · 19 file ownership · 20 UI foundation · 21 test plan · 22 risks ·
23 critique log (findings of the critique agent and what changed; read it first if you knew an earlier draft) ·
24 implementation record (departures, interface additions, integration).

---

## 1. Scope

In 0.4.0: the wavetable synth of ROADMAP 3.2 (2 wavetable oscillators with unison, sub, noise, one SVF with
drive, 3 ADSR, 2 global LFOs, 8 mod slots, Mono / Legato / Poly 16, glide, chorus / delay / reverb), 14 factory
wavetables generated in code, WAV import embedded in the project, voice expression (level, brightness,
noisiness, pitch) as mod sources, 24 factory presets and user presets, the UI foundation and the Synth page.

Not in 0.4.0 (unchanged from ROADMAP 2): second filter, warp / FM / sync per oscillator, wavetable editor,
oversampling / HQ mode, arpeggiator, MPE, vowel tracking (0.5), expression -> CC (0.6), chords (0.6).

Unchanged: the MIDI output path, the event format (`VsEvent`, `VsEventTiming`), capture, the test pattern, the
monitor tone (`MonitorSynth`, the routing test tone of the "Test & Routing" page; that page becomes "Setup" only
in 0.5), reported latency 0, plugin codes and
every existing parameter ID, range and choice list.

## 2. Signal flow and threads

```
 Mic -> downmix -> VoicePipeline.process (mono, n, voiceSettings, events, timings, expression)   [Voice mode]
                   TestPatternGenerator.process (n, settings, events)                           [Test Pattern]
                        |                                   |
                        | events (+ timings)                | ExpressionBuffer (one frame per 2 ms hop)
                        v                                   v
   MidiOutputStage -> MIDI out          SynthEngine.process (events, timings, expression | null, synthSettings,
   CaptureTap (unchanged)                                    transport, synthL, synthR, n)
   MonitorSynth (Test Tone)               VoiceAllocator -> 16 SynthVoice -> sum -> Chorus -> StereoDelay
                                          -> FdnReverb -> synthVolume
 FX out:          dry * dryGain (0 if muteDry)  +  synth  [+ test tone]
 Instrument out:  synth                         [+ test tone]       (the dry sidechain is never passed through)
```

| Thread | Does | Never |
|---|---|---|
| Audio (processBlock) | pipeline, engine, mix, `WavetableStore::acquire/release`, `ExpressionPublisher::publish` | allocate, free, lock, IO, log, throw, touch a `shared_ptr` |
| Message | UI, preset load / save (`PresetManager`), import dialog, state get / set (as today; hosts may also call them from another non-audio thread), synchronous table loads (12.2) | wait for the audio thread |
| WavetableWorker (one per instance) | asynchronous UI requests: read WAV bytes, build mip maps, publish to the store; garbage collection every 100 ms | touch APVTS parameters |
| CaptureWorker | unchanged | – |

Process-wide: factory tables and user tables are shared between instances by key (12.3); immutable after build.

## 3. Control tick and exact block-size invariance

### 3.1 Clock

`SynthEngine` keeps `samplePosition` (int64, 0 at `reset()`), advanced by every `process()` call. An event with
`sampleOffset` o in a call starting at clock c happens at absolute sample c + o. Nothing in the engine may
depend on where a host block or a processor chunk starts or ends.

### 3.2 Tick

`controlTickSamples = 16` (`SynthSettings.h`). A control tick runs at every absolute sample t with t % 16 == 0.
At a tick the engine, in this order:

1. latches the settings of the current `process()` call and advances the 20 ms smoothers (one-pole per tick,
   coefficient `1 - exp (-16 / (0.020 fs))`; the first tick after `reset()` jumps to the targets; the
   logarithmic parameters cutoff, LFO rates, chorus rate and delay time are smoothed in log2, the dB values
   in dB); choices, ints and bools switch here (a `playMode` change -> `VoiceAllocator::releaseAll()`);
2. latches pending expression frames (16.4) and advances the expression ramps; advances the bend smoother
   (one-pole, 5 ms, per tick);
3. advances both LFOs (`Lfo::advanceTick` with the block's `SynthTransport`) with the LFO rate modulation of
   the previous tick (a one-tick delay for every global destination: no evaluation cycles, LFO 1 -> LFO 1
   rate is legal); a `retrigger()` since the previous tick restarts the LFO here (8.2);
4. computes the global mod sources (LFOs, expression) and, with the per-voice sources of the newest voice
   (`VoiceAllocator::getNewestVoice`, previous tick), the global destinations (LFO rates, FX mixes);
5. calls `SynthVoice::controlTick()` for every active voice;
6. calls `beginTick()` of Chorus, StereoDelay, FdnReverb with this tick's settings and mix modulation.

Every module ramps linearly from its value at the tick start to its target at the tick end (16 samples). Between
ticks only per-sample DSP runs.

### 3.3 Voice control protocol (binding, `SynthVoice.h`)

- At a sample that is both an event position and a tick boundary, the events come first (commands applied with
  `samplesToNextTick == 16`, no ramp change), then the tick (`controlTick`).
- A command inside a tick (`samplesToNextTick < 16`): the voice applies it at that sample; envelopes that
  restart do so from the current ramp value; restarted modules advance by `samplesToNextTick`; all ramps are
  re-targeted from their current values to the (new) targets at the next tick boundary over the remaining
  samples. Non-restarted envelopes keep their state (already advanced to the tick end).
- `render (L, R, n)` with n <= samples left in the tick only runs per-sample DSP.

### 3.4 Events

- Note events of one absolute sample form one `VoiceAllocator::processGroup()` call (10.1). Bends at that
  sample are applied before the group (last one wins); CCs are ignored by the engine. If no voice was active
  before a group that starts a voice, the bend smoother jumps to the bend target (a note from silence starts
  on its bend instead of sliding 5 ms from the previous note's value). The pipeline sends the centring bend
  AFTER a note-off at the same sample (NoteSegmenter event order), so a releasing tail slides back to the
  note centre in 5 ms - exactly what an external synth on the MIDI port does.
- Pitch bend: `bend / 8192 * settings.bendRangeSemitones` (8191 -> just below +range), one-pole 5 ms per tick.
  The processor sets `bendRangeSemitones` from the voice setting `bendRange` in Voice mode and 2 in Test Pattern
  mode (the patterns assume +-2, like the monitor tone). Bend mode True therefore follows the voice, Anchored
  holds clean notes.
- `timings[i].flags & EventTimingFlags::correction` marks a provisional-note correction (10.2). Timings may be
  shorter than events (test pattern: empty) — missing entries mean "no flag".

### 3.5 Rules that make the output bit-identical for every block split

Required: block sizes {1, 7, 64, 480, 512, 4096} x {44.1, 48, 96 kHz} give bit-identical L / R output within a
build (`SynthEngineTests`, also through the processor for the synth part).

- All state changes happen at absolute positions only (events, ticks). No "per block" computation of anything
  audible: no smoothing per block, no ramps whose length depends on the block, no per-block PRNG draws.
- Per-sample loops are written so that rendering [a, c) then [c, b) performs exactly the same floating-point
  operations as [a, b): incremental ramps (`v += step`) carried in members, never recomputed from the segment
  start; no reductions whose order depends on the segment length.
- Vectorise across unison voices / channels only, never across time. No `-ffast-math`, no FMA contraction
  differences inside one build (the build flags are the project's; do not add any).
- PRNGs (xorshift32) are seeded in `prepare()` / `reset()` and drawn only at note starts, ticks or per sample.
- Every state DECISION is taken at a tick or an event position from per-sample-accumulated state, never at
  the end of a `process()` call or of a render segment (whose lengths depend on the host block): voice
  inactive, synthOn fade end, FX bypass / clear / tail end, NaN resets, the `|x| < 1e-20 -> 0` denormal
  flushes (`SvfFilter`, FX feedback states). A flush "after each render() call" would make the output
  differ in the last bits between block splits.
- A `Lfo::retrigger()` at a mid-tick command takes effect at the next tick (quantised to the grid); an
  envelope restart happens at the command's sample (3.3).
- Settings are latched at ticks; a `process()` call that changes settings mid-tick affects the next tick only.
  (Host automation arrives per block, so automated output is only invariant for equal settings — by design.)
- Cross-platform comparisons (MSVC vs clang) use feature fingerprints only (21, golden).

## 4. SynthSettings (`core/include/vocalsynth/synth/SynthSettings.h`)

One plain struct with every synth parameter in the parameter's own units ("Klarwerte": percent 0..100, ms, dB,
Hz, cents), enums whose values equal the choice indices, and `bendRangeSemitones` (filled by the processor).
`SynthSettings {}` IS the default of every parameter (`SynthParameters.h` takes its defaults from it). Ranges:
`synth::limits`. `sanitised()`: clamp every value to its range, non-finite -> default, enums forced into their
range, ints rounded and clamped, `bendRangeSemitones` clamped 0..48; real-time safe. The engine sanitises the
settings it receives once per `process()` call. `SynthTransport { bpm, quarterNotesPerBar }` lives in
`SynthSettings.h` too (the LFO, the delay and the engine use it; 8.2).

## 5. Wavetables

### 5.1 Data format (`Wavetable.h`)

- Frame size 2048, at most 256 frames, 11 mip levels. Level L holds harmonics 1..1024 >> L (bin 1024 of
  level 0, the source Nyquist, is always dropped) at `mipLength (L) = clamp (8 * (1024 >> L), 1024, 8192)`
  samples, i.e. at least 8x oversampled, with 1 guard sample before (copy of the last sample) and 2 after
  (copies of samples 0, 1) for the 4-point Hermite read (6.1):

  | level | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
  |---|---|---|---|---|---|---|---|---|---|---|---|
  | harmonics | 1024 | 512 | 256 | 128 | 64 | 32 | 16 | 8 | 4 | 2 | 1 |
  | length | 8192 | 4096 | 2048 | 1024 | 1024 | 1024 | 1024 | 1024 | 1024 | 1024 | 1024 |

- 22528 + 33 = 22561 floats per frame (`frameStride`), levels back to back, frames back to back, one
  `std::vector<float>`. Memory: 90.2 KB per frame, 5.8 MB for 64 frames, **23.1 MB for 256 frames** (bound).
  (The ROADMAP's 7.6 MB layout - 4x oversampled, linear interpolation - fails the alias gate, 23 F1.)
- Immutable after the builder returns; `getContentHash()` = FNV-1a 64 of the PCM16 frames (13).
- Level 0 is the band-limited source frame upsampled to 8192 (the UI draws it decimated); embedding uses
  the PCM16 frames (`WavetableFrames`) instead.

Why 8x + Hermite (critique measurement, saw 1/k, worst-case f0 of each level, all non-harmonic energy
relative to the fundamental, 44.1 / 48 / 96 kHz): 4x + linear -42 dB (gate: -60 / -70 dB), 8x + linear -52 dB,
16x + linear -61 dB, 8x (min 512) + Hermite -62 dB, **8x (min 1024) + Hermite -72 dB** (worst: level 3;
level 0 at f0 <= 21 Hz -81 dB). Method: 23, F1; agent A's alias test (21) is the gate.

### 5.2 RealFft (`RealFft.h`)

Own iterative radix-2 complex FFT of size N/2 with the standard real-input split (twiddles in `prepare`),
double precision, sizes 16..16384. Conventions in the header (unnormalised forward, 1/N inverse). Used by the
builder (background) and the expression tracker (audio thread after `prepare`, `powerSpectrum`).

### 5.3 WavetableBuilder (`WavetableBuilder.h`)

`build (name, samples, frameSize, cancel)`: resample every frame to 2048 via its spectrum (frameSize != 2048;
frameSize 0 = single cycle of 2..8192 samples via direct DFT of harmonics 1..min (1023, len / 2 - 1)), thin to
<= 256 frames (`round (i * (n - 1) / 255)`), remove DC per frame, peak-normalise the whole table to 1, quantise
to PCM16 and back (so a table rebuilt from embedded data is bit-identical), then per frame and level: FFT 2048,
keep bins 1..min (maxHarmonic, 1023), inverse at `mipLength` (1024..8192: zero-padded spectrum), guard
samples. Deterministic. Cancel flag polled between frames. Target: 256 frames in < 150 ms (Release,
background; estimate ~0.3 ms per frame for the 11 inverse FFTs), any factory table < 150 ms.
`makeSineTable()`: 1 frame, every level a pure sine (fallback in `SynthEngine::prepare`).

### 5.4 Import (`WavReader.h`, `WavetableBuilder::chooseFrameSize / buildFromWav`)

- `WavReader::read (bytes)`: RIFF/WAVE, PCM 8 (unsigned) / 16 / 24 / 32, float 32 / 64,
  `WAVE_FORMAT_EXTENSIBLE` with these sub-formats, 1..8 channels -> mean (mid), chunks in any order, padding,
  unknown chunks skipped; `clm ` chunk: text "<!>2048 ..." -> frame size (power of two 32..8192). Data chunk
  longer than the file: the complete sample frames present are read, `dataTruncated = true`. More than
  256 x 4096 sample frames: `tooLong`. Clean errors (`WavReadError`, `describeWavReadError`) for everything else;
  no exception except `std::bad_alloc`, no read outside the span (fuzz test). The plugin reads the file with
  `juce::File::loadFileAsData` (Unicode paths, spaces, umlauts) and passes the bytes; the core does no file IO.
- Frame size (`chooseFrameSize`; the import asks first for ambiguous files, 24.4 R2-3): import-dialog override (256 / 512 / 1024 / 2048 / 4096) if the file holds at
  least one such frame; else the `clm ` size; else 2048 if the length is a positive multiple of 2048; else 0 =
  single cycle if the file has at most `maxSingleCycleSamples` = 8192 samples (AKWF-style single cycles are
  256..4096); else 2048 (a long recording without `clm ` is not one giant cycle: no 1M x 1023 DFT). Leftover
  samples after the last whole frame are ignored.
- > 256 frames: thinned evenly; DC removed; peak-normalised; frames are not phase-aligned (phase-stable).
- The worker stores the PCM16 frames (`UserTableData`) for embedding and the hash for sharing.

### 5.5 Factory tables (`FactoryWavetables.h`)

Generated in code, deterministic (seeded where random), built on demand by a worker, shared process-wide.
Ids are permanent (state / presets: `factory:<id>`); frame counts may be tuned by agent A (positions are
percent). Recipes (spectra are built additively up to harmonic 1023 unless a time-domain process is named; the
builder band-limits anyway):

| id | name | frames | recipe |
|---|---|---|---|
| basic-shapes | Basic Shapes | 64 | keyframes sine, triangle, saw, square, pulse 25 % at 0 / 25 / 50 / 75 / 100 %, linear crossfade of the band-limited spectra between neighbours |
| pwm | PWM | 64 | pulse width 50 % -> 10 % (implemented; 24) |
| harmonic-sweep | Harmonic Sweep | 64 | odd harmonics (1/k) crossfading to even-only (1/k) + fundamental |
| saw-bright-dark | Saw Bright-Dark | 32 | saw with spectral tilt 0 -> -12 dB/oct |
| formant-vowel | Formant Vowels | 64 | saw spectrum x formant envelope (F1..F3 Gaussian bumps), vowels A E I O U at 0/25/50/75/100 %, f0 reference 130 Hz |
| sync-sweep | Sync Sweep | 64 | hard-sync saw, slave/master ratio 1 -> 8 (time domain at 8x, extra roll-off, then the builder band-limits; 24) |
| fm-2op | FM 2-Op | 64 | sin (p + I sin (2 p)), index I 0 -> 8 |
| fold | Fold | 64 | sine through a triangle wavefolder, gain 1 -> 8 |
| organ | Organ | 32 | drawbar registrations (16', 8', 5 1/3', 4', 2 2/3', 2', ...) |
| digital | Digital | 32 | saw quantised to 32 -> 2 levels and sample-held at 1/1 -> 1/32 |
| spectral-random | Spectral Random | 32 | seeded random amplitudes (1/k envelope) and phases, 8 keyframes interpolated |
| bell | Bell | 32 | sparse partials at harmonics 1, 2.76 -> 3, 5.4 -> 5, 8.9 -> 9 ... (rounded), decaying brightness |
| square-stack | Square Stack | 32 | square + octave squares, 1 -> 4 layers |
| choir | Choir | 64 | mixtures of two vowel formant sets (A/O, E/I, O/U ...) with a soft saw source |

Default table for both oscillators: `basic-shapes` (`defaultFactoryTable`); new instances, old projects, missing
references and Init. Until a table is published, the oscillator plays the engine's built-in sine.

## 6. Oscillators

### 6.1 WavetableOscillator (`WavetableOscillator.h`)

- Unison n = 1..8, SoA phases (double), increments, L/R gains. Voice i of n (n > 1): detune offset
  `detune * (i / (n - 1) - 0.5)` cents, pan `p = clamp (pan + spread * (2 i / (n - 1) - 1), -1, 1)`,
  equal-power law `gL = sqrt (2) cos ((p + 1) pi / 4)`, `gR = sqrt (2) sin ((p + 1) pi / 4)` (1 / 1 at the
  centre); the unison sum is scaled by `1 / sqrt (n)`. Detune ratios and pan gains are cached per oscillator
  and recomputed only when unison / detune / pan / spread change (no 16 `exp2` + 32 `sin`/`cos` per tick).
- Start phase (`restart`): voice i = phaseRandom * r_i (xorshift32 from the voice's seed); 0 = all phase 0.
  Legato never restarts phases. Unison count changes at a tick; added voices start at phase r_i * phaseRandom.
  Note for presets (F): with phaseRandom 0 the n unison voices start in phase, so the first cycles peak at
  `sqrt (n)` times the steady level (+9 dB at n = 8): supersaw presets use phaseRandom >= 50 %.
- Reading (binding): frame position `pos * (numFrames - 1)`; inside a level `x = phase * mipLength`,
  `i = floor (x)`, `f = x - i`, 4-point 3rd-order Hermite (Catmull-Rom) on `p = level + i` (formula in the
  header; guard samples, no wrap test); linear between the two neighbour frames. Position, level, pan ramp
  per sample over the tick.
- Mip level per tick (largest increment of the tick over start / end and all unison voices): lowest L with
  `(1024 >> L) * inc <= 0.45`, back down only when `<= 0.40`; a change crossfades linearly over that tick
  (one tick is enough, 22). inc > 0.45: silent (gain ramps to 0).
- Table change: crossfade over `ceil (0.005 fs / 16)` ticks from the next tick. A change during a running
  crossfade is queued (newest wins) and starts when the running one ends (never more than two tables read,
  no jump from dropping a half-faded table). A silent oscillator (no note yet, or gain 0) takes a new table at
  the next tick without a crossfade; `restart()` ends a running crossfade. `getTablesInUse()` = {current,
  previous while fading}; a queued table is not reported (12.1). `reset()` forgets every table pointer.
- nullptr table: silence.

### 6.2 SubOscillator (`SubOscillator.h`)

1-frame tables (sine, triangle, square) with the same mip and Hermite rules, built once per PROCESS by the
first `prepare()` (function-local static; 3 x 90 KB instead of 3 tables per voice per instance); pitch = the
voice's note pitch incl. glide, bend and the "Pitch" destination (not Osc A's offsets / Osc A Pitch
modulation) - 12 or - 24 semitones; mono, same signal on L and R; level linear (`subLevel` + Sub Level
modulation, clamped 0..1).

### 6.3 NoiseSource (`NoiseSource.h`)

xorshift32 per voice (seed from the engine: voice index), uniform [-1, 1); pink = Paul Kellet's economy filter
(3 one-poles), colour c blends `(1 - c) white + c pink` with RMS matched; mono on L and R; level linear.
`reset()` reseeds (reproducible).

## 7. Filter (`SvfFilter.h`)

TPT state-variable filter (topology and equations in the header), stereo per voice.

- Modes LP12, LP24 (two LP stages, Butterworth Q at resonance 0, resonance on stage 2), BP (unity peak), HP,
  Notch. Q = 0.7071 * (25 / 0.7071)^r (LP24 stage 2: 1.3066 * (25 / 1.3066)^r; stage 1 fixed 0.5412).
- Cutoff (Hz) = `filterCutoff * 2^((keytrack * (note - 60) + envAmount/100 * 60 * env2 + cutoffMod) / 12)`
  with keytrack = `filterKeytrack / 100`, the pitch of the note incl. glide and bend; clamped 20 Hz ..
  0.49 fs. The voice smooths the cutoff in log2 per tick (this is the "Cutoff geglättet"; the 20 ms parameter
  smoothing comes on top).
- Drive (dB, + Filter Drive mod, clamped 0..24): `x' = s (G x) / sqrt (G)`, `s (u) = u / sqrt (1 + u^2)`;
  0 dB bypasses the stage exactly.
- Per tick: g = tan (pi fc / fs), k = 1 / Q for the tick end; per-sample linear ramp of g and k.
- Mode change at the tick, states kept. `filterOn` off: the stage is skipped (drive too), states cleared when it
  is switched on again.
- NaN / Inf: states reset, `getNanResetCount()` increments; |state| < 1e-20 -> 0.

## 8. Envelopes and LFOs

### 8.1 Envelope (`Envelope.h`)

Exponential ADSR in incremental form (formulas in the header): attack overshoot target 1.5 (concave, ends at
exactly 1 at time A), decay / release reach -80 dB of their distance at exactly D / R and then snap. Each
`advance (settings, n)` multiplies the distance to the segment target by `exp (-n c)` with c from the CURRENT
times (a closed form in "time since segment start" would jump when a smoothed / automated time changes
mid-segment); it handles segment ends inside n exactly, so segment times are exact to the sample for any
tick split. Cache `exp (-16 c)` per envelope while the time is unchanged (budget 18). noteOn / noteOff restart from the caller's current value (no jumps). Env 1 drives the amp
(`gain = env1 * (1 - ampVelocity/100 * (1 - velocity/127))`), Env 2 the filter (`filterEnvAmount`) and the mod
matrix, Env 3 only the mod matrix. A voice is inactive once Env 1 is idle (and no steal fade runs). Legato:
no restart (all three).

### 8.2 LFO (`Lfo.h`)

Global (two per engine), evaluated per tick, output -1..1 (shapes in the header). Rate: Hz, or tempo sync
`bpm / 60 / tempoDivisionQuarterNotes (division, quarterNotesPerBar)` with the host tempo (120 if unknown),
times `2^(rate mod)`, ±4 octaves at 100 %. **Bars follow the host time signature** like the capture's bars
(`barLengthOf` in `CaptureTap.cpp`): `SynthTransport::quarterNotesPerBar = numerator * 4 / denominator`, 4 if the
host gives none (decision for ROADMAP open point, 22). The processor fills `SynthTransport` from the play head
once per block (bpm, time signature). Free-running phase (not locked to the song position). `retrigger()` is
called by the engine for every envelope-starting note start (start / steal commands, not legato /
correction); it takes effect at the next tick: phase 0 there if `lfoNRetrig` is on; the fade-in (`lfoNFade`
ms, linear 0 -> 1) restarts there in any case. S&H / smooth random: xorshift32 seeded in `prepare()`.

## 9. Mod matrix (`ModMatrix.h`)

### 9.1 Sources (length frozen from 0.4.0; the planned sources are reserved, 24.4)

| Source | Value | Scope |
|---|---|---|
| Off | 0 | – |
| Env 2, Env 3 | 0..1 | voice |
| LFO 1, LFO 2 | -1..1 | global |
| Velocity | velocity / 127 | voice |
| Note | clamp ((key - 60) / 60, -1, 1) | voice |
| Random | uniform -1..1, new at every start, steal and legato command, kept by a correction | voice |
| Voice Level, Voice Brightness, Voice Noisiness | 0..1 (16) | global |
| Voice Pitch | pitchCents / 100, -1..1 | global |
| Vowel X, Vowel Y (reserved for 0.5), Vowel Weight 1..3 (reserved for 0.6) | 0 in 0.4 | global |

### 9.2 Destinations and scaling (length frozen from 0.4.0, 24.4)

Amount ±100 % times the source value times the scale below; summed per destination; never written back into a
parameter.

| Destination | at +100 % and source 1 | applied as |
|---|---|---|
| Osc A/B Position | +1 (whole table) | added to position fraction, clamped 0..1 |
| Osc A/B Pitch | +12 semitones | added to the oscillator pitch |
| Osc A/B Level, Sub Level, Noise Level | +1 | added to the knob's 0..1, clamped 0..1 |
| Osc A/B Detune | +100 cents | added, clamped 0..200 |
| Filter Cutoff | +60 semitones (5 octaves) | in the cutoff exponent (7) |
| Filter Resonance | +1 | added, clamped 0..1 |
| Filter Drive | +24 dB | added, clamped 0..24 |
| Amp Level | +1 | amp gain *= clamp (1 + m, 0, 2) |
| Pan | +1 | voice pan (after the filter, equal power), clamped -1..1 |
| LFO 1/2 Rate | +4 octaves | rate *= 2^m |
| Chorus / Delay / Reverb Mix | +1 | added to the mix fraction, clamped 0..1 |
| Pitch (appended by the critique) | +12 semitones | added to the voice's note pitch like the bend: Osc A, Osc B, sub and filter keytrack follow (one slot for vibrato; Voice Pitch -> Pitch at 8.33 % = 1:1 cents) |

### 9.3 Evaluation

Per voice, per tick: per-voice sources of that voice + global sources -> `computePerVoice` (all non-global
destinations). Global destinations (LFO rates, FX mixes, `isGlobalDest`): at step 4 of the tick with the global
sources and the per-voice sources of the newest voice (0 if none), `computeGlobal`. All modulation is ramped
over the tick like every other target.

## 10. Voices

### 10.1 VoiceAllocator rules (`VoiceAllocator.h`, binding)

- Groups: all note events of one absolute sample, in event order; voice activity is passed in (the engine has
  rendered up to that sample).
- Mono / Legato: voice 0 only; note stack (16, last-note priority). A note-off that empties the stack is
  deferred to the end of the group; a note-on at the same sample cancels it -> **connected change**. This makes
  both VoicePipeline orders (retrigger: off + on; legato: on + off at one sample) connected changes.
  - connected change: Legato -> `legato` command; Mono -> `start` (retrigger from the current level);
  - note-off of the sounding note while others are held: back to the newest held note (same as a connected
    change, never a correction);
  - note-on while silent or releasing: `start`.
- Poly (16): note already on a voice (held or releasing) -> `start` on that voice; else lowest inactive voice;
  else steal (oldest release start first, then oldest note-on) -> `steal` (2 ms linear fade, then start; a
  second steal during the fade replaces the pending note, which then has no voice: its note-off is ignored).
  With the monophonic pipeline every note change overlaps the old note's release on a second voice (both
  orders: the old voice is still active when the new note-on is allocated).
- `releaseAll()`: `release` for every voice holding a note (flush, the bypass fade, play-mode change); the
  engine's bend goes back to the centre with it (24.4 R1-1).
- Invariants: <= 16 voices, every started note gets a release on its last note-off, unknown note-offs ignored,
  never more than `VoiceCommandList::capacity` (64) commands per group.

### 10.2 Glide and the provisional-note correction

- Glide: constant time `glideTime` ms, linear in semitones from the current pitch (mono: the voice's own
  current, possibly still gliding pitch; poly: the most recent note-on's note), evaluated per tick. Always: on
  every start / legato / steal once a previous note exists since `reset()`; Legato Only: on connected changes
  only. glideTime 0: no glide (`AllocatorSettings::glideEnabled` comes from the latched, unsmoothed
  `glideTimeMs > 0`; the glide itself uses the smoothed time).
- **Correction rule (<= 40 ms):** a connected change caused by a note-on whose time is within
  `correctionWindowSeconds = 0.040` (absolute samples, rounded down) of the replaced note's own note-on, or
  whose event carries `EventTimingFlags::correction`, is a `correction`: pitch jumps (no glide, also when a
  glide is running), envelopes, velocity and the random value continue, the LFO is not retriggered. Applies in
  Mono, Legato and Poly. Poly pairing: each note-on of a group is paired, in event order, with the earliest
  unpaired note-off of the same group whose note held a voice; a pair that meets the rule is a correction on
  the old note's voice (no release, no second voice); unpaired note-ons are normal starts (0.6 chords).
- Answer to ROADMAP 3.2 ("reichen die `VsEventTiming`-Infos?"): yes for the pipeline — every provisional
  correction carries the `correction` flag and happens 10–30 ms after the provisional note-on. The time rule is
  still the primary rule so that the test pattern, flushes and any future source behave the same, and so the
  synth never depends on the capture side output; the flag extends the window when a correction comes later
  (Bass / Wide window ~40 ms).

### 10.3 SynthVoice (`SynthVoice.h`)

Per voice: Osc A, Osc B, Sub, Noise -> (drive + SVF if `filterOn`) -> amp (Env 1 x velocity x `ampGain` x Amp
Level mod, per-sample ramp) -> voice pan -> added to the mix. Pitch in semitones: note + bend + glide offset; osc pitch adds
`octave * 12 + semi + fine / 100 + pitch mod`; increment = `440 * 2^((pitch - 69) / 12) / fs`. Osc on/off:
`oscXOn` off -> the oscillator is not rendered (phases frozen; switched on mid-note it continues from them, its
gain ramps up over one tick). Steal: linear fade to 0 over `stealFadeSeconds`, then a clean start of the
pending note. Inactive voices cost nothing.

### 10.4 Gain staging

Osc level 70 %, unison sum `1/sqrt(n)`, one oscillator: about -3 dBFS peak for a saw before the amp; synthVolume
-6 dB default; poly chords of 3 notes stay below 0 dBFS at defaults. No limiter in 0.4 (presets keep peak
<= -1 dBFS in the golden test). `ampGain` (-24..+12 dB, preset parameter) is the patch level: presets are
balanced with it (a heavily filtered preset can be raised), `synthVolume` stays the user's mix control that no
preset touches. Output exactly 0 when all voices are inactive and all FX tails ended.

## 11. FX (global, after the voice sum; `fx/*.h`)

Order Chorus -> StereoDelay -> FdnReverb -> synthVolume. Each: on/off with a 10 ms linear crossfade (state
cleared when fully off; `isBypassed()` = nothing to do), mix with the dual-linear law
`out = dry * min (1, 2 (1 - m)) + wet * min (1, 2 m)` (dry stays at full level up to 50 %), parameters ramped per
tick, buffers allocated in `prepare()` for the largest time at the prepared rate (any rate; 192 kHz: delay
2 x 1.5 MB, reverb ~1.2 MB, chorus ~45 KB).

### 11.1 Chorus
Two delay lines (7 ms, 11 ms) fed by the mono sum, linear-interpolated reads modulated by one sine (left /
right 90° apart), depth 0..100 % = 0..4 ms, rate 0.05..5 Hz, no feedback; line 1 -> L, line 2 -> R.

### 11.2 StereoDelay
Time 1..2000 ms or synced (division x 60 / bpm, clamped to 2000 ms); read position slews at most 1 sample per
sample (tape-like time changes); feedback 0..95 % through HP 120 Hz + LP 6 kHz (one-poles) and the soft limiter
`u / sqrt (1 + u^2)`; ping-pong: mono sum into L, L -> R -> L cross-feed; else L -> L, R -> R. Tail end: input 0
and line peak < 1e-6 for one delay time -> lines cleared, exact dry output.

### 11.3 FdnReverb
Pre-delay 10 ms, 4 series all-pass diffusers (g 0.6), 8 lines (29.7 … 79.1 ms x (0.5 + size)), Householder
feedback `y_i = x_i - (sum x) / 4`, per-line gain from RT60 = 0.3 · (10 / 0.3)^size s, one-pole damping in every
line 18 kHz -> 1.5 kHz (exponential in damping), read modulation ±0.3 ms at 0.1..0.7 Hz, outputs L = even lines,
R = odd lines with alternating signs, / sqrt (8). Tail end as the delay (4 x the longest line). NaN guard.

### 11.4 Common
Denormals: the processor runs inside `juce::ScopedNoDenormals`; the core also zeroes feedback states below 1e-20
and clears lines at tail end (no CPU rise in silence, also in the JUCE-free tests).

## 12. Wavetable store and worker

### 12.1 Store protocol (`plugin/WavetableStore.h`, binding)

```
 worker/message:  old = slots[s].exchange (new, seq_cst); retired.push ({old, retireBlock = blockCounter.load (seq_cst)})
 audio, EVERY block start (also processBlockBypassed):  engine.setWavetable (s, slots[s].load (seq_cst))   (store.acquire)
 audio, EVERY block end:  inUse[4] = engine.getTablesInUse (A / B) (release);  blockCounter.fetch_add (1, seq_cst)
 collectGarbage():    free retired r when blockCounter.load (seq_cst) >= r.retireBlock + 2 and r.table not in inUse[]
```

Why it is safe: retireBlock = N is read after the exchange, so block N + 1 starts after it and (seq_cst
total order) loads the new pointer; when the counter reaches N + 2, block N + 1 has ended and reported
everything the engine still reads - including the old table while a 5 ms crossfade runs. Only then is "not
in inUse" meaningful; each inUse entry is written at the end of a block >= N + 1, and once the engine stops
reading the old table it never reads it again (it only gets tables through acquire). seq_cst is required:
with acquire / release, a counter load that reads N does not order the exchange before the next block's slot
load (costs nothing on x86 loads; the exchange and the fetch_add are locked instructions anyway).

Rules that keep the argument true (binding for C, D and integration):
- The processor calls `setWavetable (s, acquire (s))` at the start and `release (…)` at the end of EVERY
  block, `processBlockBypassed` included. A table set in a block is applied at the next tick and replaced by
  the next block's `setWavetable` before that block's first tick, so it is only read in blocks that set it;
  `getTablesInUse` therefore reports only the voices' current / previous (union over all 16 voices, at most
  two pointers per oscillator because every voice gets the same `setTable` at the same tick).
- `SynthEngine::reset()` / `prepare()` forget every table pointer (engine, voices, oscillators: current,
  previous, queued). `releaseAllRetired()` (frees everything retired, clears inUse) is only called with the
  audio stopped AND right after an engine reset: `prepareToPlay` (`engine.prepare` first), `releaseResources`
  (`engine.reset()` first - without it the engine would keep a pointer to a freed table and crossfade from it
  in the next block), destructor.
- Retired entries are `shared_ptr`s: publishing a table that is still current elsewhere (ABA through
  sharing, e.g. basic-shapes -> pwm -> basic-shapes) only drops one reference. While a host does not call
  processBlock (suspended without releaseResources, e.g. FL's smart disable) the retired list waits; it is
  bounded by the user's table changes and the tables are shared.
The audio thread only does atomic loads / stores. Freeing = dropping the `shared_ptr` under the store mutex
on the worker / message thread. No `std::atomic<std::shared_ptr>`, no free on the audio thread. Stress test:
1000 publishes from the message thread while an audio thread renders (+ optional local TSan run).

### 12.2 Worker (`plugin/WavetableWorker.h`)

Two ways in (critique 0.4, 23 F5):
- **Synchronous** on the calling non-audio thread: `loadFactoryNow`, `loadEmbeddedNow`. Used by the processor
  constructor (default table, so a new instance never starts on the sine), `setStateInformation` (project load:
  the tables are published when it returns - offline render right after loading, state load while playing, and
  a `getStateInformation` right after a `setStateInformation` (clap-validator, FL undo / autosave) all see the
  project's tables) and `PresetManager::load`. A shared factory table costs nothing; a build <= 150 ms.
- **Asynchronous** on the worker thread (one `juce::Thread` per instance): `requestFactory`, `requestImport`,
  `requestEmbedded` - the UI's table menu and Import WAV..., so the message thread never waits for a file.
Per slot only the newest request counts: every request (sync or async) takes a per-slot sequence number; an
older queued request is dropped, a running build is cancelled via the builder's cancel flag, and a result is
published only if its number is still the newest (checked under the worker's lock right before `publish`).
`getRequestedReference (slot)` = reference + user data of the newest request (factory / embedded at request
time, import once the file is read and hashed) - SynthState saves THIS, not the last published slot.
Results: publish + status (`Status`: busy, last error per slot, completed counter) for the UI. Every 100 ms:
`collectGarbage()`. Import: bytes via `juce::File::loadFileAsData`, `WavReader`, `buildFromWav`, name = file
name, data = PCM16 frames, hash = `fnv1a64` -> `user:<16 hex>`. Embedded data: the hash is recomputed from the
data (a mismatching ref is ignored); `frames * 2048` values required, else broken -> default table. Failure:
the slot keeps its table (async) or gets the default table (sync), the UI shows the error.

### 12.3 Sharing and memory

Process-wide map key -> `weak_ptr<const Wavetable>` (`findShared` / `addShared`, mutex, never the audio
thread): `factory:<id>` and `user:<hash>`; a `user:` table is reused only when the shared data under that hash
holds the same PCM (the hash alone proves nothing, 24.4 R2-8). The default table (`factory:basic-shapes`) is additionally held by a
process-wide strong reference once built, so constructing instances (plugin scan, pluginval, the plugin tests'
hundreds of processors) builds it only once. Per instance at most 2 current + 2 crossfading + retired tables
(~4 x 23 MB worst case with two different 256-frame user tables mid-crossfade; typical: one or two 64-frame
factory tables, 5.8 MB each, shared by every instance).

## 13. State and migration

### 13.1 Layout

```xml
<PARAMS stateVersion="4">
  <PARAM id="..." value="..."/>            (all 130 parameters)
  <CAPTURE version="1" .../>               (unchanged)
  <SYNTH version="1" presetName="Init" presetModified="0" editorScale="100">
    <TABLE osc="0" ref="factory:basic-shapes" name="Basic Shapes"/>
    <TABLE osc="1" ref="user:0123456789abcdef" name="My Table.wav" frames="64" data="(base64 PCM16 LE)"/>
  </SYNTH>
</PARAMS>
```

- User table data stored once per hash (the second TABLE with the same ref has no `data`); <= 1 MB raw per
  256-frame table (~1.4 MB base64).
- `getStateInformation` (any non-audio thread): `apvts.copyState()`, remove CAPTURE and SYNTH children, append
  CAPTURE and SYNTH (`SynthState` from `WavetableWorker::getRequestedReference` per slot + user data +
  `PresetManager` name / modified + editor scale, all thread-safe accessors), set `stateVersion = 4`.
- `setStateInformation`: read CAPTURE and SYNTH, remove all of both (`SynthState::removeFromState`),
  `migrateState`, `replaceState`, apply capture, apply synth state: per slot a SYNCHRONOUS load
  (`loadFactoryNow` / `loadEmbeddedNow`; missing / broken -> default factory table), then preset name /
  modified (`PresetManager::setCurrent`, takes the snapshot), editor scale; `updateHostDisplay
  (programChanged)` as today.
- User TABLE without `data`: the data of an earlier TABLE with the same ref; else a shared table with that key
  (`findShared`, another instance has it); else the default factory table (the reference is lost; the UI
  shows "Table missing: <name>" via the worker status).
- `editorScale` stays in SYNTH for good (0.5's `UI` child holds only new UI state, e.g. the last page), so no
  0.5 migration is needed for it.
- Tolerant: missing / extra / broken SYNTH, TABLE, refs, data -> defaults; extra children of later versions are
  ignored.

### 13.2 stateVersion

Root property `stateVersion` = 4 from 0.4.0 (5 / 6 later). Missing = 0.3.x or older. Future migrations key on it.

### 13.3 Migration from 0.3.1 (synth stays off)

**Changed by the integration (24.1):** the parameter default of `synthOn` is **On**
(`params::synthOnForNewInstances`, the decision E4 / E5 value of a new instance), and `migrateState()` has one
special rule: a state without a `synthOn` PARAM (0.3.x or older) gets `synthOn` = off; every other missing
parameter gets its default as before. (The first design - default off, the constructor switching it on - failed
clap-validator's `param-default-values`: a parameter's value right after initialisation must equal its
default.) The core `SynthSettings {}` keeps `synthOn = false`. Consequences: host "reset to default" leaves the
synth on; an old project loads with the synth off and everything else unchanged (fixture test). Fixtures: `tests/data/state-0.3.1-fx.bin / .xml`,
`state-0.3.1-instrument.bin / .xml` = `getStateInformation()` of the v0.3.1 code (captured before the scaffold
added any parameter) with non-default voice, test, capture parameters and capture settings.
`StateFixtureTests.cpp`: every saved value unchanged, synthOn off even if it was on before loading, every synth
parameter at its default, CAPTURE equal, one CAPTURE child after re-saving. From now on one fixture per release
(0.4.0 adds `state-0.4.0-*.bin` with a SYNTH child and an embedded user table, captured from the finished 0.4.0
code by the integration agent).

## 14. Presets

### 14.1 Format `.vspreset`

```xml
<VocalSynthPreset version="1" name="Big Lead" category="Lead" author="..." tags="voice,bright">
  <PARAM id="oscAPosition" value="35.0"/>      (plain values, only params::isPresetParameter IDs)
  <TABLE osc="0" ref="factory:basic-shapes" name="Basic Shapes"/>
  <TABLE osc="1" ref="user:<hash>" name="..." frames="64" data="(base64 PCM16 LE)"/>
</VocalSynthPreset>
```

UTF-8, written with `juce::XmlElement::writeTo`; user folder `<Documents>/VocalSynth/Presets` (created on the
first save), file name = `juce::File::createLegalFileName (name) + ".vspreset"`, overwriting asks in the UI.
A preset with a user table embeds its data (as the state does); loading uses the same TABLE rules as 13.1.

### 14.2 Categories

`PresetCategory`: Init, Lead, Bass, Pad, Pluck/Keys, Vocal, FX (stored by name; unknown -> FX for user presets).

### 14.3 Factory presets (`plugin/FactoryPresets.cpp`, agent F)

Data in code: name, category, tags, table A / B ids, the values that differ from the defaults. 24 presets;
final names and order (agent F, `FactoryPresets.cpp`; mode Legato unless noted):

| # | Category | Name (tables A / B) | Idea (voice sources in bold) |
|---|---|---|---|
| 0 | Init | Init (basic-shapes / basic-shapes) | all defaults (saw at position 50 %, LP24 open, Legato) |
| 1 | Lead | Voice Lead (basic-shapes / saw-bright-dark) | warm saw, short glide, **Level + Brightness -> cutoff**, chorus, ping-pong delay |
| 2 | Lead | Supersaw Lead (basic-shapes / basic-shapes) | 7-voice supersaw + octave, **Brightness -> Osc B level** |
| 3 | Lead | PWM Lead (pwm / pwm) | slow PWM drift, delayed vibrato (LFO fade), **Level -> cutoff** |
| 4 | Lead | Sync Lead (sync-sweep / basic-shapes) | Env 3 -> sync position, **Brightness** sharpens |
| 5 | Lead | Brass Lead (basic-shapes / basic-shapes) | filter swell, **Level -> cutoff** |
| 6 | Lead | Whistle (basic-shapes / basic-shapes) | sine an octave up, glide, **Noisiness -> noise**, delayed vibrato |
| 7 | Bass | Sub Bass | sine + sub, **Level** adds growl |
| 8 | Bass | Reese Bass (basic-shapes / saw-bright-dark) | two detuned saws, **Level -> cutoff** |
| 9 | Bass | Acid Bass (Mono) | LP24 resonance + Env 2, glide, **Level + Brightness -> cutoff** |
| 10 | Bass | Pluck Bass (Mono) | short Env 2 decay |
| 11 | Bass | FM Bass (fm-2op / basic-shapes) | **Brightness -> FM position** |
| 12 | Pad | Warm Pad (Poly; saw-bright-dark / basic-shapes) | unison, slow attack, chorus + reverb (the heaviest preset, 21) |
| 13 | Pad | Choir Pad (Poly; choir / formant-vowel) | **Brightness -> vowel**, **Level -> amp** (small), **Noisiness -> noise** |
| 14 | Pad | Glass Pad (Poly; bell / harmonic-sweep) | long release, delay + reverb, **Brightness** -> shimmer |
| 15 | Pluck/Keys | Poly Pluck (Poly) | short amp envelope, delay |
| 16 | Pluck/Keys | FM E-Piano (Poly; fm-2op / basic-shapes) | bright attack that softens |
| 17 | Pluck/Keys | Drawbar Organ (Poly; organ / basic-shapes) | fast chorus, **Brightness -> registration** |
| 18 | Vocal | Vowel Morph (formant-vowel / saw-bright-dark) | **Brightness -> Osc A position** |
| 19 | Vocal | Talking Saw | **Brightness + Level -> cutoff** (wah) |
| 20 | Vocal | Breathy Voice (formant-vowel / basic-shapes) | **Noisiness -> noise level** |
| 21 | Vocal | Vibrato Wah (basic-shapes / pwm) | **Voice Pitch -> cutoff** |
| 22 | FX | Space Drift (Poly; spectral-random / fold) | evolving pad, long delay, big reverb, **Noisiness -> noise** |
| 23 | FX | Glitch (digital / square-stack) | S&H -> position at 1/16, synced delay, **Noisiness -> delay mix** |

Changes against the draft list: Smooth Sine, Bright Saw, Square Vibrato, Riser dropped; Supersaw Lead, PWM Lead,
Brass Lead, Space Drift added; Reese / Acid / Pluck / Organ / E-Piano renamed (Reese Bass, Acid Bass, Pluck Bass,
Drawbar Organ, FM E-Piano, Poly Pluck). Loudness: -18 LUFS +-1 LU (max momentary, K-weighted, held C4 velocity
100, expression level 0.7 / brightness 0.5 / noisiness 0.2, synthVolume -6 dB, 48 kHz), Init exempt (about
-16.5); peak <= -1 dBFS incl. a 4-note chord on the Poly presets (hidden test `[.][presetmeasure]`). Listening
selection for the user (SYNTH-TESTING.md): Init, Voice Lead, Supersaw Lead, Reese Bass, Acid Bass, Warm Pad,
Choir Pad, Vowel Morph, Talking Saw.
At least 8 presets use voice sources (above: 9). Every preset: peak <= -1 dBFS, no DC, not silent (golden test).

### 14.4 Loading rules

Message thread; each preset parameter: `beginChangeGesture`, `setValueNotifyingHost (convertTo0to1 (clamped
plain))`, `endChangeGesture`; missing IDs -> default, unknown IDs ignored; tables synchronously through the
worker (`loadFactoryNow` / `loadEmbeddedNow`); never touches `synthOn`, `synthVolume`, `dryLevel`, voice, test
or capture parameters. Afterwards name = preset name, modified = false and a snapshot of the preset
parameters' plain values. `isModified()` = a raw value differs from the snapshot, or `markModified()` (the UI
changed a table) / a restored project said so. **No APVTS listener**: listeners run on the thread that sets the
parameter - the audio thread during host automation - and JUCE's `ListenerList::call` may allocate there.
`getCurrentName` / `isModified` / `setCurrent` are safe from any non-audio thread (internal mutex), because
hosts may call get/setStateInformation off the message thread.
CLAP: the processor calls `updateHostDisplay (programChanged)` after a preset load (integration). Prev / next
walk the full list (factory, then user) with wrap-around.

## 15. Parameters (`plugin/SynthParameters.h`)

102 parameters, version hint 4, appended in this order after `recordBars` by `addSynthParameters()` (called at the
end of `createLayout()`), total 130 (28 + 102). IDs permanent. Texts without unit (label = unit). "log" = exactly
logarithmic mapping, "skew c" = `setSkewForCentre (c)`.

| # | ID | Name | Type / range | Default | Label | Mapping |
|---|---|---|---|---|---|---|
| 1 | synthOn | Synth On | bool | **on** (= new instances; states without synthOn load off, 13.3) | | |
| 2 | synthVolume | Synth Volume | -60..6 | -6 | dB | linear; -60 = silent |
| 3 | dryLevel | Dry Level | -60..0 | 0 | dB | linear; -60 = silent; FX only |
| 4 | playMode | Play Mode | Mono, Legato, Poly | Legato | | |
| 5 | glideTime | Glide Time | 0..2000 | 0 | ms | skew 300 |
| 6 | glideMode | Glide Mode | Always, Legato Only | Legato Only | | |
| 7 | ampVelocity | Amp Velocity | 0..100 | 50 | % | |
| 8 | ampGain | Amp Gain | -24..12 | 0 | dB | linear; patch level (preset parameter, 10.4) |
| 9–19 | oscAOn, oscAPosition, oscAOctave, oscASemi, oscAFine, oscALevel, oscAPan, oscAUnison, oscADetune, oscASpread, oscAPhaseRand | Osc A … | bool; 0..100; int -3..3; int -12..12; -100..100; 0..100; -100..100; int 1..8; 0..100; 0..100; 0..100 | on, 50, 0, 0, 0, 70, 0, 1, 20, 50, 0 | ; %; oct; st; ct; %; %; ; ct; %; % | linear |
| 20–30 | oscB… (same 11) | Osc B … | same | **off**, 50, 0, 0, 0, 70, 0, 1, 20, 50, 0 | same | |
| 31 | subLevel | Sub Level | 0..100 | 0 | % | |
| 32 | subOctave | Sub Octave | -1 Oct, -2 Oct | -1 Oct | | |
| 33 | subShape | Sub Shape | Sine, Triangle, Square | Sine | | |
| 34 | noiseLevel | Noise Level | 0..100 | 0 | % | |
| 35 | noiseColor | Noise Color | 0..100 (white -> pink) | 0 | % | |
| 36 | filterOn | Filter On | bool | on | | |
| 37 | filterMode | Filter Mode | LP12, LP24, BP, HP, Notch | LP24 | | |
| 38 | filterCutoff | Filter Cutoff | 20..20000 | 20000 | Hz | log |
| 39 | filterResonance | Filter Resonance | 0..100 | 0 | % | |
| 40 | filterDrive | Filter Drive | 0..24 | 0 | dB | |
| 41 | filterKeytrack | Filter Keytrack | 0..100 | 0 | % | |
| 42 | filterEnvAmount | Filter Env Amount | -100..100 (= ±5 oct) | 0 | % | |
| 43–46 | env1Attack, env1Decay, env1Sustain, env1Release | Env 1 … | 0..10000, 0..10000, 0..100, 0..10000 | 5, 300, 100, 200 | ms, ms, %, ms | skew 500 |
| 47–50 | env2… | Env 2 … | same | 5, 300, **0**, 200 | | |
| 51–54 | env3… | Env 3 … | same | 5, 300, **0**, 200 | | |
| 55–59 | lfo1Shape, lfo1Rate, lfo1Sync, lfo1Retrig, lfo1Fade | LFO 1 … | 7 shapes; 0.01..30 Hz log; 15 divisions; bool; 0..10000 ms skew 1000 | Sine, 5, Off, off, 0 | ; Hz; ; ; ms | |
| 60–64 | lfo2… | LFO 2 … | same | Sine, **0.5**, Off, off, 0 | | |
| 65–88 | modNSource, modNDest, modNAmount (N = 1..8) | Mod N … | 12 sources; 22 dests; -100..100 | Off, Off, 0 | ; ; % | |
| 89–92 | chorusOn, chorusRate, chorusDepth, chorusMix | Chorus … | bool; 0.05..5 Hz log; 0..100; 0..100 | off, 0.8, 50, 50 | | |
| 93–98 | delayOn, delayTime, delaySync, delayFeedback, delayPingPong, delayMix | Delay … | bool; 1..2000 ms log; 15 divisions; 0..95; bool; 0..100 | off, 375, Off, 35, off, 25 | | |
| 99–102 | reverbOn, reverbSize, reverbDamping, reverbMix | Reverb … | bool; 0..100; 0..100; 0..100 | off, 50, 50, 25 | | |

Choice lists (length frozen from 0.4.0, 24.4): LFO shapes Sine, Triangle, Saw Up, Saw Down, Square, S&H, Smooth Random; divisions
Off, 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2, 1 Bar, 2 Bars, 4 Bars (a bar follows the
host time signature, 4 quarter notes if the host gives none, 8.2); sources and destinations as in 9.1 / 9.2. Value texts: integers for %, cents, ms >= 100, Hz >= 100;
one decimal for dB and ms < 100; two decimals for Hz < 10. Not parameters: table choice (SYNTH state), preset
name. `isPresetParameter()`: all synth parameters except synthOn, synthVolume, dryLevel (99).

Range check (critique): every range is final for 0.4 (saved automation): glide 2 s, envelope times 10 s, LFO
0.01..30 Hz, delay 1..2000 ms (synced times are clamped to it), detune 0..100 ct total spread, drive 0..24 dB,
cutoff 20 Hz..20 kHz, amp gain -24..+12 dB. **Choice lists are frozen in length from 0.4.0** (review 0.4, 24.4:
host automation stores index / (n - 1), so a longer list moves every saved automation point); entries planned for
later releases are reserved now (mod sources Vowel X / Y, Vowel Weight 1..3: 0, shown dimmed with their release);
later additions need a new parameter. (Before 0.4.0 ModDest got "Pitch" appended.)

## 16. Expression

### 16.1 Frame and buffer (`core/include/vocalsynth/ExpressionFrame.h`)

`ExpressionFrame { sampleOffset, level, brightness, noisiness, pitchCents }` (0.5 appends vowelX, vowelY),
`ExpressionBuffer` (capacity 512 > 257 = frames of a 4096 chunk at 8 kHz), `restExpression (noteSounding)`,
`ExpressionPublisher` (4 relaxed atomics for the UI meters).

### 16.2 ExpressionTracker (`ExpressionTracker.h`)

Once per analysis hop (2 ms) with the Framer window, the processed `PitchFrame`, the sung note (without octave
shift) and the gate; formulas in the header: level from rmsDb between gate and -6 dBFS (attack 5 ms / release
50 ms), brightness = log-normalised spectral centroid 200 Hz .. 5 kHz (512-point Hann FFT every 2nd hop),
noisiness = 0.5 flatness' + 0.5 (1 - confidence), pitch = cents from the note centre ±100; brightness /
noisiness / pitch smoothed 10 ms; brightness and noisiness 0 while level is 0.

### 16.3 VoicePipeline overload (agent C adds it to `VoicePipeline.h/.cpp`, nothing else changes)

```cpp
template <std::size_t N>
void process (const float* mono, int numSamples, const VoiceSettings& settings, VsEventBuffer<N>& out,
              VsEventTimingBuffer<N>& timing, ExpressionBuffer& expression) noexcept;
void processToSink (const float* mono, int numSamples, const VoiceSettings& settings, VsEventSink out,
                    VsEventTimingSink timing, ExpressionBuffer* expression) noexcept;
```

The existing overloads forward with `expression == nullptr`; events and timings are bit-identical with and
without the buffer (existing tests unchanged + a new comparison test). Agent C may move the body of the
existing `processToSink (…, timing)` into the new function (the old one forwards) and adds the private
`ExpressionTracker` member and its `prepare` / `reset` calls; nothing else in `VoicePipeline` changes. One frame per analysis hop, pushed with
the hop's event offset (the input sample that completed the hop); the tracker runs only when a buffer is given
(no CPU cost for the old overloads). `reset()` also resets the tracker; a frame is pushed for every hop even
when unvoiced (level falls to 0). The pipeline prepares the tracker in `prepare()`.

### 16.4 Engine side and Test Pattern

The engine latches frames at the first tick at or after their absolute position (newest wins), then ramps
linearly from its current value to the frame over the distance to the previous frame (clamped 16..4096 samples,
first frame after `reset()`: 16). `expression == nullptr` (Test Pattern): `restExpression (any note held)` — level 1 while a note is held,
everything else 0, so presets stay audible; the rest level is smoothed per tick like the tracker's level
(attack 5 ms, release 50 ms) so Voice Level -> Amp neither clicks nor cuts the release. Voice mode before the
first frame after `reset()`: all 0. Preset note (F): Voice Level falls with 50 ms when the singer stops, so
Voice Level -> Amp shortens release tails; prefer Voice Level -> Cutoff / Amp with a small amount.

### 16.5 UI

The processor publishes `engine.getCurrentExpression()` once per block to its `ExpressionPublisher`; the Synth
page shows four meters at 30 Hz (Voice section). Only meaningful in Voice mode (Test Pattern: rest values).

## 17. Processor integration (integration agent; `PluginProcessor.*`, `PluginEditor.*`)

Members (declaration order matters for destruction: worker before store, engine before store):
`WavetableStore wavetableStore; synth::SynthEngine synthEngine; SynthSettingsBuilder synthSettings;
ExpressionBuffer expressionFrames; ExpressionPublisher expressionPublisher; WavetableWorker wavetableWorker;
PresetManager presetManager; std::vector<float> synthLeft, synthRight (maxChunkSamples, constructor);
DspLoadMeter synthLoad;` + accessors for the editor (`getPresetManager`, `getWavetableStore`,
`getWavetableWorker`, `getExpressionPublisher`). Constructor: see the list below.

`processBlock` additions (order):
1. `auto synth = synthSettings.build(); synth.bendRangeSemitones = voiceMode ? bendRangeSemitones (voiceSettings.bendRange) : 2;`
   `SynthTransport transport` from the same play-head read as the capture (`makeTransportInfo`): bpm (120 if
   none), `quarterNotesPerBar = numerator * 4 / denominator` (4 if none).
2. block start: `synthEngine.setWavetable (s, wavetableStore.acquire (s))` for s = 0, 1 - in EVERY block.
3. flush (pending, source change): also `synthEngine.releaseAll()`.
4. per chunk: Voice mode -> `voicePipeline.process (mono, n, voiceSettings, events, eventTimings, expressionFrames)`
   (expressionFrames cleared per chunk); then dry: FX and not muteDry -> buffer *= dryGain (dB -> gain, -60 -> 0,
   per-sample linear ramp over 20 ms when the target changes; at 0 dB the samples are untouched = bit-identical
   to 0.3.1), else cleared as today; `synthEngine.process (events, voiceMode ? timings : {}, voiceMode ?
   &expressionFrames : nullptr, synth, transport, synthLeft, synthRight, n)`; add into the output (mono layout:
   (L + R) / 2); then the monitor tone as today.
5. block end: `wavetableStore.release (synthEngine.getTablesInUse (0), synthEngine.getTablesInUse (1))`;
   `expressionPublisher.publish (synthEngine.getCurrentExpression())`.
- Instrument: the analysis input stays the sidechain (decision E5): the synth plays from the sidechain mic; the
  dry sidechain is never in the output; `dryLevel` / `muteDry` have no effect.
- FX on the mic insert: unchanged path, the synth is added after the dry gain.
- `processBlockBypassed`: as today + `setWavetable (acquire)` for both slots, a 10 ms fade-out of a sounding
  synth on entering bypass, then `synthEngine.reset()` (nothing hangs; 24.4 R1-4) + `release` of the in-use tables (the block counter must keep running while bypassed, or
  retired tables pile up); Instrument output stays silent, FX passes the dry voice.
- `prepareToPlay`: `synthEngine.prepare (rate, maxChunkSamples)`, then `wavetableStore.releaseAllRetired()`;
  `releaseResources`: `synthEngine.reset()`, then `releaseAllRetired()` (12.1: never free with a stale engine
  pointer).
- Constructor: `synthOn = synthOnForNewInstances`, then `wavetableWorker.loadFactoryNow (s, "basic-shapes")`
  for both slots (synchronous, shared process-wide after the first instance).
- `getTailLengthSeconds()` stays 0 (the synth only sounds from MIDI that the plugin itself creates; FL keeps
  processing an effect on a mixer insert; reviewed and kept, 24.4 R1-5). Latency stays 0.
- State: 13.1. Presets: CLAP `updateHostDisplay (programChanged)` after a preset load.
- DSP display: analysis and synth separately (`synthLoad` measures the engine calls).
- Editor: fourth page "Synth" (`ui::SynthPanel`, tab after Voice / Test & Routing / Capture; the editor still
  opens on Voice in 0.4, page names unchanged because the editor tests compare labels with `FL-ROUTING.md` /
  `LIESMICH.txt`), `VsLookAndFeel` for the whole editor, design size 1100 x 700 (tab row 30 + page 670),
  fixed-aspect scaling 75–200 % (`setResizable`, constrainer with fixed aspect 1100 / 700, transform on a
  content component), scale menu 75 / 100 / 125 / 150 / 200 %, scale saved in SYNTH `editorScale` (atomic int
  in the processor, any-thread read by getStateInformation); the old pages keep their content and are laid
  out in the 1100 x 670 page area (centred, not stretched, where their layout has fixed sizes); existing
  editor tests move with the size change in the same step. `SynthPanelContext::dspLoadText` = the editor's
  `formatDspLoad` of `synthLoad`.

## 18. CPU budget (48 kHz, block 128, Release; 1 % = 26.7 µs per block)

| Module | Benchmark target | Share in "typical" | Share in "worst" |
|---|---|---|---|
| WavetableOscillator (Hermite, 2 frames = 8 reads) | <= 6 ns per unison voice per sample | 8 voices: 0.25 % | 256 voices: 7.4 % |
| Sub + Noise | <= 3 ns per voice per sample | 0.05 % | 0.5 % |
| SvfFilter LP24 stereo | <= 15 ns per voice per sample | 0.1 % | 1.2 % |
| Voice tick (env, glide, mod, coefficients, mip) | <= 300 ns per voice per tick | 0.1 % | 1.4 % |
| Global tick (LFOs, smoothing, global mod) | <= 1 µs per tick | 0.3 % | 0.3 % |
| Chorus / Delay / Reverb | <= 15 / 10 / 60 ns per sample | 0.4 % | 0.4 % |
| ExpressionTracker | <= 0.5 % | 0.3 % | 0.3 % |
| Total engine | | **mean <= 3 %, peak <= 6 %** | **<= 12 %** (estimate 11.5 %) |
| Silence (all inactive, tails ended) | | <= 0.5 % | |

Typical = Legato, 2 osc x 4 unison, LP24, chorus + delay + reverb. Worst = Poly 16 x 2 osc x 8 unison, all FX.

**Measured** (Release, clang, Linux VM; benchmarks of the implementers on a 2.8 GHz Xeon, processor level by the
integration on a 2.1 GHz Xeon; min of 9 runs for the micro benchmarks):

| Module / case | Target | Measured |
|---|---|---|
| WavetableOscillator, 8 voices, 2 frames, incl. tick | <= 6 ns per unison voice per sample | 5.5 ns; 7.9 ns during a table crossfade |
| Sub / white / pink noise | <= 3 ns per voice per sample | sub 4.8, white 2.3, pink 3.7, sub + noise 8.6 ns (target missed; both only render when their level > 0, so typical patches pay nothing) |
| SvfFilter LP24 stereo | <= 15 ns per voice per sample | 9–10 ns constant, ~13 ns while ramping; drive +5–6 ns |
| Voice tick (envelopes, glide, mod, coefficients, mip) | <= 300 ns | ~88 ns; envelope advance ~9 ns |
| Whole voice (render) | – | 1 osc x 1 LP24 33–35 ns/sample; 2 x 4 + sub + noise 118–126; 2 x 8 + sub + noise ~211 |
| Chorus / Delay / Reverb | 15 / 10 / 60 ns per sample | 7 / 11 / 26 ns (delay 1 ns over, accepted) |
| ExpressionTracker | <= 0.5 % | 2.1 µs per 2 ms hop (~0.1 %) |
| Engine typical (48 kHz / 128) | mean <= 3 % | 14.7 µs per block = 0.55 % |
| Engine worst (Poly 16 x 2 x 8 unison + sub + noise + LP24 drive + all FX) | <= 12 % | 218 µs per block = 8.2 % -> **no Poly unison cap** (`SynthEngine::polyMaxUnison` = 8) |
| Engine silence | <= 0.5 % | 2.7 µs = 0.10 % |
| 256-frame mip build (background) | < 150 ms | 51 ms; factory tables 7–50 ms each |
| Wavetable memory | – | 90,244 B per frame (32 frames 2.9 MB, 64 frames 5.8 MB, 256 frames 23.1 MB); sub tables 3 x 90 KB per process; all 14 factory tables ~63.5 MB if all were loaded (built on demand, shared) |

Whole `processBlock` (integration, `SynthProcessorTests` `[.cpu]`: FX, Voice mode, a sung phrase of three notes
with vibrato + 1 s silence, analysis + capture + synth, 48 kHz, block 128, 3 passes; percent of the block time):

| Case | mean | p99 |
|---|---|---|
| Synth off (the 0.3.1 path) | 0.47 % | 1.4 % |
| Init | 0.68 % | 1.8 % |
| Voice Lead | 0.98 % | 2.2 % |
| Supersaw Lead | 1.04 % | 2.2 % |
| Choir Pad | 1.16 % | 2.2 % |
| Warm Pad (heaviest preset) | 1.43 % | 2.7 % |
| typical settings (Legato 2 x 4, LP24, chorus + delay + reverb) | 0.98 % | 2.2 % |
| worst settings (Poly, 2 x 8 unison, sub, noise, drive, all FX, 4 s release) | 2.34 % | 4.2 % |

With the monophonic voice at most a few Poly voices overlap; the 16-voice worst case is the engine benchmark
above (8.2 %). All well below the 20 % limit for the whole processBlock.
Whole `processBlock` worst case < 20 % (unchanged). The voice-tick budget needs caching: unison detune
ratios and pan gains only when their inputs change (6.1), `exp` factors of the envelopes only when a time
changes (8.1), `tan` of the cutoff once per tick per voice (not per channel / stage). The SSE2 baseline has no
gather, so the table reads are scalar; the budget assumes that.

The worst case is an estimate with ~4 % margin, and MSVC usually vectorises less than clang. **Decision rule
(agent C, end of wave 2, before agent F records any golden fingerprint):** if "worst" exceeds 12 % in the
local benchmark, the Poly mode caps the unison count of each oscillator at 4 (`SynthEngine` constant, no
parameter, documented here by the integration agent); after F's goldens exist the cap must not change (it
changes the sound of presets). Benchmarks: Catch2 `BENCHMARK` with tag `[!benchmark]`; the implementers put
their numbers in their final reports (C: typical, worst, silence; A: oscillator per voice, 256-frame mip build
< 150 ms), the integration agent records them in this section.

## 19. File ownership

Phase 0 (this design agent) wrote every file below as a scaffold. From phase 1 on, only the owner edits a file.
**Nobody but the integration agent edits CMake files, `plugin/Parameters.h`, `plugin/SynthParameters.h`,
`PluginProcessor.*`, `PluginEditor.*`**; new test helpers that are headers need no CMake entry. No agent adds a
new `.cpp` (it would need a CMake entry): helpers go into the owner's existing `.cpp` files (anonymous
namespace) or into header-only files next to the owner's tests.

| Agent | Files (headers `core/include/vocalsynth/...`, sources `core/src/...`, tests `tests/...`) |
|---|---|
| A Wavetable | `synth/RealFft`, `synth/Wavetable`, `synth/WavetableBuilder`, `synth/FactoryWavetables`, `synth/WavReader`, `synth/WavetableOscillator`, `synth/SubOscillator`, `synth/NoiseSource` (.h + .cpp); tests `core/synth/{RealFft,Wavetable,WavetableBuilder,FactoryWavetables,WavReader,WavetableOscillator,SubOscillator,NoiseSource}Tests.cpp`, `core/synth/SynthTestUtils.h` (shared helpers: others read, ask A to add) |
| B Voice | `synth/SvfFilter`, `synth/Envelope`, `synth/Lfo`, `synth/ModMatrix`, `synth/SynthSettings`, `synth/SynthVoice`, `synth/VoiceAllocator`; tests `core/synth/{SvfFilter,Envelope,Lfo,ModMatrix,SynthSettings,SynthVoice,VoiceAllocator}Tests.cpp` |
| C Engine + FX + Expression | `synth/SynthEngine`, `synth/fx/{Chorus,StereoDelay,FdnReverb}`, `ExpressionFrame.h`, `ExpressionTracker`, the new `VoicePipeline` overload only (16.3, in `VoicePipeline.h/.cpp`); tests `core/synth/{SynthEngine,Chorus,StereoDelay,FdnReverb}Tests.cpp`, `core/{ExpressionFrame,ExpressionTracker,VoicePipelineExpression}Tests.cpp` |
| D Plugin data | `plugin/{SynthSettingsBuilder,WavetableStore,WavetableWorker,SynthState,PresetManager}.{h,cpp}`; tests `plugin/{SynthSettingsBuilder,WavetableStore,WavetableWorker,SynthState,PresetManager,SynthParameter,StateFixture}Tests.cpp`, `tests/data/state-0.3.1-*` |
| E UI | `plugin/ui/*` (Theme.h, VsLookAndFeel, Knob, ChoiceChip, SectionPanel, WavetableView, PresetBar, SynthPanel); tests `plugin/{SynthPanel,UiFoundation}Tests.cpp` |
| F Sound / presets | `plugin/FactoryPresets.{h,cpp}` (data only; `PresetCategory` frozen), `tests/plugin/{FactoryPresets,GoldenPreset}Tests.cpp`, `tests/data/golden/presets-0.4.json`, `docs/SYNTH-TESTING.md` (German) |
| Integration | `PluginProcessor.*`, `PluginEditor.*`, `Parameters.h`, `SynthParameters.h`, `CMakeLists.txt`, `core/CMakeLists.txt`, `tests/CMakeLists.txt`, `cmake/*`, `tests/plugin/SynthProcessorTests.cpp` (new, processor + synth), `tests/data/state-0.4.0-*`, existing tests that the integration changes (editor size, parameter counts: `PluginEditorTests`, `ProcessorTests`, `CaptureParameterTests`), README / STATUS / this document (incl. the benchmark numbers of 18) |

Waves (at most 2 agents at a time, own build directory each): 1 = A + B, 2 = C + D, 3 = E + F, 4 = integration.
Order inside wave 1 (dependencies): A delivers `WavetableBuilder::makeSineTable`, `WavetableOscillator`,
`SubOscillator`, `NoiseSource` first (B's voice renders through them; the class members cannot be swapped for a
test oscillator), then RealFft / builder / WavReader / factory tables. B does `SvfFilter`, `Envelope`, `Lfo`,
`ModMatrix`, `SynthSettings`, `VoiceAllocator` first and the audio-level `SynthVoice` tests last; if A's
oscillators are not in the tree by then, B keeps the logic tests (source values, activity, commands) and
reports the audio tests as open for a short follow-up run. C's engine tests need A and B (wave 2); D's worker
needs A's builder (wave 2); E needs D's PresetManager / store / worker (wave 3); F's golden test needs C's
engine and D's `SynthSettingsBuilder::fromPlain` (wave 3). Shared test helpers: `SynthTestUtils.h` is A's; other
agents keep private helpers in an anonymous namespace of their own test files.
Requests for changes to files of another agent go into the final report, not into the file.

## 20. UI foundation (agent E, `plugin/ui/`)

### 20.1 Theme (`Theme.h`)
Colour, size and font tokens (dark theme, accent `#4fc3f7`, modulation `#ffb74d`); every widget uses only these.
System font (no embedded font in 0.4). Vector drawing only (HiDPI-sharp).

### 20.2 VsLookAndFeel
`LookAndFeel_V4` subclass: rotary sliders (track arc, value arc, pointer, modulation ring from the slider
properties `modStart` / `modEnd`), linear sliders, buttons, toggles, combo boxes, popup menus, tabs, labels.
Applied to the whole editor (old pages only change colours / shapes).

### 20.3 Widgets
`Knob` (rotary + label + value text, double-click default, shift = fine, wheel; componentID = parameter ID),
`ChoiceChip` (choice: popup of the choices; bool: toggle; componentID = parameter ID), `SectionPanel` (titled
rounded frame, optional header component such as the oscillator's on switch).

### 20.4 WavetableView
2D frame at the current position (+ position marker; level 0 = 8192 samples, decimated to the view width),
path cached, rebuilt when the table changes (any `setTable` with another table, 24.4 R2-5) or the position
changes (> 0.5 %), 30 Hz from the panel's timer (only while the Synth page is shown, 24.4 R2-4); table from
`WavetableStore::getSlot` (`shared_ptr`, never the audio pointer), redrawn when `getGeneration()` changes.
3D stack in 0.5.

### 20.5 PresetBar
[<] [name * — menu by category, User, Init] [>] [Save] (async AlertWindow: name + category). IDs `presetPrev`,
`presetName`, `presetNext`, `presetSave`. The current user preset is PresetManager's tracked file (name only after a
project load); the 10 Hz refresh runs while the owner shows the bar (`setRefreshing`; 24.4 R2-4, R2-6).

### 20.6 SynthPanel (1100 x 670 design pixels)

```
 +--------------------------------------------------------------------------------------------------+
 | PresetBar ......................................... | Synth On | Synth Vol | Dry | DSP %         |  30
 +------------------------------+------------------------------+--------------------+--------------+
 | OSC A [on] table v  view     | OSC B [on] table v  view     | SUB / NOISE        | VOICE        | 230
 | pos oct semi fine lvl pan    | (same)                       | lvl oct shape      | mode glide   |
 | uni det spread phase         |                              | noise lvl color    | gmode vel    |
 |                              |                              |                    | 4 meters     |
 +------------------------------+-------------+----------------+--------------------+--------------+
 | FILTER [on] mode cut res drive key env     | ENV 1 (Amp) A D S R gain | ENV 2 | ENV 3            | 150
 +--------------------------------------------+---------------------+-----------------------------+
 | LFO 1 shape rate sync retrig fade | LFO 2   | MOD MATRIX 8 rows: source v  dest v  amount      | 260
 +-----------------------------------+---------+--------------------------------------------------+
 | FX: CHORUS on rate depth mix | DELAY on time sync fb pp mix | REVERB on size damp mix           |
 +--------------------------------------------------------------------------------------------------+
```

Table menu per oscillator: the 14 factory tables, separator, "Import WAV..." (async `FileChooser`, frame-size
option in a second step only when the file is ambiguous), the current user table; worker errors shown under the
view; a table change from the menu calls `PresetManager::markModified()`. Every synth parameter has exactly one
control on this page (test; `ampGain` = "Gain" knob in the ENV 1 section). DSP display: `dspLoadText` of the
context (hidden when empty). Timer 30 Hz: expression meters, wavetable position marker, worker status.

## 21. Test plan per module

Every module has its test file (19); tags `[synth]`, `[expression]`, `[plugin]`; benchmarks `[!benchmark]`.

| Module | Tests (all must pass; numbers are gates) |
|---|---|
| RealFft | forward / inverse round trip < 1e-12 rel; Parseval; impulse, DC, single bin; all sizes 16..16384 |
| Wavetable | layout constants (22561 per frame, lengths 8192 .. 1024), memory 256 frames <= 23.2 MB, level pointers disjoint, guard samples (p[-1], p[N], p[N + 1]) inside the frame |
| WavetableBuilder | per level no energy above maxHarmonic (< -120 dB rel. to the strongest bin), DC = 0, level-0 peak = 1 ±1e-4, deterministic (two builds bit-identical), thinning 300 -> 256, single cycle of 1000 / 1001 samples, `chooseFrameSize` table (override, clm, multiple of 2048, <= 8192 single cycle, longer -> 2048), resample 512 / 4096, PCM16 rebuild bit-identical, guard samples equal their sources; benchmark 256 frames < 150 ms |
| FactoryWavetables | 14 ids unique, `fromId` round trip, every table builds, frame counts <= 256, deterministic, each < 150 ms |
| WavReader | 8 / 16 / 24 / 32 PCM, float 32 / 64, extensible, stereo -> mid, `clm ` (Serum-style "<!>2048"), odd chunk padding, chunk order, multiples of 2048, truncated data, empty / header-only / random bytes (10 000 fuzz cases, no crash, clean error) |
| WavetableOscillator | f0 within ±0.1 ct (FFT peak) 20 Hz–8 kHz; **alias** (gate): saw sweep (>= 60 log-spaced f0 per rate) + every factory table at 44.1 / 48 / 96 kHz, all non-harmonic energy >= 60 dB below the fundamental up to f0 = 5 kHz and >= 70 dB up to 1 kHz - measure with f0 on an exact FFT bin (f0 = m fs / N, N = 65536, m odd) and a rectangular window, so harmonics and aliases fall on exact bins (no window leakage floor); unison detune distribution and spread; **click** (mip switch and position morph during a glide 40 Hz -> 10 kHz in 2 s; table change saw -> square at constant pitch): the test drives `beginTick` itself, builds an additive reference with the same phase accumulation (glide: all harmonics below 0.45 fs; table change: the ideal 5 ms linear crossfade), and in every 1024-sample Hann window (hop 256) the energy of render - reference in the bins below 0.2 fs is <= -60 dB relative to the reference's energy in that window; benchmark (<= 6 ns per unison voice per sample) |
| SubOscillator / NoiseSource | shapes and octaves (FFT), band limit; noise reproducible after reset, white flat / pink -3 dB/oct ±1 dB, RMS level |
| SvfFilter | LP12 / LP24 -3 dB at fc ±0.5 dB (100 Hz, 1 kHz, 10 kHz at 44.1 / 48 / 96 kHz), slopes 12 / 24 dB/oct ±1 dB, BP / HP / notch shape; max resonance + 24 dB drive + cutoff jumps 20 Hz <-> 20 kHz every tick for 10 s: finite and bounded; NaN input -> reset counter; drive 0 = linear |
| Envelope | segment times exact to ±1 sample for split sizes 1..16 and any offset; with constant settings the values match the closed forms within 1e-5; a decay / release time ramped during the segment changes the value continuously (no step > the per-tick step at the old time); retrigger / release from mid-segment without jumps; A/D/R = 0 |
| Lfo | shapes, rate, tempo sync at 90 / 120 / 174 BPM (period within 1 sample per cycle), bars with quarterNotesPerBar 4 / 3 / 3.5 and the fallback for 0 / NaN, retrigger takes effect at the next tick, fade-in, S&H determinism |
| ModMatrix | scaling of every destination (incl. the appended Pitch), summing of slots, off slots, global / per-voice split |
| SynthSettings | defaults == parameter defaults, sanitised clamps every field (incl. ampGainDb), NaN -> default, `tempoDivisionQuarterNotes` with a bar length |
| VoiceAllocator | random event sequences (10 000, all modes): no hanging voice, <= 16 active, every note-on gets its release; steal order; a second steal during the fade (the replaced note's note-off is ignored); last-note priority; deferred note-off (both pipeline orders = connected); correction at 39 / 41 ms and with the flag, Poly pairing (off + on of one group, chord without pairs); glide flags |
| SynthVoice | control protocol (command mid-tick == command at tick with the same samples: identical output), glide time, legato without restart, correction without glide, steal fade 2 ms, exact 0 after release |
| SynthEngine | **bit-identical** for blocks {1, 7, 64, 480, 512, 4096} x {44.1, 48, 96 kHz} with notes, bends, expression, glide, LFO (incl. retrigger, sync), all FX incl. long tails reaching the denormal / tail-end thresholds, synthOn off / on, a table change; allocation counter = 0 in process (as `CaptureTapTests`); silence -> exact 0 and no active voices after the tails; bend with bendRange 2 / 12 / 48, bend jumps at a start from silence; expression interpolation, rest expression smoothed; table swap during a note without click; reset() forgets the tables (getTablesInUse empty) and getTablesInUse covers silent voices; synthOn fade; benchmarks typical / worst / silence (18) |
| Chorus / StereoDelay / FdnReverb | mix law, on/off fade, bypass, delay time + sync, feedback bounded at 95 %, ping-pong, RT60 vs size ±20 %, tails end in exact 0, NaN guards |
| ExpressionFrame / Tracker | buffer capacity, publisher; level mapping vs rmsDb and smoothing times; brightness of a sine sweep monotonic; noise > tone in noisiness; pitch cents of a detuned tone |
| VoicePipeline overload | events + timings bit-identical with and without expression for the M2 test signals; one frame per hop; frame offsets block-size invariant |
| SynthParameters | 102 parameters after `recordBars`, hint 4, unique, defaults, choice lists, texts, `isPresetParameter` (done in the scaffold) |
| StateFixture | 0.3.1 FX / Instrument fixtures (done in the scaffold); later 0.4.0 fixture with SYNTH + embedded table |
| SynthSettingsBuilder | every parameter reaches its field (set each parameter to a non-default, compare); `fromPlain (toPlain (s)) == s.sanitised()`; `toPlain ({})` = the layout's defaults; `indexOf` |
| WavetableStore | retire protocol (fake audio thread: a table is freed only after 2 counted blocks and while not in use, also during a crossfade), stress 1000 swaps while rendering, releaseAllRetired, sharing by key, ABA (re-publishing a current table) |
| WavetableWorker | factory / import / embedded, sync loads published on return, newest request wins (a late async build never overwrites a later sync load), `getRequestedReference`, embedded hash recomputed, errors, paths with spaces and umlauts |
| SynthState | round trip incl. user table (data once per hash), broken / missing children and data, stateVersion |
| PresetManager | file round trip (incl. an embedded user table), missing / unknown IDs, never touches mix / voice / capture parameters, tables published when load() returns, modified flag (snapshot compare, markModified, setCurrent), name access from a second thread, prev / next wrap, user folder |
| FactoryPresets / Golden | 24 presets, IDs valid, values in range, >= 8 with voice sources, tables exist; golden fingerprint as defined in 21.1 vs `tests/data/golden/presets-0.4.json`; peak <= -1 dBFS, no DC, not silent, no NaN |
| SynthPanel / UI | both variants, every synth parameter has exactly one control, controls inside bounds and not overlapping, preset bar, meters; widgets attach to parameters |
| Processor (integration, `SynthProcessorTests.cpp`) | synth audible in FX and Instrument, dryLevel / muteDry, 0 dB dry bit-identical to 0.3.1, synth part bit-identical for host blocks {1, 7, 64, 480, 512, 4096}, allocation counter = 0 in processBlock with the synth playing, no hanging notes on bypass / flush / source change, tables published when setStateInformation returns, getState after setState equal, state round trip with user table, preset load sets no voice parameter, new instance has synthOn on, transport bars from the time signature |
| Validators (gates) | pluginval strictness 10 (GUI) both VST3, Steinberg validator, clap-validator both CLAP (known `state-invalid-random` warning only), with 130 parameters (check CI timeouts) and synthOn on for new instances |

### 21.1 Golden fingerprint (binding for agent F; precise so clang and MSVC agree)

- Render: `SynthEngine` directly, 48 kHz, `prepare (48000, 4096)`, `process()` in chunks of 512; settings =
  `SynthSettingsBuilder::fromPlain` of the defaults overwritten by the preset's values, `synthOn` on,
  `synthVolume` -6 dB, `bendRangeSemitones` 2; tables `FactoryWavetables::build` of the preset's ids, set before
  the first chunk; transport 120 BPM, 4 quarter notes per bar; no bends, no timings.
- Sequences (each from `reset()`, velocity 100, followed by a 1.0 s tail): **held** C4 (60) from 0 to 2.0 s;
  **legato** C4 E4 G4 C5 (60, 64, 67, 72), 0.25 s each, changes in the pipeline's legato order (note-on new,
  then note-off old at the same sample), last note-off at 1.0 s; **chord** (Poly presets only) C4 E4 G4 from
  0 to 1.5 s.
- Expression (every preset): one frame every 96 samples; level = min (1, t / 0.5 s) while a note is held, then
  linearly to 0 over 0.1 s; brightness = t / T (T = sequence length incl. tail); noisiness =
  0.5 - 0.5 cos (2 pi t / T); pitchCents = 30 sin (2 pi 5 Hz t).
- Features per non-overlapping 4800-sample window (100 ms): RMS of m = (L + R) / 2 in dBFS (floor -100);
  peak of max (|L|, |R|) in dBFS (floor -100); spectral centroid (Hz) and flatness (geometric / arithmetic mean)
  of m, Hann window, zero-padded to 8192 (`RealFft` order 13), bins 20 Hz .. 20 kHz, power floor 1e-20 per bin.
- Compare: RMS ±1.5 dB and peak ±1.5 dB in every window; centroid ±10 % and flatness ±0.05 only where the RMS
  is above -60 dBFS in both reference and render. Absolute checks: peak <= -1 dBFS, |mean| per channel over
  the render < 1e-3, at least one window above -40 dBFS, no NaN / Inf.
- `VOCALSYNTH_WRITE_GOLDEN=1` rewrites the JSON (Linux clang Release); every other run compares. Format:
  `{ "version": 1, "sampleRate": 48000, "window": 4800, "presets": [ { "name": "...", "held": [[rms, peak,
  centroid, flatness], ...], "legato": [...], "chord": [...] } ] }` (`chord` only for Poly presets).

## 22. Risks and open points

- Envelope `exp` per tick: cache the per-tick factors while the times are unchanged (8.1, 18).
- MSVC vs clang floating point differ: only fingerprints cross platforms; invariance tests run per build.
- The deferred note-off makes Legato connected for the pipeline's retrigger order — intended (the voice is
  monophonic). A user who wants retriggered envelopes picks Mono.
- Host "reset to default" keeps the synth on (default on since the integration, 13.3).
- 130 parameters in FL's wrapper list and pluginval run time: checked in the integration gate.
- Worst-case CPU has ~4 % margin (18): decision rule for the Poly unison cap at the end of wave 2.
- Memory: 23 MB per 256-frame user table (5.8 MB per 64 frames) - the price of the alias gate (5.1).

Decided by the critique (were open):
- **Mip-level crossfade: one tick is enough.** The levels are built from the same spectrum and read at the
  same phase, so a linear crossfade keeps every shared harmonic exactly constant; only the top octave of the
  richer level (>= 0.225 fs, >= 9.9 kHz at 44.1 kHz) fades, and a 16-sample ramp spreads it by at most
  fs / 16 (3 kHz) - nothing reaches the band below 0.2 fs where a click would be heard. The click test (21,
  WavetableOscillator) measures exactly that band; if it fails, the fix is in the level choice, not a longer
  fade (a longer fade at a rising pitch would let the richer level alias).
- **Bars follow the host time signature**, like the capture: `quarterNotesPerBar = numerator * 4 /
  denominator`, 4 when the host reports none (8.2). Costs one field in `SynthTransport`; "1 Bar" means the
  same in the synth and in the capture's bar-based takes.


## 23. Critique log (critique agent, 2026-11-03)

Review of this document and the scaffold against ROADMAP 3 / 4.1 and the 0.3.1 code (VoicePipeline,
NoteSegmenter event order, CaptureTap transport, processor / state code). Severity: **C** = the release gate
could not be met / memory safety, **H** = wrong behaviour in normal use, **M** = invariance, real-time or
interface gap that would have forced a frozen-interface change later, **L** = clarity / small gap. Every
finding is fixed in this document and, where named, in the scaffold.

| # | Sev | Finding | Resolution (sections, files) |
|---|---|---|---|
| F1 | C | Alias gate unreachable: 4x mip levels + linear interpolation leave -42 dB of non-harmonic energy at each level's worst f0 (gate -60 / -70 dB). Measured with saw 1/k tables at the 0.45 fs threshold of every level, f0 on an exact bin (N = 65536, rectangular window), sweeps 20 Hz–5 kHz at 44.1 / 48 / 96 kHz: 8x + linear -52, 16x + linear -61, 8x (min 512) + Hermite -62, **8x (min 1024) + Hermite -72 dB** | levels `clamp (8 H, 1024, 8192)`, 4-point Hermite, guards 1 before + 2 after (5.1, 6.1); `Wavetable.h`: `wavetableMipLength`, `wavetableLevelOffset`, `guardBefore` / `guardAfter` replace `guardSamples`, `frameStride` 22561; 23 MB per 256 frames; oscillator budget 6 ns (18) |
| F2 | C | Use-after-free path: `releaseResources` freed every retired table while the engine still held it as current / previous; the next block would crossfade from freed memory (also after `prepareToPlay` if `reset()` kept pointers, as the oscillator doc said) | `reset()` / `prepare()` forget every table pointer (engine, voice, oscillator; stubs updated); `releaseResources` = `engine.reset()` then `releaseAllRetired()`; `setWavetable` / `release` in EVERY block incl. `processBlockBypassed` (12.1, 17, `SynthEngine.h`, `WavetableOscillator.h`) |
| F3 | H | Store memory order: with acquire / release, "retireBlock read after the exchange => the next block loads the new pointer" does not follow from the C++ model | seq_cst on slot exchange / load and counter load / fetch_add (12.1, `WavetableStore.h/.cpp`) |
| F4 | M | A table change during a crossfade "restarted from the current table": the half-faded table was dropped (click) and three tables could be in use; silent voices kept stale `previous` pointers that `getTablesInUse` (active voices only) did not report | changes during a crossfade are queued (newest wins); silent oscillators switch at once; `restart()` ends a crossfade; `getTablesInUse` = union over all voices, <= 2 pointers (6.1, 12.1) |
| F5 | H | All table loads were asynchronous: after a project load the first blocks (and an offline render started at once) played the fallback sine, and a `getStateInformation` right after `setStateInformation` (clap-validator, FL undo / autosave) saved the OLD table references | `WavetableWorker::loadFactoryNow` / `loadEmbeddedNow` (synchronous; constructor, state load, presets), per-slot sequence numbers, `getRequestedReference` saved by SynthState (12.2, 13.1, 14.4, `WavetableWorker.h`) |
| F6 | M | Block-size dependence: the filter's NaN guard and `< 1e-20` flush ran "after each render() call", FX tail / bypass decisions were unspecified - segment ends depend on the host block, so the last bits differed between splits | every decision and flush at ticks / events only (3.5; `SvfFilter.h`, `Chorus.h`, `StereoDelay.h`, `FdnReverb.h`, `SynthEngine.h`); synthOn-off clears state at a tick and keeps the clock |
| F7 | M | Envelope closed form in "time since segment start": a smoothed / automated decay or release time jumped the value mid-segment | incremental form with the current times, closed form only as the constant-settings reference (8.1, `Envelope.h`) |
| F8 | M | LFO retrigger at a mid-tick note start was undefined (the LFO is tick-granular) | takes effect at the next tick (3.5, 8.2, `Lfo.h`) |
| F9 | M | `PresetManager` used an APVTS listener: during host automation it runs on the audio thread and JUCE's `ListenerList::call` may allocate; the name was read by `getStateInformation` from other threads without a lock | no listener: snapshot compare + `markModified()`, internal mutex, thread rules (14.4, `PresetManager.h/.cpp`) |
| F10 | M | No patch level: presets could only get quieter (osc level <= 100 %, `synthVolume` is not a preset parameter), so 0.4.1 could not fix "zu leise" presets without a new parameter | `ampGain` -24..+12 dB, default 0, preset parameter, after `ampVelocity` -> 102 synth parameters, 130 total, 99 preset parameters (10.3, 10.4, 15, `SynthSettings.h`, `SynthParameters.h`, `SynthParameterTests.cpp`) |
| F11 | L | Vibrato needed two slots (Osc A Pitch + Osc B Pitch) and the sub did not follow | `ModDest::pitch` appended ("Pitch", +12 st, whole voice; 9.2, `SynthSettings.h`, `ModMatrix.h`, `SynthParameters.h`) |
| F12 | L | Open point: bar = 4 quarters always, while the capture's bars follow the host time signature | decided: follow it; `SynthTransport { bpm, quarterNotesPerBar }` moved to `SynthSettings.h`; `tempoDivisionQuarterNotes (d, quarterNotesPerBar)`; `Lfo::advanceTick` and `StereoDelay::beginTick` take `const SynthTransport&` (8.2, 17, 22) |
| F13 | L | Open point: one-tick mip crossfade vs the click test | decided: one tick is enough (coherent levels; only content >= 0.225 fs fades); click test made precise (22, 21) |
| F14 | M | Golden fingerprint not reproducible as written (window, FFT size, mono mix, silent windows, expression ramp, chunking, file format undefined) | 21.1 |
| F15 | M | Alias test method unspecified: a Blackman-window FFT has a ~-63 dB leakage floor, so the -70 dB gate could not be measured | exact-bin method; click metric below 0.2 fs (21) |
| F16 | L | A long WAV without `clm ` and not a multiple of 2048 became ONE cycle (1M x 1023 DFT, useless table) | single cycle only up to `maxSingleCycleSamples` = 8192, else 2048-sample frames (5.4, `WavetableBuilder.h`) |
| F17 | L | Sub tables were built per voice (3 x 16 x instances x 90 KB) | shared process-wide, built by the first `prepare()` (6.2, `SubOscillator.h/.cpp`) |
| F18 | M | Agent F's golden test had no way to turn `FactoryPreset` values into `SynthSettings` without a processor | `SynthSettingsBuilder::fromPlain` / `toPlain` / `indexOf` (`SynthSettingsBuilder.h/.cpp`, 21) |
| F19 | L | The Synth page shows "DSP %" but its context had no access to the processor's load | `SynthPanelContext::dspLoadText` (20.6, `SynthPanel.h`) |
| F20 | L | At a note start from silence the bend smoother slid 5 ms from the previous note's bend | jumps when no voice was active (3.4) |
| F21 | L | Rest expression (Test Pattern) stepped 1 -> 0 at the note-off: Voice Level -> Amp clicked and cut the release | smoothed 5 / 50 ms (16.4, `ExpressionFrame.h`) |
| F22 | L | Poly correction ("the replaced note") undefined; a note replaced by a second steal kept its voice | pairing rule; the dropped note loses its voice (10.1, 10.2, `VoiceAllocator.h`) |
| F23 | L | Ownership: implementers were to write benchmark numbers into this document (integration-owned); no file / owner for processor-level synth tests; 0.4.0 fixture owner ambiguous; no rule for new `.cpp` files; B's voice audio tests depend on A in the same wave | 19; new `tests/plugin/SynthProcessorTests.cpp` (integration, registered in `tests/CMakeLists.txt`) |
| F24 | L | "Setup page" contradicted ROADMAP 4.1 (old pages unchanged in 0.4; editor tests compare labels with `FL-ROUTING.md` / `LIESMICH.txt`); tab order and default page unspecified | 1, 17 |
| F25 | L | `editorScale` was to move to a `UI` child in 0.5 (a needless migration) | stays in SYNTH (13.1) |
| F26 | L | CPU table: typical oscillator share wrong, no caching rules, unison-cap decision not ordered before the goldens | 18 |
| F27 | L | The `VoicePipeline` overload cannot forward without touching the existing `processToSink` | allowed explicitly, tracker member (16.3) |
| F28 | L | Unison with phase random 0 starts in phase: +9 dB first-cycle peak at 8 voices (golden peak limit) | note for F (6.1) |
| F29 | L | Log-scale parameters smoothed linearly in Hz | log2 smoothing (3.2) |
| F30 | L | `glideEnabled` from the smoothed time would glide for 20 ms after switching glide off | latched value (10.2) |

Checked without change: the migration (parameter default off + "missing -> default" in `migrateState`, new
instances on in the constructor), version hint 4, IDs / choice lists append-only, both VoicePipeline change
orders become connected changes through the deferred note-off, the correction flag sits on both events of a
provisional correction (`NoteSegmenter::changeNote`), ExpressionBuffer capacity, JUCE-free core headers, no
include cycles (`SynthTransport` moved down to avoid Lfo -> SynthEngine), CMake registration of every scaffold
file, 16 voices, 1100 x 700 with 75–200 %, the old pages keep a home (tabs 1–3, Synth = tab 4).

Interface changes the implementation agents must know (all compile in the scaffold):
- A: `Wavetable` layout (F1: `guardBefore`, `guardAfter`, `wavetableMipLength` 8x / 1024..8192, `getLevel`
  points at sample 0 with readable `p[-1]`, `p[N]`, `p[N+1]`); Hermite read and queued crossfades
  (`WavetableOscillator.h`); `reset()` forgets tables; `WavetableBuilder::maxSingleCycleSamples`; shared sub
  tables.
- B: `SynthTransport` lives in `SynthSettings.h`; `tempoDivisionQuarterNotes (d, quarterNotesPerBar)`;
  `Lfo::advanceTick (settings, transport, rateOctaves, tickSamples)`; `ModDest::pitch` (22 dests);
  `SynthSettings::ampGainDb` + `limits::ampGainDb`; incremental Envelope; filter guards at `beginTick`; Poly
  correction pairing.
- C: `StereoDelay::beginTick (settings, transport, mixMod, tickSamples)`; engine reset forgets tables;
  decisions at ticks; bend jump; rest-expression smoothing; `ampGain` in the amp stage (via B's voice).
- D: seq_cst store protocol; `WavetableWorker::loadFactoryNow` / `loadEmbeddedNow` / `getRequestedReference`;
  `SynthSettingsBuilder::fromPlain` / `toPlain` / `indexOf` (102 values); `PresetManager` without listener
  (`isModified()` not noexcept any more, `markModified()`, mutex).
- E: `SynthPanelContext::dspLoadText`; `ampGain` knob ("Gain") in ENV 1; "Pitch" destination in the mod
  menus; table menu calls `markModified()`.
- F: `ampGain` for levelling; golden spec 21.1; phase-random note (6.1); Voice Level -> Amp note (16.4).
- Integration: 17 (every block acquire / release, bypass, releaseResources order, constructor sync loads,
  transport bars), 130 parameters, `SynthProcessorTests.cpp`, `docs/STATUS.md` still says 101 parameters.

## 24. Implementation record (integration agent, 2026-11-06)

What the implementation does differently from the text above, and what it added to the frozen interfaces. The
sections above stay as written where nothing is listed here.

### 24.1 Departures

- **A (wavetables):** (1) `pwm` sweeps 50 % -> 10 % (2 % was mostly silence after band-limiting); (2) `sync-sweep`
  gets an extra roll-off `k^(-0.11 log2 r)` so high ratios are not harsher than a saw; (3) time-domain recipes are
  8x oversampled, not 16x (`RealFft` stops at 16384); (4) the factory-table alias measurement is relative to the
  total harmonic power (not the fundamental: some tables have a weak fundamental); (5) the click test's reference
  fades harmonics between 0.30 and 0.45 fs like the mip choice; (6) the pink-noise coefficients are refitted per
  sample rate (<= 0.5 dB from -3 dB/oct); (7) SSE2 Hermite with a C++ fallback; `setTable` on a silent oscillator is
  immediate; `buildFromPcm16` is bit-identical to the import build; a `fmt ` chunk after `data` is accepted.
  Alias results: saw sweep -72.1 dB up to 1 kHz and < -102 dB at 1–5 kHz at every rate; factory tables -71.0 ..
  -71.9 dB. **Finding (accepted for 0.4):** a fast downward glide clicks slightly at a mip band edge because of the
  0.40 hysteresis: -48 / -53 dB below 0.2 fs (44.1 / 48 kHz), -61 / -63.5 dB below 0.17 fs; the content is above
  ~7.5 kHz and only on fast downward glides, so the click test checks below 0.17 fs. Revisit if the user hears it
  (SYNTH-TESTING.md 7 asks for it).
- **B (voice):** doc comments made precise: an Envelope segment ends at its end point (attack at 1; decay /
  release at -80 dB of the start distance); `SvfFilter` drive 0 <-> > 0 dB crossfades over one tick; a
  `SynthVoice` retrigger keeps the oscillator phases and restarts the envelopes from their current values; fresh
  phases only from silence or after a steal fade.
- **C (engine):** on deactivation a voice resets its oscillators and takes the newest table set (private
  `tablesSet`), which fixed a use-after-free the retire test found (a silent voice kept a retired table as
  "previous"). `RealFft`'s constructor parameter was renamed (no `-Wshadow-field-in-constructor` in plugin code).
- **Integration (17):**
  - The FX dry level is the class `DryGain` (`PluginProcessor.h`): dB -> gain (<= -60 dB = 0), per-sample linear
    20 ms ramp from wherever the gain is, the first block after `prepareToPlay` jumps to the target; at exactly
    0 dB and not ramping the samples are not touched.
  - The synth output is added only when it is not exactly silent (min = max = 0 over the chunk): adding +0.0 would
    turn a -0.0 sample of the dry voice into +0.0, so "0 dB dry, synth silent" stays bit-identical to 0.3.1 even
    with the synth switched on.
  - `synthTransportFor (TransportInfo)` (free function) builds `SynthTransport` from the capture's play-head read
    (one read per block for both).
  - The Instrument still declares no MIDI input (`acceptsMidi() == false`, unchanged plugin capabilities): it plays
    the synth from the sidechain mic (E5), not from host MIDI.
  - `setStateInformation` loads every user TABLE with `loadEmbeddedNow` (after `SynthState::fromValueTree` a user
    reference always carries data: embedded, or a hash-only entry the worker looks up among the tables of the
    process), every factory TABLE with `loadFactoryNow`.
  - **`synthOn` default on** (13.3): the parameter default is `synthOnForNewInstances` (on) instead of the
    core default off, and `migrateState()` sets `synthOn` = off for every state without it (0.3.x). Reason:
    clap-validator `param-default-values` failed when a new instance was switched on in the constructor while
    the default said off. The processor constructor loads `basic-shapes` into both slots synchronously and
    calls `presetManager.setCurrent ("Init", false)`.
  - The engine time per block (`synthLoad`) is accumulated over the chunks and added once per block.
- **Editor (17, 20):** every page lives on a content component of 1100 x 700 that the editor scales with a
  transform (aspect kept, centred when the host gives a size off the ratio; `setResizeLimits` 825 x 525 ..
  2200 x 1400 with a fixed-aspect constrainer, corner resizer). The top row (30 px) holds the four page tabs,
  the plugin name, version and format (the old 52 px header is gone) and the scale menu (75 / 100 / 125 / 150 /
  200 %; other scales from the corner show as text). Every resize stores the scale in the processor (SYNTH
  `editorScale`); a new editor opens at the saved scale. The old pages keep their layouts: Voice (420 px) and
  Test & Routing (440 px) are centred vertically in the 1068 x 640 page area, Capture uses the whole area (its
  take list grows). Old pages, `CapturePanel` and `FileDragSource` take the theme colours (accent blue instead of
  the old green); their constants are references to the theme tokens (a copy at static-initialisation time
  could read an uninitialised token - it did, giving a transparent editor).
  - **First-run hint (E4):** ROADMAP's first-run setup is 0.5 (4.2). 0.4 shows one line on the Voice page under
    the input meter: "Internal synth: ON - it plays what you sing (page Synth). Using another synth via the MIDI
    port? Switch Synth On off there." (or the "off" variant), following `synthOn` at 30 Hz. No new control.

### 24.2 Interface additions (all compile-time checked, used by tests)

- `SynthEngine::polyMaxUnison` (= 8, decision of 18), `Chorus::getNanResetCount`, `FdnReverb::numDiffusers`,
  `ExpressionTracker` out-of-line constructor / destructor / move (it owns a `unique_ptr<RealFft>`).
- `PresetManager::getUserPresetFile`, `renameUserPreset`, `deleteUserPreset` (the preset bar's rename / delete).
- `SynthState::Table::embedded` is non-null for every user reference after `fromValueTree` (13.1 lookup rule).
- UI (agent E): `SynthPanelContext::onPresetLoaded` (the editor calls `VocalSynthProcessor::notifyPresetLoaded()`
  = `updateHostDisplay (programChanged)` for CLAP, 14.4); `SynthPanel` getters and test hooks (`getContent`,
  `getPresetBar`, `getWavetableView`, `getTableButtonText`, `getDspText`, table menu ids, `runTimerTickForTests`);
  `Knob::Style` rotary / bar; `ChoiceChip` toggle style; `SectionPanel::setSubtitle / setPowerComponent /
  setHeaderWidth / setActive`; `WavetableView::setMessage`; `PresetBar::onPresetLoaded` and dialog hooks. The
  voice parameter `bendRange` has a second control on the Synth page (VOICE "Bend") - deliberate.
- Processor (integration): `getPresetManager`, `getWavetableStore`, `getWavetableWorker`,
  `getExpressionPublisher`, `getSynthDspLoad`, `getEditorScalePercent` / `setEditorScalePercent` (clamped
  75..200), `notifyPresetLoaded`, `getSynthSettings` (the settings incl. the bend range of the current source),
  `synthTransportFor`, `DryGain`. Editor: `Page::synth`, `defaultWidth / defaultHeight` 1100 x 700,
  `tabRowHeight`, `scaleMenuPercents`, `getContentComponent`, `getSynthPanel`, `getScalePercent`,
  `setScalePercent`, `formatSynthDspLoad`, `synthHintText`.

### 24.3 Wiring as built (17)

Member order in `VocalSynthProcessor`: `apvts`, `wavetableStore`, `synthEngine`, `synthSettings { apvts }`,
`expressionFrames`, `expressionPublisher`, `wavetableWorker { wavetableStore }` (starts its thread),
`presetManager { apvts, wavetableStore, wavetableWorker }`, `synthLeft / synthRight` (4096 each), `dryGain`,
`synthLoad`, `editorScale`; the 0.3 members follow unchanged. `processBlock`: settings (`build()` + bend range)
and transport, `acquire` both slots, flush (+ `synthEngine.releaseAll()`), per chunk: pipeline with
`expressionFrames` (Voice) or pattern -> dry gain or clear -> MIDI out -> capture -> engine (+ add) -> monitor tone;
block end: `release`, `expressionPublisher.publish`, `synthLoad.addBlock`. `processBlockBypassed`: as 0.3.1 +
`acquire`, `synthEngine.reset()`, `release`, publish. `prepareToPlay`: `synthEngine.prepare (rate, 4096)`,
`releaseAllRetired()`; `releaseResources`: `synthEngine.reset()`, `releaseAllRetired()`; destructor:
`wavetableWorker.stop()`, `synthEngine.reset()`, `releaseAllRetired()`, then the capture worker as before. State:
13.1 exactly (SYNTH child, `stateVersion` 4; tables from `getRequestedReference`). Tests:
`tests/plugin/SynthProcessorTests.cpp` (21 Processor row) and the fixtures `tests/data/state-0.4.0-{fx,instrument}`
(FX: Warm Pad modified, osc A `formant-vowel`, osc B an embedded 4-frame user table with an umlaut in its name,
scale 125 %, dry -3 dB; Instrument: Acid Bass, osc A `fm-2op`, synth off, scale 150 %).

### 24.4 Review fixes (0.4.0)

Two reviews of the finished 0.4.0 (review 1: audio path, review 2: off-thread code, UI and docs). Every fix has a
regression test that failed before the fix (tests whose name ends in "(review 0.4 Rn-k)").

| # | Finding | Fix | Test |
|---|---|---|---|
| R1-1 (high) | A flush / source switch sends the MIDI port note-offs and a centre bend, the engine only got `releaseAll()`: its `bendRaw` kept the old value and every later note played out of tune (Test Pattern "Pitch Bend" -> Voice: +1.64 st) | `SynthEngine::releaseAll()` also sets `bendRaw = 0`; the releasing tails slide back with the 5 ms smoother (10.1) | `SynthEngine: releaseAll() centres the pitch bend (flush / source switch, review 0.4 R1-1)`; processor: `SynthProcessor: after Test Pattern "Pitch Bend" the Voice source plays the sung pitch (review 0.4 R1-1)` |
| R1-2 | synthOn off -> on inside the 10 ms fade: note-offs were dropped while fading out, the note hung (and came back in Legato) | while `fadingOut`, `handleGroup` passes the note-offs to the allocator and drops only the note-ons (chosen over a `releaseAll` on the fadingOut -> running transition: a note that is really still held keeps sounding) | `SynthEngine: synthOn off -> on inside the 10 ms fade leaves no hung note (review 0.4 R1-2)` |
| R1-3 | Poly: a note stolen onto a voice, released and started again inside the 2 ms steal fade was retriggered inside the fade and then deactivated with it - silent while held. Same root: a steal arriving between the fade end and its tick restarted a fade from gain 0 (fadeStep 0: the old note came back at full level, the pending note never started) | `SynthVoice::apply`: a `start` on an active voice with a steal fade (`fadeStep > 0`) becomes the pending start (starts clean at the fade end); a `steal` starts a new fade only when no fade runs (`fadeStep <= 0` instead of `stealFadeSamples <= 0`) | `SynthVoice: a start or steal after the pending note's release inside a steal fade starts after the fade (review 0.4 R1-3)`; `SynthEngine: Poly - a note released and restarted inside its steal fade sounds (review 0.4 R1-3)` |
| R1-4 | Host bypass cut a sounding synth at once (click) | `processBlockBypassed` (after the bypassed audio is in the buffer): the first `ceil (10 ms) + 2 ticks` bypassed samples after a processed block render the synth with `releaseAll()` and `synthOn` forced off (the engine's own fade, decided at its ticks, so any bypassed block split gives the same samples) and add it to the output (FX: dry input + fading synth, Instrument: only the fading synth); then `synthEngine.reset()`. Every `processBlock` re-arms it; `prepareToPlay` / `releaseResources` disarm it. Bypassed blocks publish rest expression (`ExpressionFrame {}`) as before. A bypass on a silent synth is exactly the dry input / silence as before | `SynthProcessor: bypass fades a sounding synth out over 10 ms instead of cutting it (review 0.4 R1-4)` (bypassed blocks 7 / 64 / 512 / 4096; the "nothing hangs" test checks the second bypassed block) |
| R1-5 | `getTailLengthSeconds()` = 0 although release / reverb tails ring up to ~10 s | kept: on FL's mixer insert the plugin is processed anyway; a non-zero tail would need `updateHostDisplay` on every synthOn / reverb change. Documented for the user (SYNTH-TESTING 7: export with *Leave remainder*, no *Smart disable*) | - (unchanged; `a new instance plays the synth ...` still checks 0) |
| R2-1 | Choice lists that grow later move saved host automation (normalised index / (n - 1)) | **Rule: choice lists are frozen in length from 0.4.0; later additions need a new parameter** (9.1, 9.2, 15). ROADMAP 0.5 / 0.6 plan additions only for the mod sources: Vowel X, Vowel Y (0.5) and three calibrated vowel morph weights (0.6) - appended now as `ModSource::vowelX, vowelY, vowelWeight1..3` (17 sources, `isReservedModSource`, value 0, never written by the engine), names "Vowel X", "Vowel Y", "Vowel Weight 1..3". The Synth page lists them dimmed as "Vowel X (0.5)" ... "Vowel Weight 3 (0.6)" and not selectable (`params::modSourceReleaseHint`); the modulation rings ignore them. Destinations, LFO shapes, filter modes, divisions, play / glide / sub modes: nothing planned (0.5 adds new parameters such as `vowelRange`, 0.6 chord / CC parameters), lengths unchanged. Parameter values, presets, golden data and the 0.4.0 fixtures are unchanged (the fixture writer reproduces them byte for byte) | `Synth choice lists have their final length from 0.4.0 (saved automation, review 0.4 R2-1)`; `SynthEngine: the mod sources reserved for 0.5 / 0.6 contribute 0 (review 0.4 R2-1)`; the reserved items in `SynthPanel: every synth parameter has exactly one attached control` |
| R2-2 | SYNTH-TESTING step 6 described a right-click menu the knobs do not have | FL's way: move the knob, *Tools -> Last tweaked -> Create automation clip*; alternative *Browse parameters* (unconfirmed) | doc |
| R2-3 | A WAV without `clm ` whose length is a multiple of 2048 (WaveEdit 64 x 256, 1024 frames, a 4096 single cycle) was cut into 2048 frames without asking | `SynthPanel::needsFrameSizeChoice` also asks for every multiple of 2048 longer than 2048 (no `clm `); the question is `SynthPanel::createFrameSizeMenu` with 2048 ticked as the default; `chooseFrameSize` (5.4) is unchanged for the automatic case | `SynthPanel: a WAV of several 2048-sample lengths without clm asks for the frame size (review 0.4 R2-3)` (+ the import test's expectations for 8 x 2048 and 3 x 2048) |
| R2-4 | The Synth page's 30 Hz timer ran while hidden from editor creation until the first visit; the preset bar's 10 Hz timer always | the panel's timers start only via `visibilityChanged()` (constructor: only if visible); `PresetBar::setRefreshing` (off after construction), driven by the panel's `visibilityChanged()` | `Editor: the Synth page and its preset bar tick only while the page is shown (review 0.4 R2-4)` |
| R2-5 | `WavetableView`'s path cache was keyed by a raw pointer that a new table can reuse after the old one is freed | `setTable` with another table invalidates the cache; the raw pointer is gone | `UiFoundation: WavetableView redraws a new table that reuses a freed table's address (review 0.4 R2-5)` |
| R2-6 | The preset bar took a user preset with a factory preset's name as current (rename / delete / save acted on the user file while the factory preset played) | `PresetManager::isCurrentFactory / getCurrentUserFile`; `PresetBar::getCurrentUserPreset` matches by that file, by name only after a project load; the menu ticks by the same identity | `PresetBar: the current user preset is the loaded file, not a preset with the same name (review 0.4 R2-6)` |
| R2-7 | SYNTH-TESTING said Legato glides; the default glide time is 0 | "läuft ohne neuen Anschlag weiter; mit Glide > 0 gleitet er" | doc |
| R2-8 | User tables were shared by the 64-bit FNV-1a hash alone | `findSharedUserTable`: the shared `user:<hash>` table is reused only when the process-wide shared data under that hash holds the same PCM (embedded loads and imports); otherwise the table is built and shared under the hash | `WavetableWorker: a shared user table is reused only for the same PCM, not for the same hash alone (review 0.4 R2-8)` |
