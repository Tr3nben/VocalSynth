# M3 Spec – Capture and Drag & Drop (binding input for the M3 workflow)

Status: draft, written 2026-10-16 on branch `m2.1-polish`; waits for the user's answers to the open questions at
the end. Work branch: `m3-capture` (from `master` after v0.2.1). Target version: **0.3.0**.
Refines docs/PLAN.md section 3 ("M3 – Capture und Drag & Drop") and section 2.3 (capture data flow). The design
agent turns this into `docs/M3-DESIGN.md` and a compilable scaffold.

## Goal

FL Studio does **not** record the MIDI that a plugin sends into the piano roll (M1 result, docs/FL-ROUTING.md).
VocalSynth therefore records every take itself. The user sings; afterwards he drags the take as a `.mid` file
into a channel / pattern (piano roll) and, if he wants, the dry voice as a `.wav` into the playlist. The notes
land where he sang them on the FL grid: the detection delay of the pitch tracker and the audio round trip of
the sound card (his ASIO buffer is 2048 samples) are removed in the exported files, so the recorded timing is
better than what he hears live.

Model: Vochlea Dubler 2 "MIDI Capture" (plugin stays armed, takes in a clip history, Raw/Clean versions, tempo
synced to the host, drag & drop), plus the "never lose a good idea" part of Ableton "Capture MIDI" (always
listening; grab what was just played even if nothing was armed).

## Facts this spec builds on

- M1 (`plugin/TestClipExporter.*`, `VocalSynthEditor::FileDragSource`): `.mid` (type 1, 960 PPQ, track name,
  tempo + 4/4 meta at tick 0, end-of-track) and `.wav` (24 bit) are written to `<temp>/VocalSynth/` through a
  temporary file + move (FL may still hold an earlier file open; fallback: numbered sibling), then handed to
  `juce::DragAndDropContainer::performExternalDragDropOfFiles`. Dragging into the channel rack / piano roll and
  the `.wav` into the playlist works in FL Studio 2026 (M1 user test).
