# M3 design: capture and drag & drop (v0.3.0)

Status: implemented in 0.3.0 (branch `m3-capture`, integration 2026-10-17; changes made during the
implementation are listed in 19). Written as design + compilable scaffold on 2026-10-16 and reviewed the same
day (corrections listed in 1.2 and applied throughout this document and the headers). Binding input:
[docs/M3-SPEC.md](M3-SPEC.md) incl. "Decisions (confirmed by the user on 2026-10-16)". Rules:
`.claude/skills/juce-audio-plugin-dev/SKILL.md`, [docs/M1-DESIGN.md](M1-DESIGN.md),
[docs/M2-DESIGN.md](M2-DESIGN.md) (sections 10 and 11 describe the v0.2.1 pipeline this builds on).

The **core headers** in `core/include/vocalsynth/` are the frozen interfaces of this milestone; bodies marked
`TODO(A|B|C)` are stubs. Already implemented (they define the design): the record wire format
(`CaptureRecord.cpp`), the timing buffers (`EventTiming.h`), the capture options and round-trip formula
(`CaptureOptions.cpp`), the shared consumer loop (`CaptureDrain.cpp`), the M3 parameters
(`plugin/Parameters.h`) and the shared file helpers (`plugin/ExportFiles.cpp`). Plugin-side interfaces (`plugin/Capture*.h`) are owned by agent C and may be
refined by C. The scaffold does not change the plugin's behaviour: nothing calls the capture code yet; the
only visible change is five new (unused) parameters.

Notation: `sr` = host sample rate, "clock" = the CaptureTap's absolute host-sample counter, `rt` =
round-trip (capture timing) offset in samples, `U` = unwrapped beats (quarter notes), PPQ = ticks per
quarter note (960), "hop" = analysis hop of the VoicePipeline (~2 ms).

## 1. Goal and scope

FL Studio does not record plugin MIDI. VocalSynth therefore keeps an always-on capture of what it sends
(and of the dry voice), cuts it into takes, and offers each take as a `.mid` (drag into a pattern / piano
roll) and `.wav` (drag into the playlist). Exported timing removes the analysis delay and the sound card's
round trip, so the notes land where they were sung. Scope and out-of-scope list: M3-SPEC "Scope".

Decisions applied (M3-SPEC, binding): (1) the MIDI file starts at the bar where the take starts, bar number
in the file name; (2) voice WAV on by default, 10 min look-back, dragged WAVs kept in
`Documents\VocalSynth\Takes`; (3) latency compensation Auto = 2 x buffer plus a fine offset in ms;
(4) takes are not saved in the project, only the settings; (5) drag default Clean, no quantize, pitch bend
as configured in the plugin; (6) free mode: 3 s pause (adjustable) starts a new take.

### 1.1 Refinements of the spec (decided here, with reasons)

| Spec | Design | Why |
|---|---|---|
| Separate `noteTiming` records | The timing travels **inside the event record** (`VsEventTiming`: delay, kind, flags) | One record per event instead of two, no pairing logic in the worker; 32 bytes still suffice |
| Audio ring as a plain sample stream | Fixed **audio frames** of 256 samples (aligned to the clock) + one `audio` record per frame | The worker places audio only through records, so a dropped frame can never shift later audio; frames make the audio stream block-size invariant; ~190-750 records/s instead of one per 1-sample block |
| `.mid` via `juce::MidiFile` | JUCE-free **`SmfWriter`** in core produces the bytes; the plugin writes them (temp + move) | Byte-identical exports are testable in the core tests (block-size invariance gate); deterministic byte layout; the plugin tests still read every file back with `juce::MidiFile` |
| WAV via `juce::WavAudioFormat` | JUCE-free **`WavWriter`** (header + streaming encoder), 24-bit default, 32-bit float option | Same reasons; streams a 10-minute take through a small buffer on the worker |
| Four parameters | **Five**: `captureFineOffsetMs` (-100..+100 ms) added to Auto | Decision 3 asks for "Auto plus a fine offset"; `captureOffsetMs` keeps its spec meaning (Manual value, 0-300 ms) |
| "Audio" export option | Mapped to the `captureAudio` parameter (no WAV handle when off) | Avoids two switches for the same thing |
| Pitch-bend option on/off with a computed default | Three values: **As plugin** (default), Off, On | Decision 5 as a stored value: the default follows each take's recorded bend mode |
| History cleared by every `reset` record | Only a **clearing** reset (first prepare or a new sample rate) clears; prepare with the same rate keeps the clock and the history and only closes the open take | "The history survives FL's stop/start of the audio engine" (spec 9) |
| Takes reference the history | Every take **owns a copy** of its events and a timeline slice | Takes stay valid when the history evicts; the UI reads immutable `shared_ptr<const Take>` without locking the worker for long |
| Bypass: audio not captured, gap record | Bypassed blocks are captured as **silence frames** with markers `bypassOn/Off`; no gap | A gap means "data lost" and marks the take incomplete; a bypass is a deliberate silence, the take's audio stays contiguous |
| Synced take = transport start .. stop (emit time) | Span in **host time**, events collected for a **capture tail** after the end (section 5.2) | What is sung just before Stop reaches the plugin a round trip (up to ~300 ms) + detection delay later; without the tail those notes are cut |
| Parameter version hint (open in the spec) | **2** for the M3 parameters | JUCE: parameters of a later release need a higher hint (AUv2 index order); M1/M2 all use 1 |

### 1.2 Design review (2026-10-16): corrections to the first draft

Applied in this document and in the headers / stubs; implementers follow the corrected text.

1. **Capture tail** (5.2, 9): synced takes and grabs keep collecting events for
   `TakeSegmenter::compensationTailSamples()` = largest possible round trip + 250 ms after their end; the
   renderer clamps compensated times into the take's host-time span. Before: the last ~85-300 ms of singing
   before Stop / a split were cut, and notes arriving just after the transport start (sung before it) were
   kept at their compensated time in the stopped part of the timeline (all on one beat).
2. **Tap = what MidiOutputStage sends** (4): velocity-0 note-ons, note-offs of closed notes and duplicate
   note-ons are normalised exactly like the output stage, so Raw is what the synth got and the tap's
   open-note set mirrors the output (flush note-offs match). A same-rate `prepare()` keeps the open notes
   (MidiOutputStage keeps them and flushes them after `prepareToPlay`).
3. **Bounded transport records** (4.3): bpm-only changes **and** small position deviations (jittery hosts,
   hosts without bpm, tempo inside a block) are one rate-limited "re-sync" class (10 ms, <= 100 records/s);
   only structural changes (play state, flags, time signature, jumps >= 0.5 beats) go at once. The bar start
   is predicted across bar lines (else every bar line inside a block is a discontinuity, which breaks block
   invariance) and compared with 1e-4 beats (it travels as a float).
4. **Overflow resync** (4.4): after a gap the tap re-states every open note (note-on, flag `resync`) and
   forces a transport + settings record; the segmenter ends all sounding notes at the gap first. Before: a
   dropped note-off left a note sounding forever, so a free take never ended.
5. **Shared origin with refinement** (9): the origin uses the first note-on's refined sample when the worker
   found one (for Raw too), so Clean's refined first note never clamps to tick 0 (the <= 1 hop acceptance
   applies to the first note as well).
6. **Timeline** (6): "playing" needs a host position (`isPlaying && hasPpq`); U is monotonic; a synced take's
   timeline starts with its playing segment and is kept by the segmenter while the take is open (the
   history may evict the start of a long take); earlier samples extrapolate with that segment's tempo.
7. **Round trip per take** (8): it depends on each take's max block and rate, so the worker and the
   message thread keep `CaptureOffsetSettings` and use `TakeRenderer::roundTripFor()`; the worker reads
   the three offset parameters from their raw atomics (no listener, no lock). `setWavOptions (RenderOptions)`
   is gone.
8. **Incremental WAV writing** (11, 12): `CaptureExporter::WavFileWriter` (open / write / finish / abort), at
   most 65536 frames per worker step. The one-call `writeWav()` would block draining for seconds on a
   10-minute take and keep `stop()` from returning.
9. **Memory bound of the take list** (5.1): takes own their events, so 32 long takes could hold ~400 MB;
   now at most 2^20 events (~48 MiB) in all, oldest takes dropped first.
10. **Shared drain** (5.3): `core/CaptureDrain.*` (owner A) is the one consumer loop for the worker and the
    core end-to-end tests, so the byte-identical gate tests the worker's real path.
11. **Testability / lifetime** (15): `CaptureWorker::setManualSteppingForTests()` (the processor test's
    `prepareToPlay` would otherwise start a thread that races `runStepForTests()`); `stop()` waits without
    `juce::Thread`'s timeout kill; instance folder created lazily; `closeIdle()` closes a free take when the
    host stops calling `processBlock`; markers coalesce to one per kind per block; 0-sample blocks do not
    call the tap.
12. **Integration without waiting** (7.1): the `VoicePipeline` timing overloads exist now as forwarding stubs
    (events identical, no timings = kind none), so C can wire the processor before B lands.
13. **SMF bytes** (10): exact meta encodings (`FF 58 04 nn dd 18 08` with dd = log2, `FF 51 03`), M1 order at
    tick 0 (name, tempo, time signature), end-of-track rule for a note-off on a bar line.
