# Synth design: release 0.4.0 "Eigener Klang"

Status: binding design for 0.4.0 (2026-11-03). Basis: `docs/ROADMAP-0.4.md` sections 3 and 4.1, decisions E1–E6
accepted with the recommended options (synth first, native JUCE UI, full scope, synth on for new instances,
Instrument = full synth, 24 presets / default Legato). The scaffold (all headers with frozen public interfaces,
compiling stubs, parameters, CMake entries, one test file per module, 0.3.1 state fixtures) is in the tree.

Rules for every implementer: `docs/ROADMAP-0.4.md` 3.3 (real time), this document, the doc comments of the
frozen headers. A public signature, a constant documented as binding, a parameter (ID, range, default, choice
list), a state property or a file format changes **only** through this document (ask the integration agent).
Private members and the bodies of the `.cpp` files are free for the owner (section 19).

Contents: 1 scope · 2 signal flow and threads · 3 control tick, block-size invariance · 4 SynthSettings ·
5 wavetables · 6 oscillators · 7 filter · 8 envelopes and LFOs · 9 mod matrix · 10 voices · 11 FX ·
12 wavetable store and worker · 13 state and migration · 14 presets · 15 parameters · 16 expression ·
17 processor integration · 18 CPU budget · 19 file ownership · 20 UI foundation · 21 test plan · 22 risks.

---

## 1. Scope

In 0.4.0: the wavetable synth of ROADMAP 3.2 (2 wavetable oscillators with unison, sub, noise, one SVF with
drive, 3 ADSR, 2 global LFOs, 8 mod slots, Mono / Legato / Poly 16, glide, chorus / delay / reverb), 14 factory
wavetables generated in code, WAV import embedded in the project, voice expression (level, brightness,
noisiness, pitch) as mod sources, 24 factory presets and user presets, the UI foundation and the Synth page.

Not in 0.4.0 (unchanged from ROADMAP 2): second filter, warp / FM / sync per oscillator, wavetable editor,
oversampling / HQ mode, arpeggiator, MPE, vowel tracking (0.5), expression -> CC (0.6), chords (0.6).

Unchanged: the MIDI output path, the event format (`VsEvent`, `VsEventTiming`), capture, the test pattern, the
monitor tone (`MonitorSynth`, now the routing "Test Tone" of the Setup page), reported latency 0, plugin codes and
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
| Message | UI, preset load / save (`PresetManager`), import dialog, state get / set (as today) | wait for the audio thread |
| WavetableWorker (one per instance) | read WAV bytes, build mip maps, publish to the store, garbage collection every 100 ms | touch APVTS parameters |
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
   coefficient `1 - exp (-16 / (0.020 fs))`; the first tick after `reset()` jumps to the targets); choices,
   ints and bools switch here (a `playMode` change -> `VoiceAllocator::releaseAll()`);
2. latches pending expression frames (16.4) and advances the expression ramps; advances the bend smoother
   (one-pole, 5 ms, per tick);
3. advances both LFOs (`Lfo::advanceTick`) with the LFO rate modulation of the previous tick (a one-tick delay
   for every global destination: no evaluation cycles, LFO 1 -> LFO 1 rate is legal);
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
  sample are applied before the group (last one wins); CCs are ignored by the engine.
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
- Settings are latched at ticks; a `process()` call that changes settings mid-tick affects the next tick only.
  (Host automation arrives per block, so automated output is only invariant for equal settings — by design.)
- Cross-platform comparisons (MSVC vs clang) use feature fingerprints only (21, golden).

## 4. SynthSettings (`core/include/vocalsynth/synth/SynthSettings.h`)

One plain struct with every synth parameter in the parameter's own units ("Klarwerte": percent 0..100, ms, dB,
Hz, cents), enums whose values equal the choice indices, and `bendRangeSemitones` (filled by the processor).
`SynthSettings {}` IS the default of every parameter (`SynthParameters.h` takes its defaults from it). Ranges:
`synth::limits`. `sanitised()`: clamp every value to its range, non-finite -> default, enums forced into their
range, ints rounded and clamped, `bendRangeSemitones` clamped 0..48; real-time safe. The engine sanitises the
settings it receives once per `process()` call.

## 5. Wavetables

### 5.1 Data format (`Wavetable.h`)

- Frame size 2048, at most 256 frames, 11 mip levels. Level L holds harmonics 1..1024 >> L at
  `mipLength (L) = clamp (4 * (1024 >> L), 256, 2048)` samples + 1 guard sample (copy of sample 0):

  | level | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
  |---|---|---|---|---|---|---|---|---|---|---|---|
  | harmonics | 1024 | 512 | 256 | 128 | 64 | 32 | 16 | 8 | 4 | 2 | 1 |
  | length | 2048 | 2048 | 1024 | 512 | 256 | 256 | 256 | 256 | 256 | 256 | 256 |

- 7424 + 11 = 7435 floats per frame (`frameStride`), levels back to back, frames back to back, one
  `std::vector<float>`. Memory: 29.7 KB per frame, 1.9 MB for 64 frames, **7.6 MB for 256 frames** (bound).
- Immutable after the builder returns; `getContentHash()` = FNV-1a 64 of the PCM16 frames (13).
- Level 0 at 2048 samples is the normalised source frame (harmonics up to 1023 + Nyquist dropped): the UI draws
  it, embedding uses the PCM16 frames (`WavetableFrames`) instead.

### 5.2 RealFft (`RealFft.h`)