- FL imports a dropped `.mid` into the pattern (channel rack / piano roll target); the pattern is then placed in
  the playlist. FL only accepts the `.mid` extension. Shift while dropping skips FL's MIDI import dialog, Alt
  forces it. When a file is dropped into an existing project, FL does **not** take over the file's tempo; the
  notes are placed by beats (ticks / PPQ). CC data is not imported as note data (FL needs the event editor for
  it), and FL is reported to assume a ±2 semitone bend range on import. (Sources: Image-Line manual "MIDI Import
  tool"; user reports; to be verified in the FL checklist below.)
- A dropped `.wav` is **referenced** by the FL project, not copied. A WAV in a temp folder that VocalSynth later
  deletes breaks the user's project. A dropped `.mid` is imported (copied into the project).
- The live path already emits `VsEvent`s with sample offsets; `MidiOutputStage` (stuck-note guard) turns them
  into MIDI. The VoicePipeline keeps an absolute sample clock since `reset()`. Note-on delay (v0.2.1 model,
  48 kHz, worst case at f0 min): Tenor ~23 ms, Bass/Wide ~38 ms; higher notes are faster (fast onset path).
- FL can send blocks of any size, including 1 sample; `prepareToPlay` gets the maximum block size.
- The user runs ASIO at 2048 samples. Singing along to the beat, his voice reaches the plugin about one input +
  one output buffer late relative to the playhead position of the block (≈ 85-95 ms at 48 kHz). FL compensates
  its own audio recordings for this; a plugin cannot query the driver latency through VST3/CLAP.

## Scope

In: always-on capture of the plugin's MIDI output (and optionally the dry voice), take segmentation,
retroactive grab, latency compensation, host tempo/bar sync and free mode, export of Raw/Clean MIDI with optional
quantize, WAV export, Capture tab with take list and drag handles, "Save as…", temp-file cleanup, tests, docs.

Out (later milestones or never): tempo detection from a free performance (Ableton-style, 80-160 BPM), MIDI
editing inside the plugin, comping several takes, saving takes in the project (see "State"), offline
re-analysis with pYIN/SwiftF0 (PLAN: later stage of CleanMidi), beatbox drum capture (M4 reuses this
machinery), dragging MIDI and WAV in one gesture, swing/groove quantize, changes to the virtual MIDI port.

## Architecture (binding split; names may be refined by the design)

Audio thread does only bounded, allocation-free copies. Everything else runs on a capture worker thread or the
message thread. `core/` stays JUCE-free; file writing lives in `plugin/`.

```
[audio thread]  processBlock
   ├─ mono input (the same mono buffer the VoicePipeline gets, before conditioning)
   ├─ events handed to MidiOutputStage::write (incl. flush note-offs)  ─┐
   ├─ transport info from getPlayHead()                                  ├─► CaptureTap (core, RT-safe)
   └─ capture requests (atomics from the UI: new take, grab, clear)   ─┘       │ SPSC rings (bounded)
                                                                                ▼
[capture worker thread, plugin/]  drains every ≤ 20 ms ─► CaptureHistory (core): event history + audio spool
                                                          TakeSegmenter (core): take boundaries
                                                          closed take ─► background WAV render into cache
[message thread]  Capture tab, export options, TakeRenderer (core) ─► CaptureExporter (plugin): .mid / .wav,
                  drag (performExternalDragDropOfFiles), Save as…, cleanup
```

### 1. `SpscRing<T>` (core)

- Single producer (audio thread), single consumer (worker), power-of-two capacity fixed in `prepare()`,
  acquire/release atomics, `push` never blocks or allocates and returns false when full, bulk push/pop for
  sample blocks. `std::atomic<size_t>` indices must be lock-free (`static_assert`).
- Two rings per instance: **records** (capacity ≥ 16384) and **audio** (capacity ≥ 2 s at the prepared rate,
  rounded up to a power of two; 192 kHz included).
- Overflow: the dropped item is counted (atomic overflow counter per ring, shown in the Capture tab
  diagnostics); the next record that fits is a `gap` record so the worker marks the take "incomplete". A take
  with a gap still exports. Acceptance: the counter stays 0 in the 10-minute test.

### 2. `CaptureTap` (core, audio thread side)

Called once per processed chunk with the chunk's mono samples, the VsEvents sent to the output, and a
`TransportInfo` value (plain struct filled by the plugin from `juce::AudioPlayHead::PositionInfo`: isPlaying,
ppqPosition, bpm, timeSigNumerator/Denominator, ppqPositionOfLastBarStart, each with a valid flag).

- Keeps its own absolute host-sample clock (since `prepare()`/`reset()`), independent of block size.
- Record types (trivially copyable, ≤ 32 bytes): `event` (absolute sample + VsEvent fields), `transport`
  (absolute sample + TransportInfo), `noteTiming` (see 6), `marker` (new take / grab / clear request with the
  absolute sample where the audio thread saw it), `gap`, `reset` (sample rate, after `prepare`/`reset`).
- `transport` records are pushed only on change: play state change, bpm or time signature change, valid flags
  change, or a ppq discontinuity (|ppq − predicted ppq| > 1e-6 beats, predicted from the previous record by
  bpm and elapsed samples; loop wrap, position jump). Otherwise the worker predicts ppq. This keeps tiny FL
  blocks cheap and makes the record stream block-size invariant for a host that reports a consistent transport.
- Audio: mono float at the host rate, every sample, only if `captureAudio` is on.
- Real-time rules as everywhere: no allocation, locks, I/O, logging or exceptions; `noexcept`; cost < 1 % of a
  128-sample block at 48 kHz (benchmark test).
- **Block-size invariance (binding):** for the same input and the same per-sample transport, host blocks
  {1, 7, 64, 480, 512, 4096} at 44.1 / 48 / 96 kHz give an identical record stream (after removing block
  boundaries) and byte-identical exported `.mid` files.

### 3. `CaptureHistory` (core data structure, owned by the worker)

- Always on while `captureEnabled` is on, also with the editor closed and with the transport stopped.
- Event history: ring of ≥ 2^18 records (≈ 10 minutes of True-mode bends at the 200 Hz rate limit), allocated
  on the worker thread when the worker starts (never on the audio thread).
- Audio history: `audioHistoryMinutes` (default 10) of mono audio. The design picks RAM chunks or a disk spool
  file in the instance folder (RAM at 48 kHz float: ~115 MB per instance for 10 minutes; a spool keeps RAM
  small). The design must state memory and disk use per instance in M3-DESIGN.
- Oldest data is evicted first; a take whose start was evicted is dropped from the list (or its audio is marked
  truncated if only audio was evicted). History is cleared by "Clear", by `reset` records (sample-rate change)
  and when the plugin is destroyed.

### 4. `TakeSegmenter` (core, pure, worker thread)

Consumes the record stream and decides take boundaries. Rules (binding):

1. **Synced mode** (transport playing): a take starts when the transport starts and ends when it stops. Loop
   wraps and position jumps while playing do **not** split the take; the take's beat timeline is unwrapped
   (beats keep counting, see 5), so a melody sung across two passes of a 4-bar pattern loop stays one take.
2. **Free mode** (transport stopped or no playhead): a take starts at the first note-on and ends after
   `takeGapSeconds` (default 3 s) without any sounding note, or when the transport starts.
3. **Manual "New take"**: closes the current take at the marker sample (a note held across it is closed at the
   marker in the old take and re-opened at the marker in the new one) and starts the next take there.
4. A `reset` record (prepare, sample-rate change) closes the current take.
5. Takes without any note-on are discarded (not listed). A take that started while a note was already sounding
   (manual split, transport start mid-note) opens that note at the take start.
6. Max takes in the list: 32 (oldest dropped). Each take stores: number, start/end absolute sample, mode
   (synced/free), tempo map, start bar, note count, incomplete flag, audio availability.

**Retroactive grab** ("Grab last …"): creates a take from the history regardless of the rules above.
Synced: the last N bars (choice 1, 2, 4, 8, 16, 32; default 8): from the bar start N − 1 bars before the
current bar up to the grab moment (the current bar is included as far as it was played).
Free: the last N seconds (10, 20, 30, 60, 120; default 30). The grab is a marker record, so it is timed by the
audio clock, not the UI clock. Grabbed takes are listed like other takes ("Grab").

### 5. Time base, tempo and bar sync (core `TakeTimeline`)

- Every record has an absolute host sample. The timeline maps sample → musical time (beats, unwrapped)
  using the transport records: beats advance by bpm × elapsed samples / (60 × sampleRate) from the last record;
  a ppq discontinuity re-anchors the bar grid but not the unwrapped beats. Tempo changes are honoured from the
  sample where they were reported (tempo automation in FL → piecewise-constant tempo map).
- **Latency compensation (binding):** the musical time of a captured sample s is taken at
  `s − roundTripSamples` (both MIDI and audio), and of a note event additionally at its detection delay (6).
  `roundTripSamples` = the capture timing offset: parameter `captureOffsetMode` Auto = 2 × the maximum block
  size from `prepareToPlay` (estimate of ASIO input + output buffer), Manual = `captureOffsetMs`; Off = 0.
  In free mode only the detection delay applies (no grid to align to).
- **Synced export origin:** tick 0 = the start of the bar that contains the take's first (compensated) note-on,
  using the host's bar position (`ppqPositionOfLastBarStart`; fallback: bars of the current time signature
  counted from ppq 0). The file name and the take list show that bar number in FL convention (first bar = 1).
  Ticks = (beats since origin) × PPQ, so the notes sit on the FL grid when the pattern is placed at that bar,
  whatever the project tempo. The tempo meta event(s) mirror the host tempo map (informational for FL).