14. **State** (13): `setStateInformation` removes the `CAPTURE` child before `replaceState` (else every
    save/load cycle adds one more), may run on a background thread (pluginval).
15. `CapturedEvent::refinedSample` uses `noSample` (INT64_MIN) instead of -1 (estimates can be negative
    near clock 0). `Take::observedBlockSize` records the largest host block seen (FL check of Auto).

## 2. Architecture and data flow

```
[audio thread] VocalSynthProcessor::processBlock / processBlockBypassed            (plugin, RT-safe)
   transport = makeTransportInfo (getPlayHead())
   captureTap.beginBlock (transport, {captureEnabled, captureAudio, settings})   reset/transport/settings/markers
   [flush]  midiOutput.flushAllNotes (0, midi); captureTap.flushNotes()            note-offs of open notes
   per chunk (<= 4096):  mono = downmix (input)                                     (also in Test Pattern mode)
                         voicePipeline.process (mono, n, settings, events, eventTimings)  | generator.process
                         midiOutput.write (events ...)
                         captureTap.processChunk (mono, n, events, timings)          events + audio frames
   bypassed: beginBlock; flushNotes; processBypassed (n)                             silence frames
        |                                   |
        | SpscRing<CaptureRecord> 16384     | SpscRing<float> >= 2 s (2^n)
        v                                   v
[capture worker thread]  CaptureWorker::step() every 10 ms                          (plugin, juce::Thread)
   CaptureDrain::drain (core): clock = tap.getClock(); pop records (+ the audio frame of each audio record)
   CaptureHistory.addRecord / addAudio      (core: event ring 2^18, timeline, audio chunks in RAM)
   TakeSegmenter.process / advanceTo        (core: take boundaries, open / closing takes, pending grabs)
   closed take -> refineNoteOnsets (audio) -> take list (max 32 / 2^20 events, newest first) -> WAV cache job
   WAV cache: CaptureExporter::WavFileWriter (WavWriter) into <temp>/VocalSynth/Capture/<id>/ (<= 65536 frames/step)
   heartbeat file every 60 s; stale instance folders deleted at start
        |  std::mutex (never touched by the audio thread): shared_ptr<const Take> list, open-take
        v  snapshot (<= 2 Hz), WAV states, version counter
[message thread]  CapturePanel (10 Hz timer, repaint on change) -> TakeRenderer::render (core)
   drag "MIDI": CaptureExporter::writeMidi (SmfWriter bytes, temp + move) -> performExternalDragDropOfFiles
   drag "WAV":  CaptureExporter::persistWav (cache -> Documents/VocalSynth/Takes) -> drag
   Save as... (async FileChooser), New take / Grab / Clear -> captureTap.request...() (atomics)
```

Threads and what they own:

| Thread | Owns / may touch | Never |
|---|---|---|
| audio | `CaptureTap` producer side, its open-note set, the two rings' producer ends | allocate, lock, I/O, read takes |
| capture worker | `CaptureHistory`, `TakeSegmenter`, the WAV cache files, heartbeat, ring consumer ends | touch the editor |
| message | `CapturePanel`, `TakeRenderer` calls, MIDI files, persisting WAVs, `CaptureState` | read the rings or the history |

## 3. `SpscRing<T>` (core, header-only, owner A)

`core/include/vocalsynth/SpscRing.h`. Bounded single-producer / single-consumer ring of trivially copyable
items. Capacity = power of two set by `prepare (minCapacity)` (allocates, not RT). Indices are monotonically
increasing `std::atomic<std::size_t>` counters (`head` = pushed, `tail` = popped; full when
`head - tail == capacity`, no wasted slot), stored with `release`, loaded with `acquire`; each side caches the
other side's index and re-reads it only when the cache says full / empty (fewer cache-line transfers). Put
producer and consumer fields on separate cache lines (`alignas (64)`). `static_assert` lock-free atomics.

- `push`, `tryPushAll (span)` (all or nothing), `freeSpace` = producer; `pop`, `popBulk (span)`, `size` =
  consumer; `getCapacity`, `getOverflowCount` = any thread. A push that does not fit stores nothing and adds
  the dropped item count to `overflowCount` (relaxed atomic).
- `prepare` / `reset` are not concurrent with anything (section 15).
- Copies with `std::copy` / `memcpy` in at most two contiguous pieces (wrap-around).

## 4. `CaptureTap` (core, audio thread, owner A)

`core/include/vocalsynth/CaptureTap.h`. Called by the processor in this order per host block (binding):

```
tap.beginBlock (transport, blockSettings);     // block start sample: reset, transport, settings, markers
tap.flushNotes();                              // only when the processor flushes midiOutput
for each chunk: tap.processChunk (mono, n, events, timings);   // after the chunk's events are final
bypassed block: tap.beginBlock (...); tap.flushNotes(); tap.processBypassed (n);
```

A 0-sample block calls nothing (the processor returns before any MIDI, as today).

- **Clock**: absolute host samples. `prepare (sr, maxBlock)` restarts it at 0 only on the first prepare or a
  new rate (`clearsHistory`; open notes and the staged audio frame are forgotten too), otherwise it
  continues with the open notes and the staged frame kept (FL restarts its engine on buffer changes; the
  history survives; `MidiOutputStage` keeps its notes as well and the processor's flush after
  `prepareToPlay` turns them off, which `flushNotes()` records). `getClock()` is stored with `release` at the end of each `processChunk` / `processBypassed`, after
  the records were pushed, so a consumer that loads it (acquire) and then drains has every record with
  `sample < clock`.