Own iterative radix-2 complex FFT of size N/2 with the standard real-input split (twiddles in `prepare`),
double precision, sizes 16..16384. Conventions in the header (unnormalised forward, 1/N inverse). Used by the
builder (background) and the expression tracker (audio thread after `prepare`, `powerSpectrum`).

### 5.3 WavetableBuilder (`WavetableBuilder.h`)

`build (name, samples, frameSize, cancel)`: resample every frame to 2048 via its spectrum (frameSize != 2048;
frameSize 0 = single cycle of any length via direct DFT of harmonics 1..min (1023, len / 2 - 1)), thin to <= 256
frames (`round (i * (n - 1) / 255)`), remove DC per frame, peak-normalise the whole table to 1, quantise to PCM16
and back (so a table rebuilt from embedded data is bit-identical), then per frame and level: FFT 2048, keep bins
1..maxHarmonic, inverse at `mipLength` (zero-padded / truncated spectrum), guard sample. Deterministic.
Cancel flag polled between frames. Target: 256 frames in < 150 ms (Release, background), any factory table
< 150 ms. `makeSineTable()`: 1 frame, every level a pure sine (fallback in `SynthEngine::prepare`).

### 5.4 Import (`WavReader.h`, `WavetableBuilder::chooseFrameSize / buildFromWav`)

- `WavReader::read (bytes)`: RIFF/WAVE, PCM 8 (unsigned) / 16 / 24 / 32, float 32 / 64,
  `WAVE_FORMAT_EXTENSIBLE` with these sub-formats, 1..8 channels -> mean (mid), chunks in any order, padding,
  unknown chunks skipped; `clm ` chunk: text "<!>2048 ..." -> frame size (power of two 32..8192). Data chunk
  longer than the file: the complete sample frames present are read, `dataTruncated = true`. More than
  256 x 4096 sample frames: `tooLong`. Clean errors (`WavReadError`, `describeWavReadError`) for everything else;
  no exception except `std::bad_alloc`, no read outside the span (fuzz test). The plugin reads the file with
  `juce::File::loadFileAsData` (Unicode paths, spaces, umlauts) and passes the bytes; the core does no file IO.
- Frame size (`chooseFrameSize`): import-dialog override (256 / 512 / 1024 / 2048 / 4096) if the file holds at
  least one such frame; else the `clm ` size; else 2048 if the length is a positive multiple of 2048; else 0 =
  single cycle (the whole file is one cycle). Leftover samples after the last whole frame are ignored.
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
| pwm | PWM | 64 | pulse width 50 % -> 2 % |
| harmonic-sweep | Harmonic Sweep | 64 | odd harmonics (1/k) crossfading to even-only (1/k) + fundamental |
| saw-bright-dark | Saw Bright-Dark | 32 | saw with spectral tilt 0 -> -12 dB/oct |
| formant-vowel | Formant Vowels | 64 | saw spectrum x formant envelope (F1..F3 Gaussian bumps), vowels A E I O U at 0/25/50/75/100 %, f0 reference 130 Hz |
| sync-sweep | Sync Sweep | 64 | hard-sync saw, slave/master ratio 1 -> 8 (time domain at 16x, then the builder band-limits) |
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
  centre); the unison sum is scaled by `1 / sqrt (n)`.