- **Free export origin:** tick 0 = the first compensated note-on minus `freePreRollMs` (default 0); ticks from
  seconds at the host bpm (the playhead usually reports bpm while stopped), else 120 BPM; 4/4.
- PPQ 960 (as M1). Type 1 file with a single track (track name, tempo, time signature, notes, optional bend /
  CC, end-of-track at the end of the last bar that contains an event, synced; or right after the last event,
  free).

### 6. Detection-delay compensation (core, spec of the result, method chosen by the design)

The live MIDI is late by the analysis delay, and that delay depends on the pitch and on which onset path fired
(fast/provisional vs full window). Exported timing must not carry it.

- The VoicePipeline gains a side output (no change to `VsEvent` or the live MIDI): for each note-on the absolute
  sample of the estimated tone onset, for each note-off the estimated tone end, and for bends the analysis
  delay of the frame. The CaptureTap records these as `noteTiming` records. Fallback where no estimate exists
  (test pattern, flush): the model delay (`computeFastOnsetLatencyMs` / `computeConfirmedLatencyMs`) or 0 for
  the test pattern.
- If audio was captured, `CleanMidi` may refine each note-on onto the energy onset in the dry audio within
  [estimate − 15 ms, emit sample].
- A provisional note that the full path later corrected (v0.2.1 fast onset) is exported as the corrected note
  from the provisional start in Clean; Raw keeps the events as sent.