- **Events**: sample = chunk start + `clamp (sampleOffset, 0, n - 1)` (same clamp as `MidiOutputStage`).
  Recorded **exactly as `MidiOutputStage::writeEvents` sends them** (binding): channel 1..16, data 0..127,
  bend -8192..8191 clamped; a note-on with velocity 0 is a note-off; a note-off of a note that is not open
  is dropped; a note-on of an open note is preceded by a note-off of it (same sample, same timing). The
  tap tracks open notes (16 x 128 bitset) and non-centred bends per channel the same way, for
  `flushNotes()` (= the output's `flushAllNotes()`), `captureOff` and the overflow resync. Timings:
  `timings[i]` belongs to `events[i]`; missing entries (shorter or empty span: test pattern, the scaffold
  pipeline) are kind none, delay 0. Monitor events are never passed in.
- **Audio**: only while `captureAudio` is on (and capture enabled). Mono float, the same downmix the
  VoicePipeline gets, before conditioning. Staged into one frame of `audioFrameSamples = 256`; frame k covers
  clock samples `[256 k, 256 k + 256)`. When audio is switched on mid-frame, the part before is zeros. A full
  frame is pushed with `audio.tryPushAll (frame)` + `records.push (makeAudio (256 k, 256))` only if both rings
  have room (checked with `freeSpace()` first; the producer's view is a lower bound, so the pushes cannot then
  fail), else the frame is dropped (`audio` overflow counter += 256) and a gap is pending. `nullptr` mono =
  silence. Bypassed blocks stage zeros (the take's audio stays contiguous; marker `bypassOn/Off`).
- **Disabled** (`captureEnabled` off): `beginBlock` at the on -> off transition sends note-offs for open notes
  (capture-only, not sent to the synth), pushes the staged partial audio frame (zero-padded) and marker
  `captureOff`; afterwards only the clock advances (the tap still tracks notes). Off -> on: marker
  `captureOn`, then a forced transport and settings record. Notes that were already sounding at
  `captureOn` are not re-opened (their later note-offs are unpaired; the renderer drops those).
- **Markers** from UI requests (`requestNewTake/Grab/Clear`: atomic counters + grab lengths, any thread): the next
  `beginBlock` pushes at most one marker per kind (requests made between two blocks coalesce; a grab carries
  the newest lengths) at the block start sample. `audioOff` pushes the staged partial frame (zero-padded)
  before its marker; the history ends the audio run at the marker sample.
- **Settings record** (`CaptureSettingsInfo`, on change): Voice = the voice settings of the block; Test
  Pattern = `testPattern` true, `bendMode = trueBend` (the pattern sends bends), `bendRangeSemitones = 2`
  (the pattern's and MonitorSynth's range), `minNoteLengthMs = 0`.
- **Real-time**: everything `noexcept`, no allocation / locks / I/O, bounded by events + samples. Cost target:
  < 1 % of a 128-sample block at 48 kHz (benchmark test).

### 4.1 Record order at one sample (binding)

`reset` -> `transport` -> `settings` -> markers (`captureOn/Off`, `audioOn/Off`, `bypassOn/Off`, then the
requests in the order newTake, grab, clear) -> flush note-offs -> events in emission order. A pending `gap`
record (with its resync note-ons, 4.4) is always pushed right before the next record that fits; the
transport and settings records it forces follow at the next block start. Audio records are pushed when their frame
completes (inside `processChunk`, at the frame's first sample; they may come after events of later samples in
the same chunk: the worker must not assume audio records are in sample order relative to events).

### 4.2 Record formats (`CaptureRecord`, 32 bytes, implemented in `core/src/CaptureRecord.cpp`)

Layout: `int64 sample, uint64 word0, uint64 word1, uint32 word2, uint8 type, uint8 byte0..2`; use the
`make...` / `as...` functions only.

| Type | Sample | Payload |
|---|---|---|
| `reset` | 0 (clearing) or the clock | sampleRate (double), maxBlockSize, clearsHistory |
| `transport` | block start | ppq (double), bpm (double), ppq - lastBarStart (float), flags (playing, hasPpq, hasBpm, hasTimeSig, hasBarStart, looping), numerator, denominator (1..255) |
| `settings` | block start | bendMode, bendRangeSemitones, channel, minNoteLengthMs (float), testPattern |
| `event` | emit sample | VsEvent (type, channel, data1, data2, bend), `VsEventTiming` (delaySamples int32, kind, flags) |
| `audio` | first sample of the frame | numSamples (256) |
| `marker` | block start | kind (`CaptureMarkerKind`), grabBars (int64), grabSeconds (double) |
| `gap` | where the drop was noticed | droppedRecords, droppedAudioSamples since the previous gap |

`TransportInfo` (plain struct filled by the plugin) carries `isPlaying`, `ppqPosition`, `bpm`,
`timeSigNumerator/Denominator`, `ppqPositionOfLastBarStart`, `isLooping`, each with a valid flag.
`quantiseTransport()` gives the value as decoded from a record; the tap compares quantised values so the
lossy bar-start field (float offset, < 1e-5 beats error) can never cause a record per block.

### 4.3 Transport records only on change

All comparisons use quantised values (`quantiseTransport`). Prediction from the last transport record:
ppq = `ppq + bpm * (sample - recordSample) / (60 sr)` while playing, unchanged while stopped; bar start =
`lastBarStart + k * barLength`, `k = floor ((predictedPpq - lastBarStart + barStartToleranceBeats) /
barLength)`, so a bar line crossed inside a block is no discontinuity. Two classes:

| Class | Trigger | When pushed |
|---|---|---|
| structural | first block after a reset record, `captureOn` or a gap; play state; valid flags; time signature; a position jump while playing (abs (reported ppq - predicted) >= `resyncLimitBeats` = 0.5 = `TakeTimeline::jumpThresholdBeats`: loop wrap, relocation) | at once |
| re-sync | bpm-only change; position deviation while playing with `ppqToleranceBeats` (1e-6) < abs (diff) < 0.5, or bar start off by more than `barStartToleranceBeats` (1e-4) | at most one per `minResyncIntervalSeconds` (10 ms); a held change goes with the first block at or after the interval, with that block's values |
| stopped position | position-only change while stopped (before and now) | at most every `stoppedPositionIntervalSeconds` (50 ms) |

Bound: <= 100 re-sync records/s even with 1-sample blocks, a tempo ramp, a host that reports positions
with jitter or no bpm (prediction at 120 BPM, re-synced to the reported ppq). The beat error of holding a
tempo ramp for 10 ms is negligible (2.5 BPM/s ramp: < 1e-5 beats); the timeline re-syncs U to the
reported ppq at every record. Only a host that changes play state or time signature every block could still
send one record per block; that is bounded by the block rate and ends in an overflow gap at worst.

Block-size invariance (binding, test): the host reports the transport once per block, so a change inside a
block is seen at the next block start. For the same input and the same per-sample transport, with every
transport change (tempo change, loop wrap, stop/start) on a block boundary of every compared block size (the
tests put them at multiples of 430080 = lcm (7, 64, 480, 512, 4096) samples), blocks {1, 7, 64, 480, 512,
4096} at 44.1 / 48 / 96 kHz give an identical record stream once audio records are removed and the audio
samples are compared as one stream. Known limitation: in a real host a tempo change or loop wrap inside a
4096-sample block is registered up to one block late (<= 85 ms at 48 kHz; see 6 for how a re-sync bounds the
effect).

### 4.4 Overflow policy and counters

- Record ring full: the record is dropped, `records` overflow counter + 1, gap pending. Audio frame does not
  fit (audio ring or its record): frame dropped, `audio` overflow counter + 256, gap pending.
- The pending gap record (dropped counts since the last gap) is pushed, together with one note-on per
  note still open (flag `EventTimingFlags::resync`, kind none, at the gap sample), before the next record
  that fits (all of them plus that record must fit, else they stay pending; the next block start then
  forces a transport and a settings record too: a dropped record may have been a change). The segmenter
  ends every sounding note at the gap (synthetic note-off) and re-opens the resync notes, so its note state
  equals the output's again (a lost note-off cannot keep a free take open forever). The worker marks the
  open / closing takes (and grabs over that span) `incomplete`; a take with a gap still exports. Missing
  audio is zero-filled by the history (sample positions come from the audio records, so later audio stays
  aligned).
- Counters: `CaptureTap::getOverflowCounts()` (cumulative since prepare, relaxed atomics), shown in the
  Capture tab status line only when non-zero. Acceptance: both stay 0 in the 10-minute test.

### 4.5 Ring sizes and memory (per instance)

| Ring | Capacity | Bytes | Lasts at the worst-case rate |
|---|---|---|---|
| records | 16384 (fixed) | 512 KiB | worst ~1.3 k records/s (True bends 200/s + test-pattern CC 200/s + 750 audio records/s at 192 kHz + 100 re-sync transport/s): >= 12 s; typical (Voice, 48 kHz): ~400/s, 40 s |
| audio | nextPow2 (2 s x sr) | 44.1/48 kHz 131072 (512 KiB, 2.7-3.0 s), 96 kHz 262144 (1 MiB), 192 kHz 524288 (2 MiB) | 2.7 s |

The worker drains every 10 ms; the margin covers a worker that is descheduled for over 2 s. Rings are
allocated in `CaptureTap::prepare()` (reallocated only when a new rate needs more, while the worker is
suspended, section 15). The block size does not change the record rate: with 1-sample blocks the
VoicePipeline still emits at most 3 events per hop and audio still goes in 256-sample frames; a 4096-sample
chunk pushes at most 16 audio frames + 1024 events at once, far below the capacity.

## 5. Worker side (core `CaptureHistory`, `TakeSegmenter`; plugin `CaptureWorker`)

### 5.1 `CaptureHistory` (core, owner A) and the memory budget

`core/include/vocalsynth/CaptureHistory.h`. Owned by the worker thread, no locking.

- Event ring: `eventCapacity = 2^18` records (events, markers, settings, gaps; not audio / transport) =
  8 MiB, allocated in `prepare()` on the worker thread. ~22 min of True-mode bends (200/s).
- Timeline: every transport record goes into a `TakeTimeline` (section 6); segments older than the oldest
  retained event or audio sample (whichever is older) are evicted (the segment containing it stays). Open
  takes keep their own timeline (5.2), so this only limits grabs.
- Audio: **RAM, float, in chunks of 65536 samples (256 KiB)**, allocated lazily by the worker as the history
  fills and reused as a ring once full. Capacity = `min (audioHistoryMinutes x 60 x sr, maxAudioBytes / 4)`
  rounded up to whole chunks, `maxAudioBytes = 256 MiB`. No disk spool: simpler (no file failures, no spool
  cleanup after a crash), and the WAV cache is written from RAM.
- Eviction: oldest first, events and audio independently. Takes own their events (5.2), so eviction never
  drops a listed take; it only reduces what a grab can reach and can truncate the audio of a very long take.
- Clear marker: drops everything before the marker but keeps the transport / settings state in force.
  Clearing reset: drops everything.

Memory and disk per instance (audio look-back 10 min, the default):

| Rate | Audio history (RAM) | + rings, event ring, takes (typical / bound) | WAV cache (disk, 24-bit) |
|---|---|---|---|
| 44.1 kHz | 106 MB (10 min) | ~10 MB / ~60 MB | 7.9 MB per minute of cached takes |
| 48 kHz | 115 MB (10 min) | ~10 MB / ~60 MB | 8.6 MB/min |
| 96 kHz | 230 MB (10 min) | ~11 MB / ~61 MB | 17.3 MB/min |
| 192 kHz | 268 MB (cap 256 MiB = 5.8 min) | ~12 MB / ~62 MB | 34.6 MB/min |

Fixed parts: record ring 512 KiB, audio ring 0.5-2 MiB, event ring 8 MiB, drain scratch 33 KiB. Takes own
their events (`CapturedEvent` = 48 bytes): a 10-minute True-bend take ~120 k events = 5.8 MB; the take
list holds at most `CaptureWorker::maxTotalTakeEvents` = 2^20 events (48 MiB) in all (oldest takes dropped
first, like the 32-take limit), each take at most `maxTakeEvents` = 2^18. Timelines: <= 100 segments/s
(4.3) x ~80 bytes, normally a handful per take. Two instances double everything (the user's FL test with
two instances: ~250 MB at 48 kHz).

The audio RAM grows over the first minutes and is reached only after `audioHistoryMinutes` of running
(also with the transport stopped: capture is always on). With `captureAudio` off it stays 0. The WAV cache
holds one file per listed take with audio (max 32 takes); its total is bounded by roughly the history length
times the overlap of grabs and takes (typically <= 1-2 x the history: <= ~170 MB at 48 kHz). `audioHistoryMinutes`
(state, 1-30, default 10) is applied on the worker; shrinking frees chunks.

### 5.2 `TakeSegmenter` (core, owner A)

`core/include/vocalsynth/TakeSegmenter.h` (rules in the class comment are binding). "Playing" = the
timeline's definition (`isPlaying && hasPpq`). Summary:

| Situation | Result |
|---|---|
| transport start (playing record after stopped / no playhead) | closes a free take at that sample; starts a **synced** take there, opening sounding notes |
| transport stop | ends the synced take at the stop sample (`endSample`); it keeps collecting until `captureEndSample` = stop + tail (below), then closes (notes still sounding: synthetic note-off at `captureEndSample`) |
| loop wrap / position jump while playing | nothing (one take; the timeline unwraps) |
| stopped, first note-on | starts a **free** take at that note-on's emit sample, unless it lies in the capture tail of the synced take that just ended (that note was sung before Stop) |
| stopped, no sounding note for `takeGapSeconds` (default 3 s, 0.5-30) after the last note-off | closes the free take; end = last note-off + `freeTailSeconds` (0.5 s) |
| marker `newTake` | ends the take at the marker; the next take starts there (synced if playing, else free), kind manual. Synced: the old take collects its tail in parallel with the new one (both hold the events of `[marker, marker + tail)`; the renderer's clamp gives each its part, so a note held across the split is closed / re-opened at the marker in compensated time). Free: synthetic note-off / note-on at the marker |
| `reset` | closes the open take and ends every tail at the newest sample (nothing more arrives on the old clock); clearing reset also forgets notes and state |
| marker `clear` | discards the open take, closing takes and pending grabs; the next take starts by the rules at the marker |
| `captureOff` / `captureOn` | closes the take at the marker (no tail: nothing is recorded after it; the tap's capture-only note-offs end the notes there: events after the marker sample are removed, note-offs at the marker sample stay, and the open-note list is rebuilt) / like a transport start or free wait |
| `gap` | open and closing takes `incomplete = true`; every sounding note ends at the gap, the tap's resync note-ons re-open the real ones |
| note-off of a note that is not sounding | ignored for the note state (kept in the take; the renderer drops unpaired note-offs) |
| take reaches `maxTakeEvents` (2^18) | closed and continued in a new take (manual-split rules) |
| take without any note-on | discarded |
| marker `grab` | pending until the records up to marker + tail are in; then `makeGrab()` from the history, listed as kind grab; the open take continues |
| host stopped processing (`closeIdle()`, worker: no clock progress for `takeGapSeconds` of wall time) | closes an open free take if no note sounds |

**Capture tail.** A take's `[startSample, endSample)` is its span in host time; synced takes and grabs
keep collecting events until `captureEndSample = endSample + compensationTailSamples (sr, maxBlock)` =
`maxRoundTripSamples` (largest round trip any parameter value allows: max (2 x maxBlock + 100 ms, 300 ms))
+ `detectionTailSeconds` (0.25 s). ASIO 2048 at 48 kHz: 14400 + 12000 samples = 0.55 s. The round trip is
a render-time setting (8), so the tail covers every value; the renderer then clamps compensated times into
`[startSample, endSample]` and drops what falls outside (notes sung before the transport start, after
Stop). Free takes: `captureEndSample == endSample` (no round trip in free mode). The tail delays the take's
appearance in the list by ~0.5 s after Stop.

Free-mode gap timing: the gap is measured on the emit samples (the compensated times are not known to the
segmenter, and the gap is a user-facing pause). `advanceTo (tap clock)` closes a free take, ends tails and
builds pending grabs without waiting for another record; results never depend on how often it is called
(block-size invariance). Just below vs above the gap is tested.

**Grab** ("Grab last ..."): marker time = the audio clock. Synced (timeline playing at the marker): span from
the start of the bar `grabBars - 1` bars before the bar containing the marker up to the marker (the current
bar as far as it was played); bars 1, 2, 4, 8, 16, 32, default 8. Free: the last `grabSeconds` (10, 20, 30,
60, 120; default 30). Events of `[start, marker + tail)` (free grabs: tail = `detectionTailSeconds`), built
once the history holds them; the grab's timeline covers only the samples before its capture end (a transport
record exactly at the capture end is not included, so the result does not depend on how often the worker drains); the start is clamped to the oldest stored record; sounding notes opened at the
start and closed at the capture end (synthetic); no note-on -> nothing listed. The export origin (9) is then
computed as for any other take.

A `Take` (`core/include/vocalsynth/Take.h`) owns: number (1, 2, ... per instance, never reused, also not after
Clear), mode, kind, start / end / capture-end sample, sample rate and max block (from the reset record), the
settings at the start (bend mode / range, channel, minNoteLength, test pattern), the events (`CapturedEvent`:
emit sample, estimated sample, refined sample (`noSample` = none), VsEvent, timing, synthetic flag), the
timeline (synced: from the playing segment at `startSample` to `captureEndSample`, appended by the segmenter
as transport records arrive while the take is open, because the history may evict the start of a long take;
free: may be empty), `freeBpm` (host bpm at the start of a free take, else 120), note count, `incomplete`,
audio availability, overview, and from the worker: wall-clock start (file names) and `observedBlockSize`.

### 5.3 `CaptureWorker::step()` (plugin, owner C)

```
options = pendingOptions (setOptions, any thread; small mutex)      // take gap, look-back, pre-roll, WAV format
offset  = sources.readOffset()                                      // raw atomics of the 3 offset parameters
drainer.drain (tap, history, segmenter, closed)                     // core CaptureDrain, see below
if tap clock unchanged for takeGapSeconds of wall time: segmenter.closeIdle (history, closed)
for each closed take: wallClockStartMs = now - (clock - startSample) / sr; observedBlockSize;
   audio availability (TakeRenderer::computeAudioSpan with roundTripFor (take, offset) vs
   history.getAudioRange()); refineNoteOnsets (audio of the take); overview; publish (lock: list front;
   then drop the oldest takes while > maxTakes or > maxTotalTakeEvents events, deleting their cache WAV
   unless persisted); queue a WAV cache job; version++
clear marker seen (segmenter): empty the take list the same way (persisted WAVs stay); version++
open take snapshot: at most every 500 ms and only if it changed (copy under the lock; version++)
WAV job: WavFileWriter, <= wavFramesPerStep (65536) frames per step read from the history (file kept
   open between steps), so draining stays every 10 ms and stop() returns within one step; state
   preparing -> ready (or unavailable); re-render when the round trip of the take (offset parameters),
   the pre-roll (free) or the format changed and the audio is still in the history; a job whose audio got
   evicted meanwhile marks the take's audio truncated
heartbeat: every 60 s touch <instance>/heartbeat if the folder exists (created with the first cached WAV)
```

`CaptureDrain::drain()` (core, owner A; the same loop the core end-to-end tests use): clock = `tap.getClock()`
(acquire, before popping); pop records (at most about one ring capacity per call); audio record -> its frame
from the audio ring -> `history.addAudio`; other records -> `history.addRecord` then `segmenter.process`;
finally, only if the record ring was emptied, `segmenter.advanceTo (clock)` and `getLastClock()` = clock (a
call that stopped at its size limit with records still queued leaves both, so a free take never closes before
a queued note-on is read). The audio frame of an audio record is always in the audio ring when
the record is read (pushed before the record; the consumer's acquire of the audio head makes it visible). A
forward jump of audio record samples (dropped frames) is zero-filled by the history.

## 6. Time base: `TakeTimeline` (core, owner A)

`core/include/vocalsynth/TakeTimeline.h`. Piecewise-constant tempo map built from transport records.

- Playing = `isPlaying && hasPpq` (a host that says "playing" without a position has no grid: free mode).
- Segment = {startSample, beats U at start, bpm, playing, hasHostPosition, hostPpq at start, time signature,
  bar anchor ppq, bar anchor number}. Within a segment: `U (s) = U0 + (playing ? (s - s0) bpm / (60 sr) : 0)`
  and `hostPpq (s) = ppq0 + (U (s) - U0)`.
- New record while playing, previous segment playing: predicted host ppq vs reported. `abs (diff) <
  jumpThresholdBeats` (0.5): **re-sync** `U += diff`, but never below the U of the previous segment start
  (monotonic; the host changed tempo inside a block, or the tap's rate-limited re-sync records). Otherwise
  (loop wrap, jump) or a transport start: U continues from the prediction (unwrapped), only the bar grid
  re-anchors to the new host position. While stopped U does not advance.
- Without bpm: 120 (`fallbackBpm`). Without ppq: not playing (see above).
- Bar grid: bar length = numerator x 4 / denominator quarter notes. Anchor = host bar start
  (`ppqPositionOfLastBarStart`) if valid, else `floor (ppq / barLength) x barLength`; anchor bar number =
  `llround (anchor / barLength) + 1` (FL convention, bar 1 at ppq 0; with time-signature changes earlier in
  the song this is an approximation, stated in M3-TESTING). `barAt (s)`: `k = floor ((hostPpq (s) - anchor +
  barSnapToleranceBeats) / barLength)`, number = anchorNumber + k, start U = `U0 + (anchor + k barLength -
  ppq0)`. `barSnapToleranceBeats = 1/32`: a note sung a hair before beat 1 belongs to the new bar (its tick
  clamps to 0, so it moves later by at most 1/32 beat = 15.6 ms at 120 BPM; quantize removes the rest).
- Before the first segment, `beatsAt` / `sampleAtBeats` extrapolate with its bpm if it is playing. A synced
  take's timeline starts with the playing segment at `startSample` (`slice()` cuts the containing segment
  to start there), so a bar origin a little before the transport start still maps to a sample (WAV start).
- Tempo changes are honoured from the sample where they were reported (FL tempo automation -> piecewise
  constant map). `tempoMap()` / `timeSignatureMap()` list them in U for the renderer.
- Precision: all in double; ticks = `llround (beats x PPQ)`; deterministic.

Free takes do not use the timeline for ticks: seconds at `freeBpm`, 4/4 (spec 5).

## 7. Detection-delay compensation (core, owner B)

Goal (acceptance, spec 6): in the export, MIDI vs audio onset offset <= 1 hop median, p95 <= 2 hops, for
synthetic voiced onsets, all ranges, YIN and MPM, 44.1 / 48 / 96 kHz. The live MIDI and `VsEvent` stay
unchanged.

### 7.1 Exact API change (B implements these; existing signatures and their events stay identical)

The `VoicePipeline` overloads below already exist as **scaffold stubs** (they forward to the existing
overloads and push no timings; the tap then records kind none), so C can wire the processor now. B
replaces the stubs and adds the `NoteSegmenter` part.

`core/include/vocalsynth/EventTiming.h` (done): `VsEventTiming {int32 delaySamples; EventTimingKind kind;
uint8 flags}` with `delaySamples = emit sample - estimated true sample`, kinds `none / model / measured`,
flags `correction`, `noteChange`; `VsEventTimingBuffer<N>`, `VsEventTimingSink`.

`VoicePipeline.h` additions:

```cpp
/** M3 capture side output: the same events as process (mono, n, settings, out); for every event that out
    accepted, one VsEventTiming is pushed to timing, in the same order. */
template <std::size_t N>
void process (const float* mono, int numSamples, const VoiceSettings& settings,
              VsEventBuffer<N>& out, VsEventTimingBuffer<N>& timing) noexcept
{ processToSink (mono, numSamples, settings, VsEventSink { out }, VsEventTimingSink { timing }); }

template <std::size_t N>
void flush (int sampleOffset, VsEventBuffer<N>& out, VsEventTimingBuffer<N>& timing) noexcept
{ flushToSink (sampleOffset, VsEventSink { out }, VsEventTimingSink { timing }); }

void processToSink (const float* mono, int numSamples, const VoiceSettings& settings,
                    VsEventSink out, VsEventTimingSink timing) noexcept;
void flushToSink (int sampleOffset, VsEventSink out, VsEventTimingSink timing) noexcept;
// The existing overloads without a timing argument forward with a default (disconnected) VsEventTimingSink.

static constexpr double onsetEnvelopeTimeConstantMs = 1.0;   // level envelope for onset / release estimates
```

`NoteSegmenter.h` additions:

```cpp
/** M3: newest upward / downward crossing of the pipeline's level envelope through
    gateThresholdDb - gateHysteresisDb (absolute host samples). Never emits. */
void setLevelOnsetSample (int64_t absoluteSample) noexcept;
void setLevelReleaseSample (int64_t absoluteSample) noexcept;

/** M3: timing of the events emitted by the most recent processFrame / processOnsetFrame / flush call
    (valid until the next call; delays relative to that call's absoluteSample). */
struct FrameEventTiming
{
    VsEventTiming noteOn;   ///< a note-on of this call, and a bend sent right before it (note start bend)
    VsEventTiming noteOff;  ///< a note-off of this call, and a bend centre sent right after it
    VsEventTiming update;   ///< any other bend of this call (held-note update)
};
const FrameEventTiming& getLastEventTiming() const noexcept;
```

Pipeline mapping (internal, suggested): when the timing sink is connected, each segmenter call emits into a
member scratch `VsEventBuffer<NoteSegmenter::maxEventsPerFrame>`; the pipeline forwards each event to `out`
and, if accepted, pushes the timing chosen by type: noteOn -> `noteOn`; noteOff -> `noteOff`; pitchBend
directly before a noteOn of the same call -> `noteOn`; directly after a noteOff -> `noteOff`; other bends ->
`update`. Settings flushes (`requiresFlush`) and `flushToSink` -> kind `none`, delay 0. The events must be
identical with and without the timing sink (test), and nothing allocates.

### 7.2 Estimates (initial method; B tunes against the acceptance tests)

- **Level envelope** (pipeline, host rate, on the conditioned signal, before decimation): one-pole power
  smoother, tau = 1 ms; crossings through `gate - gateHysteresisDb` are passed to the segmenter. Per sample,
  block-size invariant, ~3 flops per sample.
- **Note start from silence** (confirmed or provisional): estimate = newest upward level crossing minus the
  envelope's rise bias (a function of tau and the level above the threshold; B calibrates), if it lies within
  `[emit - (window + provisionalTimeoutHops hops) x D, emit]`; kind measured. Else the model: `emit -
  computeFastOnsetLatencyMs (sr, range, f)` for a provisional start, `computeConfirmedLatencyMs (sr, range)`
  for a full-window start; kind model.
- **Note change** (legato or retrigger without silence): estimate = start of the change timer (first frame
  whose candidate won) minus `getFrameCentreDelayInputSamples()`, but after the old note's start; kind
  measured, flag `noteChange`. The old note's note-off (legato order: after the note-on) gets the same
  estimate, so the old note ends where the new one starts.
- **Provisional correction** (the full window changes a provisional note within the settle time / at
  confirmation): the note-off of the provisional note and the corrected note-on get the provisional note's
  delay (so Raw shows the correction at its true time) and flag `correction` (Clean merges, section 9).
- **Release**: newest downward level crossing within `[note start, emit]`, else the first non-voiced frame of
  the release run minus the frame-centre delay; kind measured.
- **Bend updates**: emit - frame-centre delay; kind measured.
- **Test pattern, CC, flushes**: none (the event happened when it was sent).
- **Audio refinement** (Clean only, `TakeRenderer::refineNoteOnsets`, worker, with captured audio): each
  measured / model note-on is moved to the energy onset of the dry audio inside `[estimate - 15 ms, emit]` if
  a clear onset is found (`refinedSample`).

## 8. Latency (round-trip) compensation

`computeRoundTripSamples()` (`core/src/CaptureOptions.cpp`, done):

| `captureOffsetMode` | rt (samples) |
|---|---|
| Auto (default) | `2 x maxBlockSize` (from `prepareToPlay`, i.e. the take's reset record) + `llround (captureFineOffsetMs x sr / 1000)` |
| Manual | `llround (captureOffsetMs x sr / 1000)` (0-300 ms) |
| Off | 0 |

Clamped to >= 0. Example: ASIO 2048 at 48 kHz -> 4096 samples = 85.3 ms. rt is applied **at render time**
(the parameters are read when the user drags, and by the worker for the WAV cache), so changing the offset
after singing re-times the export. It is **per take**: `TakeRenderer::roundTripFor (take,
CaptureOffsetSettings)` uses the take's own max block and rate (from its reset record); synced takes only
(free takes have no grid; spec 5). Musical time of a captured sample s is taken at `s - rt` for MIDI and
audio: the WAV covers capture-clock samples `[sampleAtBeats (origin) + rt, endSample + rt)`. Compensated
synced event times are clamped into the take's host-time span `[startSample, endSample]`; the capture tail
(5.2) guarantees that every event up to `endSample + rt` is in the take for any rt the parameters allow.

The Capture tab shows the Auto value in ms and the largest block actually seen (`RecentBlockSize`; also
stored per take as `Take::observedBlockSize`); if the observed block is much smaller than the prepared
maximum, it shows a hint that Manual may fit better (FL may prepare with a larger maximum than its ASIO
buffer; to be checked in the FL test, M3-TESTING step 2). Should FL show that mismatch, Auto can switch to
the observed block without a format change (the value is already in every take).

Testability: core tests construct takes with a known `maxBlockSize` / rate and check that rt moves notes by
`llround (rt / sr x bpm / 60 x PPQ)` ticks at constant tempo (2048 @ 48 kHz -> 85.3 ms); the end-to-end
compensation test runs synthetic onsets through VoicePipeline -> CaptureTap -> CaptureDrain -> renderer and
compares the MIDI tick with the onset's position in the WAV span (both shifted by rt, so the offset is the
detection-delay error alone, for rt = 0 and rt = 4096). The plugin test checks that `prepareToPlay (48000,
2048)` gives takes with `maxBlockSize` 2048.

## 9. `TakeRenderer` (core, owner B) and export options

`core/include/vocalsynth/TakeRenderer.h`, options in `CaptureOptions.h` (`RenderOptions`). The class comment
is the binding rule set; summary of the pipeline (order binding: compensation -> version -> quantize ->
pairing / clamping):

1. **Compensation and origin.** Event time t = `estimatedSample` (Clean note-ons: `refinedSample` if set),
   minus rt for synced takes; synced times are then clamped into `[startSample, endSample]` (notes entirely
   outside are dropped: sung before the transport start or after Stop / the split). Origin: the take's
   first note-on (lowest emit sample, synthetic ones included) at `o = refinedSample` if set, else
   `estimatedSample` - for every version (the worker refines before publishing), so the Clean first note
   never lies before tick 0. Synced = start of the bar containing `clamp (o - rt, start, end)` (decision 1),
   free = `o - freePreRollMs` (default 0). The origin is independent of version and quantize, so Raw, Clean,
   every quantize setting and the WAV share tick 0. Synced ticks `llround ((U (t) - U (origin)) x PPQ)`;
   free ticks from seconds at `freeBpm`. Events before the origin clamp to tick 0.
2. **Version.** Raw: the events as sent (compensated). Clean, first stage (spec 7, thresholds as constants
   in the header): provisional-correction merge (flag `correction`: the corrected note takes the provisional
   start, the provisional note is dropped), ghost notes shorter than `max (cleanMinNoteMs = 80, take's
   minNoteLength)` removed, transition notes (< 120 ms, between two notes with gaps <= 15 ms, pitch between the
   neighbours) merged into the following note, octave strays (< 200 ms, >= 11 semitones from both neighbours)
   removed. Clean never moves a note by more than the merged span.
3. **Quantize** (synced only; free takes are never quantized): grids 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32
   (`quantizeGridTicks`: PPQ / 1, 2, 3, 4, 6, 8), start moves `strength x (nearest grid - start)` (llround),
   ends to the grid when enabled (length >= half a grid step). Bends / CCs of a note move with its start
   (clamped into the note).
4. **Pairing / clamping.** Unpaired note-offs are dropped (capture switched on mid-note, gaps); every
   note-on has a note-off inside the take (endTick at the latest); same pitch +
   channel never overlaps (the earlier note ends at the later start); length >= 1 tick; ticks >= 0; bends only
   if `bendsEnabled (options.bends, take.settings.bendMode)` (then RPN 0 range = the take's bend range) and a
   centre bend after the last note if the last bend was not 0; CCs only if `controlChanges`; sorted by tick,
   then note-off < CC < bend < note-on, then emission order.
5. **endTick**: synced = the first bar line at or after the last event tick (a note-off exactly on a bar line
   adds no empty bar; bars counted from tick 0 with the rendered time-signature map), >= 1 bar; free = last
   event tick (>= 1). Time-signature changes sit at the host bar start where they took effect (in ticks).
6. **Audio span** (`computeAudioSpan`, also used by the worker): synced `[sampleAtBeats (origin) + rt,
   take.endSample + rt)`, free `[origin, take.endSample)`. WAV sample 0 = tick 0, length = the span. The
   synced span may start before the transport start (bar origin) and ends inside the capture tail; both are
   in the history.

| Option (`RenderOptions` / `CaptureState`) | Values | Default |
|---|---|---|
| version | Raw, Clean | Clean |
| quantize | Off, 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 (synced only) | Off |
| quantizeStrengthPercent | 0-100 | 100 |
| quantizeEnds | off / on | off |
| bends | As plugin, Off, On | As plugin |
| controlChanges | off / on | on |
| cleanMinNoteMs | 0-500 (UI: not exposed in 0.3.0) | 80 |
| freePreRollMs | 0-1000 (UI: not exposed in 0.3.0) | 0 |
| SMF format | type 1, type 0 | type 1 |
| WAV format | 24-bit, 32-bit float | 24-bit |

Performance: `render()` of a 10-minute take < 50 ms on the message thread (test with a synthetic 10-minute
True-bend take); allocations are fine there.

## 10. `SmfWriter` (core, owner B)

`core/include/vocalsynth/SmfWriter.h`, layout binding (header comment, exact bytes there): MThd (format 0 or
1, 1 track, division PPQ 960), one MTrk; at tick 0 in the M1 order that FL accepted: track name (`FF 03`,
"VocalSynth Take 3 Clean", ASCII), tempo (`FF 51 03`, `llround (60e6 / bpm)` us), time signature
(`FF 58 04 nn dd 18 08`, dd = log2 of the denominator, non-powers of two rounded to the nearest), RPN 0 bend
range per used channel when bends are exported; then tempo / time-signature changes and channel events by
tick (meta first on equal ticks), note-off as `8n kk 00`, no running status; end-of-track at endTick.
Type 1 with a single track is the M1 format (`juce::MidiFile::writeTo (out, 1)` with one track) that FL
imported in the M1 test.

FL import behaviour (spec facts; verified in M3-TESTING): dropped into an existing project, FL keeps the
project tempo and places notes by ticks / PPQ. Synced takes therefore land on the grid at any project tempo
(ticks come from beats). Free takes are ticks from seconds at the host bpm: they keep their real-time
feel only if the project plays at that bpm (stated in the checklist). CCs are not imported as notes
(event editor), bends maybe with a fixed +-2 range: RPN 0 is written anyway. The tempo meta is
informational. No I/O in core: the plugin writes the bytes (`ExportFiles::writeBytesAtomically`).
Byte-identical for identical input.

## 11. `WavWriter` (core, owner B)

`core/include/vocalsynth/WavWriter.h`: `makeHeader (format, sr, numFrames)` (44-byte PCM 24-bit header,
58-byte IEEE-float header with `fact`), `encode (samples, format, out)` (24-bit: `clamp (llround (x x 2^23),
-2^23, 2^23 - 1)`, no dither; float as is; NaN -> 0), pad byte for odd data sizes, `maxFrames` (RIFF 32-bit
limit: ~8 h of 48 kHz mono 24-bit, never reached). The plugin streams with `CaptureExporter::WavFileWriter`
(open: header into a `juce::TemporaryFile` next to the target; write: blocks of <= 65536 samples read from
the history, encoded into a reusable byte buffer; finish: pad byte, close, move into place with the
numbered-sibling fallback; abort / destructor: temp file deleted), one slice per worker step.

## 12. Files (plugin `CaptureExporter`, `ExportFiles`, owner C)

- **Names** (ASCII, no spaces; timestamp = local time of the take start):
  `VocalSynth_<yyyy-MM-dd_HHmmss>_Take<NN>_Bar<BBB>_<Raw|Clean>[_Q16[-50]].mid` (free takes without `_Bar`;
  quantize tag from `quantizeGridTag`, `-NN` when the strength is below 100 %),
  `VocalSynth_<...>_Take<NN>_Bar<BBB>_Voice.wav`. NN >= 2 digits, BBB >= 3 digits.
- **Locations**: `<temp>/VocalSynth/Capture/<instanceId>/` holds the MIDI files and the WAV cache (instance id:
  16 hex chars per processor). Dragged / saved WAVs: `<Documents>/VocalSynth/Takes/` (or the folder in
  `CaptureState::takesFolder`), never deleted by VocalSynth. The M1 test clips in `<temp>/VocalSynth/` stay.
- **MIDI on drag start** (message thread): `TakeRenderer::render` -> `SmfWriter::write` ->
  `ExportFiles::writeBytesAtomically` (temp + move, numbered sibling if FL still holds the file; M1 logic,
  factored out of `TestClipExporter.cpp` into `plugin/ExportFiles.*`; C makes TestClipExporter use it).
- **WAV**: rendered by the worker into the cache when a take closes (state preparing -> ready; the WAV handle
  shows "preparing..." until then). On drag or Save as the cached file is **moved** into the takes folder
  (`persistWav`: rename, copy + delete as fallback, numbered sibling if the name exists), because FL
  references dropped WAVs; the take remembers the persisted path, so a second drag reuses it.
- **Save as...**: async `juce::FileChooser` for the selected take: MIDI with the current options, WAV copied
  from the cache / persisted file.
- **Cleanup**: the instance folder is created lazily with the first file (an instance that never wrote
  anything, e.g. in pluginval, leaves nothing behind); `deleteInstanceFolder` in the processor destructor
  (after the worker stopped); at worker start `deleteStaleInstanceFolders` removes other instance folders
  whose heartbeat (touched every 60 s while the folder exists) is older than 24 h. A take dropped from the
  list (32-take / event limit, Clear) deletes its cached WAV unless persisted.

## 13. Parameters and state

New parameters in `plugin/Parameters.h` (done; appended after the M2 parameters; IDs permanent; **version
hint 2** = `params::captureParameterVersionHint`: JUCE sorts AUv2 parameters by version hint, then ID hash,
and Logic / GarageBand address AU parameters by index, so parameters of a later release need a higher hint
than the released ones (M1/M2: 1); VST3 and CLAP use the ID hash only; the next release's parameters get 3):

| ID | Name | Type | Range / values | Default |
|---|---|---|---|---|
| `captureEnabled` | Capture | bool | off / on | on |
| `captureAudio` | Capture Audio | bool | off / on | on (decision 2) |
| `captureOffsetMode` | Capture Offset Mode | choice | Auto, Manual, Off (index == `CaptureOffsetMode`) | Auto (decision 3) |
| `captureOffsetMs` | Capture Offset | float, ms | 0-300 (Manual) | 0 |
| `captureFineOffsetMs` | Capture Fine Offset | float, ms | -100..+100 (added in Auto) | 0 |

Value texts without unit (label "ms"), one decimal, continuous (like M2). Reported host latency stays 0.

**Non-parameter state** (`plugin/CaptureSettings.h`, `CaptureState`): export options (9), SMF / WAV format,
grab bars / seconds, `takeGapSeconds` (0.5-30, default 3), `audioHistoryMinutes` (1-30, default 10), takes
folder. Stored as child `CAPTURE` (property `version` = 1) of the APVTS state in `getStateInformation`;
`setStateInformation` reads it with `CaptureState::fromValueTree` (missing child / properties -> defaults,
values clamped, grab lengths snapped to the choices) and **removes the child** from the tree before
`apvts.replaceState` (else the APVTS state keeps it and every save adds another copy);
`getStateInformation` appends a fresh child to `apvts.copyState()`. Both may run on a background thread
(pluginval's state tests): `CaptureState` sits under its own mutex, `CaptureWorker::setOptions` may be
called from any thread. `migrateState()` already
gives the five new parameters their defaults in old projects. **Takes are never saved** (decision 4); the UI
says so. Thread-safety: the processor keeps `CaptureState` under a mutex that the audio thread never touches;
the worker gets `setOptions (CaptureWorkerOptions)` copies and reads the offset parameters itself (5.3).

## 14. UI: "Capture" tab (plugin `CapturePanel`, owner C)

Third page button "Capture" next to "Voice" and "Test & Routing" (`VocalSynthEditor::Page::capture`); version
text "0.3.0 – M3 capture". Contents (spec 10):

- Status line: "Synced 128.0 BPM 4/4 – recording take 3 (bar 17)" / "Free – waiting for a note" /
  "Free – recording take 4" / "Capture off"; overflow counters only when non-zero.
- Take list (newest on top; a `juce::ListBox` or own rows): number, start (bar or time), length (bars or
  seconds), note count, badges Grab / incomplete / audio truncated, mini piano roll of the selected version
  (rendered with `TakeRenderer` on selection or option change, cached per take; auto pitch range) and a
  waveform strip (`Take::overview`: min / max pairs per 512 samples, computed by the worker when the take
  closes).
- Drag handles "MIDI" and "WAV" (`FileDragSource`, moved out of the editor; tooltip "Drag into FL Studio –
  hold Shift while dropping to skip FL's import dialog"), "Save as...".
- Buttons: "New take", "Grab last [N bars / N s]" (combo with the choices, label follows the mode), "Clear"
  (with confirmation), export options (version, quantize + strength + ends, bends, CC), capture timing
  (mode, ms, fine ms, Auto value in ms), audio on/off, free-mode gap.
- Hint: "Takes are not saved with the project – drag or save what you want to keep."
- Timer 10 Hz; repaint only when `getTakeListVersion()` or the status text changed (the user found 30 Hz
  updates hectic). The panel never reads audio-thread data; take data comes from the worker's lock.

## 15. Threading and lifetime (binding)

| Moment | What happens |
|---|---|
| constructor | `CaptureTap`, then `CaptureWorker` (member order: the worker is destroyed first; no thread), instance id; no files |
| `prepareToPlay` | `worker.suspend()` (drains the rings, parks; no-op before the first start) -> `captureTap.prepare (sr, maxBlock)` (rings, reset record armed) -> `worker.start()` (first time: creates the thread; the thread allocates the history and deletes stale folders) or `worker.resume()` |
| `releaseResources` | nothing for the capture (the history survives FL's engine stop/start) |
| `reset()` (host) | `flushPending` as today; the next block's flush also calls `captureTap.flushNotes()` |
| destructor | `worker.stop()` (signal, notify, `waitForThreadToExit (-1)`: no `stopThread (timeout)`, which kills the thread on timeout; returns within one bounded step, an unfinished WAV job is aborted) -> `deleteInstanceFolder` |
| tests | `getCaptureWorker().setManualSteppingForTests (true)` before the first `prepareToPlay`: `start()` prepares the history on the calling thread and creates no thread; the test drives `runStepForTests()` |

pluginval / Steinberg validator / clap-validator open and close many instances quickly: no thread before
the first `prepareToPlay`, a prompt join, nothing on the message thread that waits for the audio thread.
The audio thread only touches the tap; the tap's rings are reallocated only inside `prepareToPlay` while
the worker is parked.

## 16. Test plan (Catch2, all in CI)

Core (`VocalSynthCoreTests`, one file per module, tags as given):

| File | Tag | Owner | Cases |
|---|---|---|---|
| `CaptureRecordTests.cpp` | `[capturerecord]` | A | round trip of every record type (done) |
| `CaptureDrainTests.cpp` | `[capturedrain]` | A | records and frames reach history + segmenter in stream order, frame matched to its audio record, records after the clock do not move `advanceTo` back, bounded per call |
| `SpscRingTests.cpp` | `[spscring]` | A | wrap-around, full / empty, bulk, all-or-nothing, overflow count, two-thread stress >= 10^7 items with checksum |
| `CaptureTapTests.cpp` | `[capturetap]` | A | no allocation (counting operator new around the calls), event normalisation = `MidiOutputStage` (vel-0 note-on, note-off of a closed note, duplicate note-on, clamping; the plugin test compares with the real stage), same-rate prepare keeps clock + open notes, transport records only on change (structural at once; re-sync <= 1 per 10 ms with 1-sample blocks, a tempo ramp and a jittery ppq; bar line inside a block is no record; stopped position limit), overflow -> gap + resync note-ons + forced transport/settings, markers coalesce, block-size invariance {1, 7, 64, 480, 512, 4096} x 44.1/48/96 kHz with a synthetic playhead (constant tempo, tempo change, loop wrap, stop/start), markers, flushNotes, bypass, captureOff / audioOff, overflow -> gap, `[.benchmark]` < 1 % of a 128-sample block |
| `CaptureHistoryTests.cpp` | `[capturehistory]` | A | eviction (events, audio), lazy chunks + cap, holes, audio runs, clear keeps transport state, settingsAt |
| `TakeTimelineTests.cpp` | `[taketimeline]` | A | exact beats at 60 / 97.5 / 120 / 174 BPM, tempo change, stop/start, loop wrap unwrap, re-sync (monotonic), playing without ppq = stopped, backward extrapolation of a playing first segment, bar grid (host bar start, from ppq 0, no ppq), 3/4 and 7/8, snap tolerance, maps, slice |
| `TakeSegmenterTests.cpp` | `[takesegmenter]` | A | transport start/stop, capture tail (a note sung just before Stop stays in the synced take; no free take starts in the tail; a split's tail overlaps the next take; reset ends the tail), gap ends sounding notes + resync, `closeIdle`, results independent of the `advanceTo` call pattern, loop wrap one take, free gap just below / above, manual new take inside a held note, empty takes, reset, clear, eviction, event limit, grab N bars / N seconds |
| `CaptureOptionsTests.cpp` | `[captureoptions]` | B | round trip (Auto 2048 @ 48 kHz = 4096), grids, tags, sanitise (done) |
| `TakeRendererTests.cpp` | `[takerenderer]` | B | clamp into the host-time span (pre-start and post-stop notes dropped / cut), origin from the refined first note-on shared by Raw / Clean, unpaired note-offs dropped, endTick with a note-off on a bar line, `roundTripFor`, exact ticks at 60 / 97.5 / 120 / 174 BPM x PPQ 96 / 480 / 960, tempo change inside a take, 3/4 and 7/8 origins, origin after a loop wrap, free at host bpm / 120 fallback, rt moves notes by the expected ticks, quantize (each grid incl. triplets x 0 / 50 / 100 % x ends), no zero-length / reordered notes, same-pitch overlap, free never quantized, Clean cases (ghost, transition, octave stray, correction) with neighbours untouched, Raw unchanged, bends As plugin / Off / On, CC, endTick, audio span, 10-minute render < 50 ms |
| `SmfWriterTests.cpp` | `[smfwriter]` | B | header, meta order (name, tempo, time signature) and exact meta bytes (`FF 58 04 nn dd 18 08` incl. 3/4, 7/8, 6/8 and a non-power-of-two denominator), VLQ edges, RPN, running-status-free bytes, end-of-track, determinism |
| `WavWriterTests.cpp` | `[wavwriter]` | B | headers, pad byte, clamping / rounding / NaN, streaming = one block |
| `CaptureCompensationTests.cpp` | `[capturecompensation]` | B | pipeline timing side output (same events with / without), synthetic tones with known onsets through VoicePipeline -> CaptureTap -> CaptureHistory -> TakeSegmenter -> TakeRenderer: onset offset median <= 1 hop, p95 <= 2 hops per range x algorithm x rate; corrections; legato; byte-identical `.mid` for all block sizes (end-to-end invariance gate) |
| existing `VoicePipelineTests.cpp`, `NoteSegmenterTests.cpp` | | B | unchanged results with the new overloads, timing per event |

Plugin (`VocalSynthPluginTests`):

| File | Owner | Cases |
|---|---|---|
| `CaptureParameterTests.cpp` | C | IDs / ranges / defaults / version hint 2 (done), CaptureState defaults (done), state round trip of parameters + CaptureState (one `CAPTURE` child after repeated save/load), old states load with defaults, state calls from a background thread |
| `CaptureWorkerTests.cpp` | C | lifecycle (start / suspend / resume / stop, join within ~100 ms also during a WAV job, no thread with manual stepping), drain step, take list (32 takes, 2^20 events), Clear empties the list, open-take snapshot, WAV cache states (sliced, abort deletes the temp file), re-render on a new offset parameter value, `closeIdle` after wall-clock idle, lazy folder + heartbeat, `makeTransportInfo` |
| `CaptureExporterTests.cpp` | C | `.mid` readable by `juce::MidiFile` (PPQ, meta at tick 0, RPN when bends on, end-of-track, pairing), WAV readable by `juce::WavAudioFormat` (length = span, sample 0 = tick 0), names, temp + move with a locked target, persistWav, stale cleanup, folders (partly done) |
| `CapturePanelTests.cpp` | C | status texts, rows, repaint on change only, handles available / preparing, buttons -> tap requests, Clear confirmation, options persist |
| `ProcessorTests.cpp` (existing) | C | 10-minute simulated take (Voice and Test Pattern; the test drives `runStepForTests`) with overflow 0; capture on/off; bypass; flush note-offs captured; Instrument sidechain; CPU worst block still < 20 % |

Gates as before: pluginval 10, Steinberg validator, clap-validator, auval (macOS manual), vs-eval self-test.

## 17. File ownership (next phase)

Every agent builds in its own build directory, never touches CMake (everything is registered), and only
edits its files. Interfaces between the groups are the core headers listed below; a needed change to
another group's header goes through the integrator (report it).

**A – core capture**
- `core/include/vocalsynth/SpscRing.h`
- `core/include/vocalsynth/CaptureRecord.h`, `core/src/CaptureRecord.cpp` (wire format: change only with a
  design update)
- `core/include/vocalsynth/CaptureTap.h`, `core/src/CaptureTap.cpp`
- `core/include/vocalsynth/CaptureHistory.h`, `core/src/CaptureHistory.cpp`
- `core/include/vocalsynth/TakeTimeline.h`, `core/src/TakeTimeline.cpp`
- `core/include/vocalsynth/Take.h`, `core/include/vocalsynth/TakeSegmenter.h`, `core/src/TakeSegmenter.cpp`
- `core/include/vocalsynth/CaptureDrain.h`, `core/src/CaptureDrain.cpp` (implemented in the scaffold)
- `tests/core/SpscRingTests.cpp`, `CaptureRecordTests.cpp`, `CaptureTapTests.cpp`, `CaptureHistoryTests.cpp`,
  `TakeTimelineTests.cpp`, `TakeSegmenterTests.cpp`, `CaptureDrainTests.cpp`

**B – core render + detection-delay compensation**
- `core/include/vocalsynth/EventTiming.h`, `core/include/vocalsynth/CaptureOptions.h`, `core/src/CaptureOptions.cpp`
- `core/include/vocalsynth/TakeRenderer.h`, `core/src/TakeRenderer.cpp`
- `core/include/vocalsynth/SmfWriter.h`, `core/src/SmfWriter.cpp`
- `core/include/vocalsynth/WavWriter.h`, `core/src/WavWriter.cpp`
- `core/include/vocalsynth/VoicePipeline.h`, `core/src/VoicePipeline.cpp` (the 7.1 overloads are declared,
  with forwarding stubs at the end of the .cpp: replace them),
  `core/include/vocalsynth/NoteSegmenter.h`, `core/src/NoteSegmenter.cpp` (section 7.1 API; behaviour of the
  existing overloads unchanged)
- `tests/core/CaptureOptionsTests.cpp`, `TakeRendererTests.cpp`, `SmfWriterTests.cpp`, `WavWriterTests.cpp`,
  `CaptureCompensationTests.cpp`, and the existing `VoicePipelineTests.cpp`, `NoteSegmenterTests.cpp`
- B's end-to-end tests use A's classes; until A lands, write them against the headers and keep the cases
  that need A's implementation hidden (tag `[.]` added to their tags); the integrator un-hides them. ctest
  must be green at every hand-over.

**C – plugin, UI, docs**
- `plugin/PluginProcessor.*`, `plugin/PluginEditor.*`, `plugin/Parameters.h` (from now on),
  `plugin/TestClipExporter.*` (switch to `ExportFiles`)
- new: `plugin/CaptureWorker.*`, `plugin/CaptureExporter.*`, `plugin/CapturePanel.*`, `plugin/CaptureSettings.*`,
  `plugin/ExportFiles.*`, `plugin/FileDragSource.*`
- `tests/plugin/*` (incl. the new `CaptureParameterTests.cpp`, `CaptureWorkerTests.cpp`,
  `CaptureExporterTests.cpp`, `CapturePanelTests.cpp`)
- `VERSION` -> 0.3.0, `docs/M3-TESTING.md` (German, from the spec's FL checklist, with expected results and
  what to report), `packaging/windows/LIESMICH.txt` (CRLF), `README.md`, `docs/PLAN.md` (M3 checkboxes),
  `.github/workflows/build_and_test.yml` (add `docs/M3-TESTING.md` to the Windows package)

**Integrator (after A, B, C)**: `core/CMakeLists.txt`, `tests/CMakeLists.txt`, `cmake/VocalSynthPlugin.cmake`,
`docs/M3-DESIGN.md` (section 19 "Implementation notes"), `docs/STATUS.md`.

Cross-group needs, all covered by the frozen headers (so no group edits another group's file): C calls
A's `CaptureTap` / `CaptureHistory` / `TakeSegmenter` / `CaptureDrain`, B's `VoicePipeline` timing
overloads (stubbed), `TakeRenderer::render` / `roundTripFor` / `computeAudioSpan` / `refineNoteOnsets`,
`SmfWriter`, `WavWriter`; B's end-to-end tests use A's classes through `CaptureDrain`; A uses B's
`maxRoundTripSamples` (implemented). Plugin headers `plugin/Capture*.h`, `ExportFiles.h`,
`FileDragSource.h` are C's to refine (nobody else includes them).

Processor changes for C (summary of sections 2, 4, 15): members `CaptureTap captureTap`,
`VsEventTimingBuffer<eventBufferCapacity> eventTimings` (cleared with `events`), `CaptureWorker` (declared
after the tap; `CaptureWorkerSources` = the three offset atomics + `recentBlockSize.get()`), instance
id / folder, `CaptureState` + mutex, raw atomics of the five parameters; `processBlock` / `processBlockBypassed`
calls as in 4; the mono downmix also in Test Pattern mode while capture audio is on; accessors for the panel
(`getCaptureWorker()`, `getCaptureTap()` requests, `getCaptureState()` / `setCaptureState()`,
`getCaptureRoundTripSamples()`, Auto value in ms).

## 18. Risks and open points

- FL's `maximumExpectedSamplesPerBlock` may differ from the ASIO buffer: Auto could be off. The Capture tab
  shows Auto in ms next to the observed block size (also stored per take); the FL checklist (M3-TESTING
  step 2) checks both 256 and 2048. If they differ, Auto switches to the observed block (8). Clap
  calibration is later (decision 3).
- Bar numbers with time-signature changes earlier in the song are approximate (6).
- FL's handling of bend range on import (reported +-2) and of CCs: verified in the FL checklist; RPN 0 is
  written anyway.
- Free takes are placed by ticks at the host bpm; in a project at another tempo they stretch (FL keeps the
  project tempo). Stated in M3-TESTING.
- Tempo changes inside large blocks are seen one block late (4.3); the timeline re-sync bounds the drift.
- The capture tail delays a synced take's appearance by ~0.5 s after Stop; a note-on emitted in that tail
  cannot start a free take (it was sung before Stop).
- A first note sung up to 1/32 beat before a bar line starts that bar and moves to tick 0 (<= 15.6 ms at
  120 BPM).
- RAM at 96/192 kHz (5.1): capped at 256 MiB per instance; the take list is capped at 2^20 events.
- A host that stops calling `processBlock` freezes the clock: free takes then close by wall clock
  (`closeIdle`), synced takes and grabs wait for the clock.

## 19. Implementation notes (integration of 0.3.0, 2026-10-17)

Changes against the text above that came up while implementing; the sections above already describe the
result.

- **TakeSegmenter, `captureOff`** (5.2): note-offs at the marker sample stay in the take (before the fix
  every event at or after the end sample was removed, so a note held at Capture off lost its note-off). The
  open-note list is rebuilt after the removal. Found by the block-size invariance test.
- **CaptureDrain** (5.3): `advanceTo (clock)` and `getLastClock()` only after a call that emptied the record
  ring. Before, a call that hit its size limit still advanced the segmenter and could close a free take
  before a queued note-on was read.
- **Grabs** (5.2): the grab's timeline covers only the samples before its capture end, so a transport record
  exactly there is never included (the result depended on the drain timing).
- **Plugin interfaces** (owner C refined them): `CaptureWorker::persistWav (number, folder)` replaces
  `markWavPersisted` (the move and the worker's deletes share one lock); `CaptureWorker` gained
  `isStarted()`, `getTake()` and `getOptions()`; `CaptureStatus` gained `started`, `takeBytes` and
  `numTakes`; `CaptureState::operator==` is written out (the defaulted `==` of the core option structs
  triggers `-Wfloat-equal` in the plugin), with `sameRenderOptions` / `sameOffset` helpers; the open-take
  snapshot does not copy the take's events. `FileDragSource` has its own file (`plugin/FileDragSource.*`);
  `TestClipExporter` uses `ExportFiles`. The worker refines note onsets in one small audio window per note
  (no whole-take audio buffer).
- **Validators**: clap-validator reports one warning per CLAP, `state-invalid-random` ("loaded random bytes
  successfully"), from JUCE's CLAP wrapper accepting arbitrary state bytes; not caused by M3.
- **End-to-end test** (`tests/plugin/ProcessorTests.cpp`, tag `[e2e]`): FX, Voice / Tenor, A3 C4 E4 A3 on
  beats 2-4 of bar 3 and beat 1 of bar 4 at 120 BPM, playhead from bar 3, voice 2 x 2048 samples late (Auto),
  host blocks 2048 / 480 / 64: one take `_Take01_Bar003_Clean.mid`, notes in order within 1/64 note of the
  beats (measured: C4 and E4 exact, A3 +4 ticks = 2 ms), WAV sample 0 = bar 3 beat 1, `.mid` byte-identical
  and WAV identical for all three block sizes.