- Start phase (`restart`): voice i = phaseRandom * r_i (xorshift32 from the voice's seed); 0 = all phase 0.
  Legato never restarts phases. Unison count changes at a tick; added voices start at phase r_i * phaseRandom.
- Reading: frame position `pos * (numFrames - 1)`, linear interpolation inside the level (guard sample, no
  wrap test), linear between the two neighbour frames. Position, level, pan ramp per sample over the tick.
- Mip level per tick (largest increment of the tick over start / end and all unison voices): lowest L with
  `(1024 >> L) * inc <= 0.45`, back down only when `<= 0.40`; a change crossfades linearly over that tick.
  inc > 0.45: silent (gain ramps to 0).
- Table change: crossfade over `ceil (0.005 fs / 16)` ticks from the next tick; a change during a crossfade
  restarts it from the then-current table. `getTablesInUse()` = {current, previous while fading}.
- nullptr table: silence.

### 6.2 SubOscillator (`SubOscillator.h`)

Own 1-frame tables (sine, triangle, square) built in `prepare()` with the same mip rules; pitch = the voice's
note pitch incl. glide and bend (not Osc A's offsets / pitch modulation) - 12 or - 24 semitones; mono, same
signal on L and R; level linear (`subLevel` + Sub Level modulation, clamped 0..1).

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

Exponential ADSR, closed form per segment (formulas in the header): attack overshoot target 1.5 (concave, ends
at exactly 1 at time A), decay / release reach -80 dB of their distance at exactly D / R and then snap.
`advance (settings, n)` handles segment ends inside n exactly, so segment times are exact to the sample for any
tick split. noteOn / noteOff restart from the caller's current value (no jumps). Env 1 drives the amp
(`gain = env1 * (1 - ampVelocity/100 * (1 - velocity/127))`), Env 2 the filter (`filterEnvAmount`) and the mod
matrix, Env 3 only the mod matrix. A voice is inactive once Env 1 is idle (and no steal fade runs). Legato:
no restart (all three).

### 8.2 LFO (`Lfo.h`)

Global (two per engine), evaluated per tick, output -1..1 (shapes in the header). Rate: Hz, or tempo sync
`bpm / 60 / quarterNotes (division)` with the host tempo (120 if unknown), times `2^(rate mod)`, ±4 octaves at
100 %. Free-running phase (not locked to the song position). `retrigger()` is called by the engine for every
envelope-starting note start (start / steal commands, not legato / correction): phase 0 if `lfoNRetrig` is on;
the fade-in (`lfoNFade` ms, linear 0 -> 1) restarts in any case. S&H / smooth random: xorshift32 seeded in
`prepare()`.

## 9. Mod matrix (`ModMatrix.h`)

### 9.1 Sources (append only; 0.5 adds Vowel X, Vowel Y)

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

### 9.2 Destinations and scaling (append only)

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
  second steal during the fade replaces the pending note).
- `releaseAll()`: `release` for every voice holding a note (flush, bypass is a reset, play-mode change).
- Invariants: <= 16 voices, every started note gets a release on its last note-off, unknown note-offs ignored,
  never more than `VoiceCommandList::capacity` (64) commands per group.

### 10.2 Glide and the provisional-note correction

- Glide: constant time `glideTime` ms, linear in semitones from the current pitch (mono: the voice's own
  current, possibly still gliding pitch; poly: the most recent note-on's note), evaluated per tick. Always: on
  every start / legato / steal once a previous note exists since `reset()`; Legato Only: on connected changes
  only. glideTime 0: no glide.
- **Correction rule (<= 40 ms):** a connected change caused by a note-on whose time is within
  `correctionWindowSeconds = 0.040` (absolute samples, rounded down) of the replaced note's own note-on, or
  whose event carries `EventTimingFlags::correction`, is a `correction`: pitch jumps (no glide, also when a
  glide is running), envelopes, velocity and the random value continue, the LFO is not retriggered. Applies in
  Mono, Legato and Poly (the new note takes over the old note's voice).
- Answer to ROADMAP 3.2 ("reichen die `VsEventTiming`-Infos?"): yes for the pipeline — every provisional
  correction carries the `correction` flag and happens 10–30 ms after the provisional note-on. The time rule is
  still the primary rule so that the test pattern, flushes and any future source behave the same, and so the
  synth never depends on the capture side output; the flag extends the window when a correction comes later
  (Bass / Wide window ~40 ms).

### 10.3 SynthVoice (`SynthVoice.h`)

Per voice: Osc A, Osc B, Sub, Noise -> (drive + SVF if `filterOn`) -> amp (Env 1 x velocity x Amp Level mod,
per-sample ramp) -> voice pan -> added to the mix. Pitch in semitones: note + bend + glide offset; osc pitch adds
`octave * 12 + semi + fine / 100 + pitch mod`; increment = `440 * 2^((pitch - 69) / 12) / fs`. Osc on/off:
`oscXOn` off -> the oscillator is not rendered (phases frozen; switched on mid-note it continues from them, its
gain ramps up over one tick). Steal: linear fade to 0 over `stealFadeSeconds`, then a clean start of the
pending note. Inactive voices cost nothing.

### 10.4 Gain staging

Osc level 70 %, unison sum `1/sqrt(n)`, one oscillator: about -3 dBFS peak for a saw before the amp; synthVolume
-6 dB default; poly chords of 3 notes stay below 0 dBFS at defaults. No limiter in 0.4 (presets keep peak
<= -1 dBFS in the golden test). Output exactly 0 when all voices are inactive and all FX tails ended.

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
 worker/message:  old = slots[s].exchange (new, acq_rel); retired.push ({old, retireBlock = blockCounter.load (acquire)})
 audio, block start:  engine.setWavetable (s, slots[s].load (acquire))        (store.acquire)
 audio, block end:    inUse[4] = engine.getTablesInUse (A / B) (release);  blockCounter.fetch_add (1, release)
 collectGarbage():    free retired r when blockCounter.load (acquire) >= r.retireBlock + 2 and r.table not in inUse[]
```

Why it is safe: the block running during the exchange ends (+1); the next block starts after the exchange, loads
the new pointer, reports what the engine still reads — including the old table while a 5 ms crossfade runs —
and ends (+2). Only after that is "not in inUse" meaningful. The audio thread only does atomic loads / stores.
Freeing = dropping the `shared_ptr` under the store mutex on the worker / message thread. Without audio
(`prepareToPlay`, `releaseResources`, destructor): `releaseAllRetired()` clears `inUse` and frees everything
retired. No `std::atomic<std::shared_ptr>`, no free on the audio thread. Stress test: 1000 publishes from the
message thread while an audio thread renders (+ optional local TSan run).

### 12.2 Worker (`plugin/WavetableWorker.h`)

One `juce::Thread` per instance; requests per slot (newest wins; a running build is cancelled via the builder's
cancel flag): factory id, import (file + frame-size override), embedded `UserTableData`. Results: publish +
status (`Status`: busy, last error per slot, completed counter) for the UI. Every 100 ms: `collectGarbage()`.
Import: bytes via `juce::File::loadFileAsData`, `WavReader`, `buildFromWav`, name = file name, data = PCM16
frames, hash = `fnv1a64` -> `user:<16 hex>`. Failure: the slot keeps its table, the UI shows the error.

### 12.3 Sharing and memory

Process-wide map key -> `weak_ptr<const Wavetable>` (`findShared` / `addShared`, mutex, never the audio
thread): `factory:<id>` and `user:<hash>`. Per instance at most 2 current + 2 crossfading + retired tables
(~4 x 7.6 MB worst case with 256-frame user tables; typical factory use 2–4 MB shared).

## 13. State and migration

### 13.1 Layout

```xml
<PARAMS stateVersion="4">
  <PARAM id="..." value="..."/>            (all 129 parameters)
  <CAPTURE version="1" .../>               (unchanged)
  <SYNTH version="1" presetName="Init" presetModified="0" editorScale="100">
    <TABLE osc="0" ref="factory:basic-shapes" name="Basic Shapes"/>
    <TABLE osc="1" ref="user:0123456789abcdef" name="My Table.wav" frames="64" data="(base64 PCM16 LE)"/>
  </SYNTH>
</PARAMS>
```

- User table data stored once per hash (the second TABLE with the same ref has no `data`); <= 1 MB raw per
  256-frame table (~1.4 MB base64).
- `getStateInformation`: `apvts.copyState()`, remove CAPTURE and SYNTH children, append CAPTURE and SYNTH
  (`SynthState` from the store's slot references + user data + `PresetManager` name / modified + editor scale),
  set `stateVersion = 4`.
- `setStateInformation`: read CAPTURE and SYNTH, remove all of both (`SynthState::removeFromState`),
  `migrateState`, `replaceState`, apply capture, apply synth state: per slot a worker request (factory id, or the
  embedded data; missing / broken -> default factory table), preset name / modified, editor scale;
  `updateHostDisplay (programChanged)` as today.
- Tolerant: missing / extra / broken SYNTH, TABLE, refs, data -> defaults; extra children of later versions are
  ignored.

### 13.2 stateVersion

Root property `stateVersion` = 4 from 0.4.0 (5 / 6 later). Missing = 0.3.x or older. Future migrations key on it.

### 13.3 Migration from 0.3.1 (synth stays off)

The **parameter default of `synthOn` is Off**. `migrateState()` already gives every parameter missing from a
state its default, so a 0.3.x project loads with `synthOn` off and every synth parameter at its default — no
special rule needed, robust for any older state. New instances get the decision E4 / E5 value: the processor
constructor sets `synthOn` to `params::synthOnForNewInstances` (= on, FX and Instrument) right after creating the
APVTS (integration). Consequences: host "reset to default" turns the synth off (acceptable); everything else of
an old project is unchanged (fixture test). Fixtures: `tests/data/state-0.3.1-fx.bin / .xml`,
`state-0.3.1-instrument.bin / .xml` = `getStateInformation()` of the v0.3.1 code (captured before the scaffold
added any parameter) with non-default voice, test, capture parameters and capture settings.
`StateFixtureTests.cpp`: every saved value unchanged, synthOn off even if it was on before loading, every synth
parameter at its default, CAPTURE equal, one CAPTURE child after re-saving. From now on one fixture per release
(0.4.0 adds `state-0.4.0-*.bin` with a SYNTH child and an embedded user table, agent D / integration).

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

### 14.2 Categories

`PresetCategory`: Init, Lead, Bass, Pad, Pluck/Keys, Vocal, FX (stored by name; unknown -> FX for user presets).

### 14.3 Factory presets (`plugin/FactoryPresets.cpp`, agent F)

Data in code: name, category, tags, table A / B ids, the values that differ from the defaults. 24 presets
(names may be tuned by F):

| # | Category | Name | Idea (voice sources in bold) |
|---|---|---|---|
| 0 | Init | Init | all defaults (saw at position 50 %, LP24 open, Legato) |
| 1–6 | Lead | Voice Lead, Smooth Sine, Bright Saw, Square Vibrato, Sync Lead, Whistle | **Level -> cutoff** (Voice Lead); glide; 5x unison; LFO 1 vibrato with fade-in; Env 3 -> sync position; sine + **Noisiness -> noise** (Whistle) |
| 7–11 | Bass | Sub Bass, Reese, Acid, Pluck Bass, FM Bass | sine + sub; 2 detuned saws; LP24 resonance + Env 2; short Env 2 decay; **Brightness -> FM position** |
| 12–14 | Pad (Poly) | Warm Pad, Choir Pad, Glass Pad | unison + chorus + reverb; choir table + **Level -> amp**; bell table, long release |
| 15–17 | Pluck/Keys | Pluck, E-Piano, Organ | short amp env; FM 2-op; organ table, Poly |
| 18–21 | Vocal | Vowel Morph, Talking Saw, Breathy Voice, Vibrato Wah | **Brightness -> Osc A position** on formant-vowel; **Brightness + Level -> cutoff**; **Noisiness -> noise level**; **Voice Pitch -> cutoff** |
| 22–23 | FX | Riser, Glitch | LFO -> pitch / delay; digital table, S&H -> position |

At least 8 presets use voice sources (above: 9). Every preset: peak <= -1 dBFS, no DC, not silent (golden test).

### 14.4 Loading rules

Message thread; each preset parameter: `beginChangeGesture`, `setValueNotifyingHost (convertTo0to1 (clamped
plain))`, `endChangeGesture`; missing IDs -> default, unknown IDs ignored; tables via the worker; never touches
`synthOn`, `synthVolume`, `dryLevel`, voice, test or capture parameters. Afterwards name = preset name,
modified = false; any later change of a preset parameter sets modified (APVTS listener; ignored during load).
CLAP: the processor calls `updateHostDisplay (programChanged)` after a preset load (integration). Prev / next
walk the full list (factory, then user) with wrap-around.

## 15. Parameters (`plugin/SynthParameters.h`)

101 parameters, version hint 4, appended in this order after `recordBars` by `addSynthParameters()` (called at the
end of `createLayout()`), total 129. IDs permanent. Texts without unit (label = unit). "log" = exactly
logarithmic mapping, "skew c" = `setSkewForCentre (c)`.

| # | ID | Name | Type / range | Default | Label | Mapping |
|---|---|---|---|---|---|---|
| 1 | synthOn | Synth On | bool | **off** (new instances: on, 13.3) | | |
| 2 | synthVolume | Synth Volume | -60..6 | -6 | dB | linear; -60 = silent |
| 3 | dryLevel | Dry Level | -60..0 | 0 | dB | linear; -60 = silent; FX only |
| 4 | playMode | Play Mode | Mono, Legato, Poly | Legato | | |
| 5 | glideTime | Glide Time | 0..2000 | 0 | ms | skew 300 |
| 6 | glideMode | Glide Mode | Always, Legato Only | Legato Only | | |
| 7 | ampVelocity | Amp Velocity | 0..100 | 50 | % | |
| 8–18 | oscAOn, oscAPosition, oscAOctave, oscASemi, oscAFine, oscALevel, oscAPan, oscAUnison, oscADetune, oscASpread, oscAPhaseRand | Osc A … | bool; 0..100; int -3..3; int -12..12; -100..100; 0..100; -100..100; int 1..8; 0..100; 0..100; 0..100 | on, 50, 0, 0, 0, 70, 0, 1, 20, 50, 0 | ; %; oct; st; ct; %; %; ; ct; %; % | linear |
| 19–29 | oscB… (same 11) | Osc B … | same | **off**, 50, 0, 0, 0, 70, 0, 1, 20, 50, 0 | same | |
| 30 | subLevel | Sub Level | 0..100 | 0 | % | |
| 31 | subOctave | Sub Octave | -1 Oct, -2 Oct | -1 Oct | | |
| 32 | subShape | Sub Shape | Sine, Triangle, Square | Sine | | |
| 33 | noiseLevel | Noise Level | 0..100 | 0 | % | |
| 34 | noiseColor | Noise Color | 0..100 (white -> pink) | 0 | % | |
| 35 | filterOn | Filter On | bool | on | | |
| 36 | filterMode | Filter Mode | LP12, LP24, BP, HP, Notch | LP24 | | |
| 37 | filterCutoff | Filter Cutoff | 20..20000 | 20000 | Hz | log |
| 38 | filterResonance | Filter Resonance | 0..100 | 0 | % | |
| 39 | filterDrive | Filter Drive | 0..24 | 0 | dB | |
| 40 | filterKeytrack | Filter Keytrack | 0..100 | 0 | % | |
| 41 | filterEnvAmount | Filter Env Amount | -100..100 (= ±5 oct) | 0 | % | |
| 42–45 | env1Attack, env1Decay, env1Sustain, env1Release | Env 1 … | 0..10000, 0..10000, 0..100, 0..10000 | 5, 300, 100, 200 | ms, ms, %, ms | skew 500 |
| 46–49 | env2… | Env 2 … | same | 5, 300, **0**, 200 | | |
| 50–53 | env3… | Env 3 … | same | 5, 300, **0**, 200 | | |
| 54–58 | lfo1Shape, lfo1Rate, lfo1Sync, lfo1Retrig, lfo1Fade | LFO 1 … | 7 shapes; 0.01..30 Hz log; 15 divisions; bool; 0..10000 ms skew 1000 | Sine, 5, Off, off, 0 | ; Hz; ; ; ms | |
| 59–63 | lfo2… | LFO 2 … | same | Sine, **0.5**, Off, off, 0 | | |
| 64–87 | modNSource, modNDest, modNAmount (N = 1..8) | Mod N … | 12 sources; 21 dests; -100..100 | Off, Off, 0 | ; ; % | |
| 88–91 | chorusOn, chorusRate, chorusDepth, chorusMix | Chorus … | bool; 0.05..5 Hz log; 0..100; 0..100 | off, 0.8, 50, 50 | | |
| 92–97 | delayOn, delayTime, delaySync, delayFeedback, delayPingPong, delayMix | Delay … | bool; 1..2000 ms log; 15 divisions; 0..95; bool; 0..100 | off, 375, Off, 35, off, 25 | | |
| 98–101 | reverbOn, reverbSize, reverbDamping, reverbMix | Reverb … | bool; 0..100; 0..100; 0..100 | off, 50, 50, 25 | | |

Choice lists (append only): LFO shapes Sine, Triangle, Saw Up, Saw Down, Square, S&H, Smooth Random; divisions
Off, 1/32, 1/16T, 1/16, 1/16D, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2, 1 Bar, 2 Bars, 4 Bars (a bar = 4 quarter
notes in 0.4); sources and destinations as in 9.1 / 9.2. Value texts: integers for %, cents, ms >= 100, Hz >= 100;
one decimal for dB and ms < 100; two decimals for Hz < 10. Not parameters: table choice (SYNTH state), preset
name. `isPresetParameter()`: all synth parameters except synthOn, synthVolume, dryLevel (98).

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
without the buffer (existing tests unchanged + a new comparison test). One frame per analysis hop, pushed with
the hop's event offset (the input sample that completed the hop); the tracker runs only when a buffer is given
(no CPU cost for the old overloads). `reset()` also resets the tracker; a frame is pushed for every hop even
when unvoiced (level falls to 0). The pipeline prepares the tracker in `prepare()`.

### 16.4 Engine side and Test Pattern

The engine latches frames at the first tick at or after their absolute position (newest wins), then ramps
linearly from its current value to the frame over the distance to the previous frame (clamped 16..4096 samples,
first frame after `reset()`: 16). `expression == nullptr` (Test Pattern, or Voice mode before the first frame):
`restExpression (any voice gated)` — level 1 while a note sounds, everything else 0, so presets stay audible.

### 16.5 UI

The processor publishes `engine.getCurrentExpression()` once per block to its `ExpressionPublisher`; the Synth
page shows four meters at 30 Hz (Voice section). Only meaningful in Voice mode (Test Pattern: rest values).

## 17. Processor integration (integration agent; `PluginProcessor.*`, `PluginEditor.*`)

Members (declaration order matters for destruction: worker before store, engine before store):
`WavetableStore wavetableStore; synth::SynthEngine synthEngine; SynthSettingsBuilder synthSettings;
ExpressionBuffer expressionFrames; ExpressionPublisher expressionPublisher; WavetableWorker wavetableWorker;
PresetManager presetManager; std::vector<float> synthLeft, synthRight (maxChunkSamples, constructor);
DspLoadMeter synthLoad;` + accessors for the editor (`getPresetManager`, `getWavetableStore`,
`getWavetableWorker`, `getExpressionPublisher`). Constructor: `synthOn = synthOnForNewInstances`, worker
requests the default factory table for both slots.

`processBlock` additions (order):
1. `auto synth = synthSettings.build(); synth.bendRangeSemitones = voiceMode ? bendRangeSemitones (voiceSettings.bendRange) : 2;`
2. block start: `synthEngine.setWavetable (s, wavetableStore.acquire (s))` for s = 0, 1.
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
- `processBlockBypassed`: as today + `synthEngine.reset()` (silent; nothing hangs) + `release` of the in-use
  tables; Instrument output stays silent, FX passes the dry voice.
- `prepareToPlay`: `synthEngine.prepare (rate, maxChunkSamples)`, `wavetableStore.releaseAllRetired()`;
  `releaseResources`: `releaseAllRetired()`.
- `getTailLengthSeconds()` stays 0 (the synth only sounds from MIDI that the plugin itself creates; FL keeps
  processing an effect on a mixer insert). Latency stays 0.
- State: 13.1. Presets: CLAP `updateHostDisplay (programChanged)` after a preset load.
- DSP display: analysis and synth separately (`synthLoad` measures the engine calls).
- Editor: fourth page "Synth" (`ui::SynthPanel`), `VsLookAndFeel` for the whole editor, design size
  1100 x 700 (tab row 30 + page 670), fixed-aspect scaling 75–200 % (`setResizable`, constrainer, transform on
  a content component), scale menu 75 / 100 / 125 / 150 / 200 %, scale saved in SYNTH `editorScale`; the old
  pages keep their content; existing editor tests move with the size change in the same step.

## 18. CPU budget (48 kHz, block 128, Release; 1 % = 26.7 µs per block)

| Module | Benchmark target | Share in "typical" | Share in "worst" |
|---|---|---|---|
| WavetableOscillator | <= 5 ns per unison voice per sample | 8 voices: 0.8 % | 256 voices: 6 % |
| Sub + Noise | <= 3 ns per voice per sample | 0.05 % | 0.5 % |
| SvfFilter LP24 stereo | <= 15 ns per voice per sample | 0.1 % | 1.2 % |
| Voice tick (env, glide, mod, coefficients, mip) | <= 300 ns per voice per tick | 0.1 % | 1.4 % |
| Global tick (LFOs, smoothing, global mod) | <= 1 µs per tick | 0.3 % | 0.3 % |
| Chorus / Delay / Reverb | <= 15 / 10 / 60 ns per sample | 0.4 % | 0.4 % |
| ExpressionTracker | <= 0.5 % | 0.3 % | 0.3 % |
| Total engine | | **mean <= 3 %, peak <= 6 %** | **<= 12 %** |
| Silence (all inactive, tails ended) | | <= 0.5 % | |

Typical = Legato, 2 osc x 4 unison, LP24, chorus + delay + reverb. Worst = Poly 16 x 2 osc x 8 unison, all FX.
Whole `processBlock` worst case < 20 % (unchanged). If "worst" misses 12 % locally, Poly unison is capped at 4
(constant in the engine, no parameter). Benchmarks: Catch2 `BENCHMARK` with tag `[!benchmark]`; values recorded
in this section by the implementers (C: typical, worst, silence; A: 256-frame mip build < 150 ms).

## 19. File ownership

Phase 0 (this design agent) wrote every file below as a scaffold. From phase 1 on, only the owner edits a file.
**Nobody but the integration agent edits CMake files, `plugin/Parameters.h`, `plugin/SynthParameters.h`,
`PluginProcessor.*`, `PluginEditor.*`**; new test helpers that are headers need no CMake entry.

| Agent | Files (headers `core/include/vocalsynth/...`, sources `core/src/...`, tests `tests/...`) |
|---|---|
| A Wavetable | `synth/RealFft`, `synth/Wavetable`, `synth/WavetableBuilder`, `synth/FactoryWavetables`, `synth/WavReader`, `synth/WavetableOscillator`, `synth/SubOscillator`, `synth/NoiseSource` (.h + .cpp); tests `core/synth/{RealFft,Wavetable,WavetableBuilder,FactoryWavetables,WavReader,WavetableOscillator,SubOscillator,NoiseSource}Tests.cpp`, `core/synth/SynthTestUtils.h` (shared helpers: others read, ask A to add) |
| B Voice | `synth/SvfFilter`, `synth/Envelope`, `synth/Lfo`, `synth/ModMatrix`, `synth/SynthSettings`, `synth/SynthVoice`, `synth/VoiceAllocator`; tests `core/synth/{SvfFilter,Envelope,Lfo,ModMatrix,SynthSettings,SynthVoice,VoiceAllocator}Tests.cpp` |
| C Engine + FX + Expression | `synth/SynthEngine`, `synth/fx/{Chorus,StereoDelay,FdnReverb}`, `ExpressionFrame.h`, `ExpressionTracker`, the new `VoicePipeline` overload only (16.3, in `VoicePipeline.h/.cpp`); tests `core/synth/{SynthEngine,Chorus,StereoDelay,FdnReverb}Tests.cpp`, `core/{ExpressionFrame,ExpressionTracker,VoicePipelineExpression}Tests.cpp` |
| D Plugin data | `plugin/{SynthSettingsBuilder,WavetableStore,WavetableWorker,SynthState,PresetManager}.{h,cpp}`; tests `plugin/{SynthSettingsBuilder,WavetableStore,WavetableWorker,SynthState,PresetManager,SynthParameter,StateFixture}Tests.cpp`, `tests/data/state-*` |
| E UI | `plugin/ui/*` (Theme.h, VsLookAndFeel, Knob, ChoiceChip, SectionPanel, WavetableView, PresetBar, SynthPanel); tests `plugin/{SynthPanel,UiFoundation}Tests.cpp` |
| F Sound / presets | `plugin/FactoryPresets.{h,cpp}` (data only; `PresetCategory` frozen), `tests/plugin/{FactoryPresets,GoldenPreset}Tests.cpp`, `tests/data/golden/presets-0.4.json`, `docs/SYNTH-TESTING.md` (German) |
| Integration | `PluginProcessor.*`, `PluginEditor.*`, `Parameters.h`, `SynthParameters.h`, `CMakeLists.txt`, `core/CMakeLists.txt`, `tests/CMakeLists.txt`, `cmake/*`, existing tests that the integration changes (editor size, parameter counts), README / STATUS / this document |

Waves (at most 2 agents at a time, own build directory each): 1 = A + B, 2 = C + D, 3 = E + F, 4 = integration.
B builds `SynthVoice` against the frozen oscillator interface (stubs are silent; voice tests may use a test
oscillator via `SynthTestUtils.h` or wait for A's merge). C's engine tests need A and B (wave 2).
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
2D frame at the current position (+ position marker), path cached, rebuilt when table / position change
(> 0.5 %), 30 Hz from the panel's timer; table from `WavetableStore::getSlot` (`shared_ptr`, never the audio
pointer). 3D stack in 0.5.

### 20.5 PresetBar
[<] [name * — menu by category, User, Init] [>] [Save] (async AlertWindow: name + category). IDs `presetPrev`,
`presetName`, `presetNext`, `presetSave`.

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
 | FILTER [on] mode cut res drive key env     | ENV 1 (Amp) A D S R | ENV 2 | ENV 3                 | 150
 +--------------------------------------------+---------------------+-----------------------------+
 | LFO 1 shape rate sync retrig fade | LFO 2   | MOD MATRIX 8 rows: source v  dest v  amount      | 260
 +-----------------------------------+---------+--------------------------------------------------+
 | FX: CHORUS on rate depth mix | DELAY on time sync fb pp mix | REVERB on size damp mix           |
 +--------------------------------------------------------------------------------------------------+
```

Table menu per oscillator: the 14 factory tables, separator, "Import WAV..." (async `FileChooser`, frame-size
option in a second step only when the file is ambiguous), the current user table; worker errors shown under the
view. Every synth parameter has exactly one control on this page (test). Timer 30 Hz: expression meters,
wavetable position marker, worker status.

## 21. Test plan per module

Every module has its test file (19); tags `[synth]`, `[expression]`, `[plugin]`; benchmarks `[!benchmark]`.

| Module | Tests (all must pass; numbers are gates) |
|---|---|
| RealFft | forward / inverse round trip < 1e-12 rel; Parseval; impulse, DC, single bin; all sizes 16..16384 |
| Wavetable | layout constants (7435 per frame), memory 256 frames <= 7.7 MB, level pointers disjoint |
| WavetableBuilder | per level no energy above maxHarmonic (< -120 dB rel. to the strongest bin), DC = 0, level-0 peak = 1 ±1e-4, deterministic (two builds bit-identical), thinning 300 -> 256, single cycle of 1000 / 1001 samples, resample 512 / 4096, PCM16 rebuild bit-identical; benchmark 256 frames < 150 ms |
| FactoryWavetables | 14 ids unique, `fromId` round trip, every table builds, frame counts <= 256, deterministic, each < 150 ms |
| WavReader | 8 / 16 / 24 / 32 PCM, float 32 / 64, extensible, stereo -> mid, `clm ` (Serum-style "<!>2048"), odd chunk padding, chunk order, multiples of 2048, truncated data, empty / header-only / random bytes (10 000 fuzz cases, no crash, clean error) |
| WavetableOscillator | f0 within ±0.1 ct (FFT peak) 20 Hz–8 kHz; alias: saw sweep + every factory table at 44.1 / 48 / 96 kHz, non-harmonic energy >= 60 dB below f0 up to f0 = 5 kHz (>= 70 dB up to 1 kHz); unison detune distribution and spread; position morph and mip switch during a glide: max sample step vs reference bounded; table crossfade 5 ms without click; benchmark |
| SubOscillator / NoiseSource | shapes and octaves (FFT), band limit; noise reproducible after reset, white flat / pink -3 dB/oct ±1 dB, RMS level |
| SvfFilter | LP12 / LP24 -3 dB at fc ±0.5 dB (100 Hz, 1 kHz, 10 kHz at 44.1 / 48 / 96 kHz), slopes 12 / 24 dB/oct ±1 dB, BP / HP / notch shape; max resonance + 24 dB drive + cutoff jumps 20 Hz <-> 20 kHz every tick for 10 s: finite and bounded; NaN input -> reset counter; drive 0 = linear |
| Envelope | segment times exact to ±1 sample for split sizes 1..16 and any offset; values match the closed forms; retrigger / release from mid-segment without jumps; A/D/R = 0 |
| Lfo | shapes, rate, tempo sync at 90 / 120 / 174 BPM (period within 1 sample per cycle), retrigger, fade-in, S&H determinism |
| ModMatrix | scaling of every destination, summing of slots, off slots, global / per-voice split |
| SynthSettings | defaults == parameter defaults (static_asserts), sanitised clamps every field, NaN -> default |
| VoiceAllocator | random event sequences (10 000, all modes): no hanging voice, <= 16 active, every note-on gets its release; steal order; last-note priority; deferred note-off (both pipeline orders = connected); correction at 39 / 41 ms and with the flag; glide flags |
| SynthVoice | control protocol (command mid-tick == command at tick with the same samples: identical output), glide time, legato without restart, correction without glide, steal fade 2 ms, exact 0 after release |
| SynthEngine | **bit-identical** for blocks {1, 7, 64, 480, 512, 4096} x {44.1, 48, 96 kHz} with notes, bends, expression, glide, LFO, all FX; allocation counter = 0 in process (as `CaptureTapTests`); silence -> exact 0 and no active voices after the tails; bend with bendRange 2 / 12 / 48; expression interpolation; table swap during a note without click; synthOn fade; benchmarks typical / worst / silence (18) |
| Chorus / StereoDelay / FdnReverb | mix law, on/off fade, bypass, delay time + sync, feedback bounded at 95 %, ping-pong, RT60 vs size ±20 %, tails end in exact 0, NaN guards |
| ExpressionFrame / Tracker | buffer capacity, publisher; level mapping vs rmsDb and smoothing times; brightness of a sine sweep monotonic; noise > tone in noisiness; pitch cents of a detuned tone |
| VoicePipeline overload | events + timings bit-identical with and without expression for the M2 test signals; one frame per hop; frame offsets block-size invariant |
| SynthParameters | 101 parameters after `recordBars`, hint 4, unique, defaults, choice lists, texts, `isPresetParameter` (done in the scaffold) |
| StateFixture | 0.3.1 FX / Instrument fixtures (done in the scaffold); later 0.4.0 fixture with SYNTH + embedded table |
| SynthSettingsBuilder | every parameter reaches its field (set each parameter to a non-default, compare) |
| WavetableStore | retire protocol (fake audio thread), stress 1000 swaps while rendering, releaseAllRetired, sharing by key |
| WavetableWorker | factory / import / embedded, newest request wins, errors, paths with spaces and umlauts |
| SynthState | round trip incl. user table (data once per hash), broken / missing children and data, stateVersion |
| PresetManager | file round trip, missing / unknown IDs, never touches mix / voice / capture parameters, modified flag, prev / next wrap, user folder |
| FactoryPresets / Golden | 24 presets, IDs valid, values in range, >= 8 with voice sources, tables exist; fingerprint (RMS, peak, centroid, flatness per 100 ms; held C4 2 s, legato run C4 E4 G4 C5 at 0.25 s, C-E-G chord 1.5 s for Poly; fixed expression ramp) vs `tests/data/golden/presets-0.4.json` with tolerances (RMS ±1.5 dB, centroid ±10 %, flatness ±0.05); peak <= -1 dBFS, no DC, not silent, no NaN |
| SynthPanel / UI | both variants, every synth parameter has exactly one control, controls inside bounds and not overlapping, preset bar, meters; widgets attach to parameters |
| Processor (integration) | synth audible in FX and Instrument, dryLevel / muteDry, 0 dB dry bit-identical to 0.3.1, no hanging notes on bypass / flush, state round trip with user table, preset load sets no voice parameter |
| Validators (gates) | pluginval strictness 10 (GUI) both VST3, Steinberg validator, clap-validator both CLAP (known `state-invalid-random` warning only), with 129 parameters (check CI timeouts) |

## 22. Risks and open points

- Envelope closed forms need `exp` per segment change and tick: watch the voice-tick budget (18); precompute
  per-tick factors when settings are unchanged.
- MSVC vs clang floating point differ: only fingerprints cross platforms; invariance tests run per build.
- The deferred note-off makes Legato connected for the pipeline's retrigger order — intended (the voice is
  monophonic). A user who wants retriggered envelopes picks Mono.
- Host "reset to default" switches the synth off (13.3); documented in `SYNTH-TESTING.md`.
- 129 parameters in FL's wrapper list and pluginval run time: checked in the integration gate.
- Open for the critique agent: crossfade length of mip changes (one tick) vs the click test; whether a bar
  should follow the host time signature (0.4: always 4 quarters).