- **Acceptance (PLAN):** in the export, MIDI vs audio offset ≤ 1 analysis hop (~2-3 ms) for synthetic voiced
  onsets with a known start, all ranges, both algorithms, 44.1/48/96 kHz (median; 95th percentile ≤ 2 hops).

### 7. `TakeRenderer` (core, pure) and export options

Turns a take into a sorted list of tick-stamped MIDI-like events (JUCE-free), applying in this order:
compensation (5, 6) → version → quantize → pairing/clamping. Rules: every note-on has a note-off inside the
take, no overlapping notes of the same pitch on the same channel (the earlier one ends at the later start),
minimum note length 1 tick, bends reset to centre after the last note, events sorted with note-offs before
note-ons on equal ticks.

| Option | Values | Default |
|---|---|---|
| Version | Raw (exactly what was sent, compensated), Clean | Clean |
| Quantize | Off, 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 (synced takes only; disabled in free mode) | Off |
| Quantize strength | 0-100 % (moves note starts this far towards the grid) | 100 % |
| Quantize ends | off / on (note ends to the grid, length ≥ half a grid step) | off |
| Pitch bend | off / on (exports bends; writes RPN 0 bend range = `bendRange` at tick 0) | on if `bendMode` ≠ Off |
| CC | exported when present (Voice mode sends none in 0.3.0; test pattern does) | on |
| Audio | WAV of the dry voice, same origin as the MIDI (tick 0 = sample 0 of the WAV) | on |

**Clean, first stage (PLAN):** remove ghost notes shorter than `cleanMinNoteMs` (default 80 ms, never below
`minNoteLength`); merge transition notes (a note < 120 ms between two notes, legato or with a gap ≤ 15 ms, whose
pitch lies between its neighbours: its time goes to the following note); remove octave strays (a note
< 200 ms that is ≥ 11 semitones from both neighbours). Thresholds are initial values, tuned with tests and the
user's takes. Clean never moves notes in time by more than the merged span.

Export options are remembered per project (state, not automatable parameters) and apply to every drag until
changed; changing them re-renders the selected take (MIDI synchronously on the message thread, < 50 ms for a
10-minute take; WAV is independent of the options except the origin).

### 8. `CaptureExporter` and files (plugin, message thread + worker)

- MIDI: written on drag start like the M1 exporter (temp file + move, numbered sibling fallback), via
  `juce::MidiFile`. Reuse/factor the M1 helpers instead of copying them.
- WAV: mono, 24 bit, host rate, sample-exact length of the take span. Rendered by the worker into the instance
  cache when a take closes (so the drag never waits for a 10-minute file); the drag handle shows "preparing…"
  until it is ready. On drag/save the WAV is moved (rename; copy as fallback) into the persistent takes folder,
  because FL references dropped WAVs.
- Names (ASCII, no spaces): `VocalSynth_<yyyy-MM-dd_HHmmss>_Take<NN>_Bar<BBB>_<Raw|Clean>[_Q16].mid` (free:
  without `_Bar`), `VocalSynth_<…>_Take<NN>_Bar<BBB>_Voice.wav`. The timestamp is the take start.
- Locations: MIDI and the WAV cache in `<temp>/VocalSynth/Capture/<instanceId>/`; dragged/saved WAVs in
  `<Documents>/VocalSynth/Takes/` (never deleted by VocalSynth).
- Cleanup: the instance folder is deleted when the processor is destroyed; at start-up, other instance folders
  whose heartbeat file (touched by the worker every 60 s) is older than 24 h are deleted. The M1 test clips in
  `<temp>/VocalSynth/` stay as they are.
- "Save as…": async `juce::FileChooser` for the selected take, MIDI and/or WAV, with the current options.

### 9. Plugin integration

- `processBlock`: the CaptureTap is fed in both sources (Voice and Test Pattern), FX and Instrument (Instrument:
  sidechain mono), after the events of the chunk are final (including flush note-offs). Monitor events are not
  captured. Bypass: no events, audio of the bypassed blocks is not captured (gap record).
- The worker thread starts in the first `prepareToPlay` and stops in the destructor (not in
  `releaseResources`, so the history survives FL's stop/start of the audio engine); `prepareToPlay` with a new
  rate pushes a `reset` record. pluginval, the Steinberg validator and clap-validator must still pass
  (instances must start and stop the worker quickly; no thread before `prepareToPlay`).
- New parameters (`juce::ParameterID`, IDs permanent; version hint per JUCE guidance for parameters added after
  a release, decided in the design):

  | ID | Type | Values / range | Default |
  |---|---|---|---|
  | `captureEnabled` | bool | off / on | on |
  | `captureAudio` | bool | off / on | on |
  | `captureOffsetMode` | choice | "Auto", "Manual", "Off" | Auto |
  | `captureOffsetMs` | float ms | 0-300 | 0 (used in Manual) |

  Export options, grab length, `takeGapSeconds`, `audioHistoryMinutes` and the takes folder are non-parameter
  state (child tree of the APVTS state, migrated like M2 state; old projects load with defaults).
- Reported host latency stays 0 (live path unchanged).

### 10. UI – "Capture" tab (third tab beside "Voice" and "Test & Routing")

- Status line: "Synced 128.0 BPM 4/4 – recording take 3 (bar 17)" / "Free – waiting for a note" / "Capture
  off"; overflow counters only when non-zero.
- Take list (newest on top): number, start (bar or time), length (bars or seconds), note count, Grab/
  incomplete badge, a mini piano-roll of the notes (auto pitch range, Clean/Raw as selected) and, with audio, a
  waveform strip under it. Selecting a take selects it for the drag handles.
- Drag handles "MIDI" and "WAV" (the M1 `FileDragSource` behaviour; tooltip mentions Shift = skip FL's import
  dialog), "Save as…".
- Buttons: "New take", "Grab last [N bars / N s]", "Clear" (with confirmation), export options from 7, capture
  timing offset (mode + ms, showing the Auto value in ms), audio on/off.
- Hint text: "Takes are not saved with the project – drag or save what you want to keep."
- Repaint at ≤ 10 Hz and only when the take list changed (the user found 30 Hz tuner updates hectic). No take
  data is read from the audio thread by the UI; the UI reads the worker's take list under a lock that the audio
  thread never touches.
- Version text "0.3.0 – M3 capture".

### 11. State: takes are not saved in the project (decision)

The take history lives only for the plugin instance's lifetime. Reasons: (1) audio takes are tens to hundreds of
MB and FL calls `getStateInformation` for every save and autosave, which would bloat and slow the `.flp`;
(2) the result of a take is meant to leave the plugin: dragged MIDI is copied into the FL project, dragged
WAVs are referenced from the persistent takes folder; (3) restoring takes would need a versioned binary format
and migration. The capture settings (parameters and export options) are saved. The UI says so (hint text).

## Tests (Catch2; all in CI)

- `SpscRing`: wrap-around, full/empty, bulk push/pop, overflow count, producer/consumer stress on two threads
  (≥ 10^7 items, checksum).
- `CaptureTap`: no allocation (allocation counter around `process`), transport records only on change,
  block-size invariance {1, 7, 64, 480, 512, 4096} × 44.1/48/96 kHz with a synthetic playhead (constant tempo,
  tempo change, loop wrap, stop/start), identical record streams.
- `TakeSegmenter`: transport start/stop, loop wrap keeps one take, free-mode gap (just below / above
  `takeGapSeconds`), manual new take inside a held note, empty takes discarded, reset closes the take, history
  eviction, 32-take limit, grab N bars (synced) and N seconds (free).
- `TakeTimeline` / `TakeRenderer` timing: exact ticks for notes at known beats at 60, 97.5, 120, 174 BPM and
  PPQ 96 / 480 / 960; tempo change inside a take; 3/4 and 7/8 bar origins; bar origin after a loop wrap; free
  mode at host bpm and at the 120 BPM fallback; round-trip offset (Auto from max block 2048 at 48 kHz =
  85.3 ms) moves notes by the expected ticks.
- Quantize: each grid incl. triplets, strength 0 / 50 / 100 %, ends on/off, no zero-length or reordered notes,
  same-pitch overlap rule; free takes are never quantized.
- Clean: ghost, transition and octave-stray cases each removed/merged, neighbours untouched; Raw unchanged.
- Detection-delay compensation: synthetic tones (sines, saws, voice-like corpus items) with known onsets through
  VoicePipeline → capture → render: acceptance in 6 per range, algorithm and rate.
- `CaptureExporter` (plugin): files readable by `juce::MidiFile`, PPQ, tempo/time-signature meta at tick 0,
  RPN bend range when bends are on, end-of-track rule, MIDI pairing invariants; WAV length = take span and WAV
  sample 0 = tick 0; names; temp + move with a locked target (numbered sibling); cleanup of stale folders.
- Processor: 10-minute simulated take (Voice and Test Pattern, worker draining in real time is not required:
  the test drives the worker step manually) with overflow counters 0; capture on/off; state round trip of the
  new parameters and export options; pluginval 10, Steinberg validator, clap-validator, auval (macOS manual).
- CPU benchmark: CaptureTap cost < 1 % of a 128-sample block at 48 kHz; total worst block still < 20 %.

## FL Studio test checklist (becomes docs/M3-TESTING.md, German)

1. Project 120 BPM, metronome on, FX on the mic insert, ASIO 2048 (the user's setting) and 256 for comparison.
   Play, sing quarter notes on the beat for 4 bars, stop. One take appears with the right bar number.
2. Drag "MIDI" (Clean, quantize off) onto the Vital channel in the channel rack and into an open piano roll;
   place the pattern at the take's bar. Notes on the beats within a few ms at both buffer sizes (Auto offset)?
   Also with Shift (no import dialog) and Alt (dialog).
3. Same with quantize 1/16 at 100 % and 50 %; with Raw; with bends on (True bend mode): does FL import the
   bends, and with which range?
4. Drag "WAV" into the playlist at the same bar: voice and synth pattern line up. Save, close and reopen the
   project: the audio clip is still there (persistent takes folder).
5. Tempo 90 and 140 BPM, and a tempo change in the middle of a take; 3/4 time signature.
6. Pattern mode with a 4-bar loop, sing 8 bars across two passes: one take of 8 bars.
7. Free mode (transport stopped): sing two phrases with a 5 s pause: two takes; drag one into a pattern.
8. Sing something without watching the plugin, then "Grab last 8 bars" / "last 30 s".
9. "New take" in the middle of a held note; "Clear".
10. 10-minute take: no dropouts, overflow counters 0, UI stays responsive; drag the WAV.
11. CPU in FL's performance view with and without capture (VocalSynth alone, without Vital); RAM per instance.
12. Two VocalSynth instances at once; close FL and check that `%TEMP%\VocalSynth\Capture` is cleaned up.

## Docs

`docs/M3-DESIGN.md` (English), `docs/M3-TESTING.md` (German, checklist above with expected results and what
to report), LIESMICH.txt update (CRLF), README status, PLAN.md M3 checkboxes, CI package includes
`docs/M3-TESTING.md`, STATUS.md.

## Acceptance (from PLAN.md, made concrete)

- Drag & drop into FL works for pattern (channel rack / piano roll) and playlist (WAV), position correct.
- MIDI vs audio offset in the export ≤ 1 hop (section 6).
- No audio dropout in 10-minute takes; FIFO overflow counters stay 0.
- Block-size invariant capture and byte-identical `.mid` exports (section 2).
- All existing gates stay green.

## Decisions (confirmed by the user on 2026-10-16)

The user chose "alle 6 übernehmen" for the open questions below, so these are binding for M3:

1. MIDI file starts at the bar where the take starts (bar number in the file name).
2. Voice is captured as WAV too: on by default, 10 minutes look-back, dragged WAVs kept in `Documents\VocalSynth\Takes`.
3. Latency compensation "Auto" (2 × buffer) plus a fine offset in ms; clap calibration later (not in M3).
4. Takes are not saved in the project, only the capture settings.
5. Drag default: Clean, no quantize, pitch bend as configured in the plugin.
6. Free mode: a pause of 3 s (adjustable) starts a new take.

## Open questions for the user (German; each with the recommended default)

1. **Ausrichtung der MIDI-Datei:** Soll die Datei am Taktanfang des Takes beginnen (Dateiname enthält die
   Taktnummer, du legst das Pattern an diesen Takt) oder immer ab Takt 1 des Songs (passt sofort, aber das
   Pattern hat vorne leere Takte)? *Empfehlung: Taktanfang des Takes.*
2. **Stimme als WAV mitschneiden:** Immer mitschneiden (10 Minuten Rückblick, bis ca. 115 MB pro Instanz
   bei 48 kHz) und gezogene WAVs in `Dokumente\VocalSynth\Takes` ablegen? Oder anderer Ordner / standardmäßig
   aus? *Empfehlung: an, 10 Minuten, `Dokumente\VocalSynth\Takes`.*
3. **Latenzausgleich der Aufnahme:** Bei ASIO 2048 kommt deine Stimme ca. 85-95 ms nach dem Beat im Plugin an.
   Reicht „Auto“ (2 × Puffergröße) plus ein Feinregler in ms, oder wünschst du eine Kalibrierung (8-mal zum
   Metronom klatschen, das Plugin misst den Versatz)? *Empfehlung: Auto + Feinregler jetzt, Kalibrierung später.*
4. **Takes im Projekt speichern:** Takes gehen beim Schließen des Projekts verloren (Gründe: Projektgröße,
   Autosave). Ist das in Ordnung? *Empfehlung: nicht speichern, nur die Einstellungen.*
5. **Standard beim Ziehen:** Clean ohne Quantisierung, Pitchbend nur wenn Bend Mode an ist? Oder lieber
   gleich auf 1/16 quantisiert? *Empfehlung: Clean, Quantisierung aus, Pitchbend wie im Plugin.*
6. **Neuer Take im Freimodus (Transport gestoppt):** Nach wie vielen Sekunden Pause beginnt ein neuer Take?
   *Empfehlung: 3 Sekunden (einstellbar).*
