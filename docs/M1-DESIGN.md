# M1 design: FL Studio routing test build

Status: the scaffold is in place and was reviewed (plan:critique, see section 9). All interfaces compile;
the bodies are `TODO(<owner>)` stubs. Version 0.1.0.
Background: `docs/PLAN.md` (sections 2 and 3, M1) and `docs/RESEARCH.md` 2.5 (test matrix T1-T7).
Mandatory rules: `.claude/skills/juce-audio-plugin-dev/SKILL.md` (realtime section 1, MIDI section 2).

## 1. Goal

M1 delivers a **test build** that the user loads in FL Studio 2026 (Windows) to find out which MIDI path
works:

- T1: FX MIDI-out port -> VST synth
- T2: Patcher -> native synth
- T3: Instrument with sidechain
- T4: Patcher chain
- T5: CLAP
- T6: internal sound
- T7: virtual MIDI port

There is no voice analysis yet. The plugin **generates deterministic test MIDI** (notes, pitch bend, CC)
and can make it audible itself. It also shows the host's block sizes and how many messages it sent,
mirrors MIDI to an optional virtual port, and offers `.mid`/`.wav` test clips for drag & drop.

Both products ("VocalSynth FX", "VocalSynth Instrument") share all of this. The variant only changes the
audio output and the virtual port name:

- FX: dry voice plus the optional tone
- Instrument: only the tone

### 1.1 How each test uses the build (input for `docs/FL-ROUTING.md`)

| Test | What the user does | What tells them it works |
|---|---|---|
| T1 | FX on the mic insert, wrapper MIDI output port N; a VST3 synth on input port N | Synth plays the scale ("Notes"), bends ("Pitch Bend"), filter/mod moves ("CC Sweep", CC number set to what the synth has learned). The "sent" counters prove the plugin did emit. |
| T2 | Same as T1, but the receiver is a native FL synth inside Patcher | Same as T1 |
| T3 | Instrument in the Channel Rack, mic routed to its sidechain | Input meter and "Input: N ch" in the editor; MIDI out as in T1 |
| T4 | FX and synth inside one Patcher on the mic insert, MIDI cable between them | Same as T1 |
| T5 | T1 to T4 with the `.clap` build | Header shows "Format: CLAP"; same as T1 |
| T6 | "Monitor Tone" on, no routing | Sine follows notes and bend; works in every setup |
| T7 | "Virtual MIDI Out" on, port shows "open: VocalSynth FX"; FL MIDI settings enable that input; record into the Piano Roll | Recorded notes in the Piano Roll |
| Extra | Drag "test MIDI" into a pattern and "test WAV" into the playlist; switch "Use fixed size buffers"; bypass while notes sound | 4-bar clip in the pattern; histogram changes; no stuck notes |

Held notes (Pitch Bend, CC Sweep) are re-struck once per bar, and a "Restart pattern" button exists. So a
synth that is routed while the pattern is already running starts sounding within one bar.

## 2. Data flow (audio thread)

```
processBlock(buffer, midi)
 ├─ ScopedNoDenormals ; midi.clear() ; n = numSamples ; blockSizeHistogram.record(n)        -> UI
 ├─ if n <= 0: return                          (keep flushPending; no MIDI is allowed in a 0-sample block)
 ├─ settings = getTestPatternSettings()        (cached raw atomics, rounding + clamping)
 ├─ if flushPending.exchange(false):
 │      midiOutput.flushAllNotes(0, midi)      note-offs + bend centre for everything the host has seen
 │      generator.reset() ; monitorSynth.reset()
 ├─ input peak metering                        (before the audio is touched; unchanged from M0)
 ├─ audio: clear outputs without an input; FX: clear if muteDry; Instrument: clear
 ├─ monitorSynth.setOutputEnabled(monitorTone)
 ├─ for each chunk [start, start + len) with len <= maxChunkSamples (4096):
 │      events.clear()
 │      generator.process(len, settings, events)                     VsEventBuffer<1024>
 │      midiOutput.write(events, len, midi, start) ── mirror ──► VirtualMidiPort FIFO ─► [port thread]
 │      monitorSynth.process(events, channel pointers + start, numCh, len)   (ADDS; every block)
 ├─ currentTestNote = generator.currentNote()                                               -> UI
 └─ virtualMidiPort.setEnabled(virtualMidiOut)
```

## 3. Core (JUCE-free, `namespace vocalsynth`, `core/include/vocalsynth/`, `core/src/`)

Rules for every class:

- no allocation after construction or `prepare()`, no locks, no I/O
- `noexcept`
- no warnings under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`

### 3.1 `VsEvent.h` (implemented, not a stub)

- `enum class VsEventType : uint8_t { noteOn, noteOff, pitchBend, controlChange }`
- `struct VsEvent { type; int sampleOffset; uint8_t channel /*1..16*/; uint8_t data1; uint8_t data2; int16_t bend; }`
  plus `makeNoteOn/makeNoteOff/makePitchBend/makeControlChange` factories and `operator==`.
- `template <std::size_t Capacity> class VsEventBuffer`:
  - `std::array` storage
  - `push()` returns false when full
  - `clear()`, `size()`, `empty()`, `full()`, `operator[]`, `begin()/end()`, `view()` (returns `std::span<const VsEvent>`)
  - producers append in time order
- `class VsEventSink`: a non-owning, type-erased push handle to any `VsEventBuffer<N>`, built on a function
  pointer with no allocation. With it, non-template logic lives in `.cpp` files: template members of the
  other classes are one-line forwards to `processToSink()` / `processEvents()` / `writeEvents()`.

### 3.2 `TestPatternGenerator.h/.cpp`

```cpp
enum class TestPattern { off = 0, notes, pitchBend, controlChange, all };   // == choice param index
struct TestPatternSettings { TestPattern pattern; int channel; double bpm; int ccNumber; operator== };
class TestPatternGenerator {
    void prepare (double sampleRate) noexcept;    // calls reset()
    void reset() noexcept;                        // restart at sample 0, forget state, emits nothing
    template <size_t N> void process (int numSamples, const TestPatternSettings&, VsEventBuffer<N>&) noexcept;
    template <size_t N> void flush (int sampleOffset, VsEventBuffer<N>&) noexcept;
    void processToSink (int, const TestPatternSettings&, VsEventSink) noexcept;   // the real implementation
    void flushToSink (int, VsEventSink) noexcept;
    int currentNote() const noexcept;             // -1 if none
};
```

`TestPatternSettings::operator==` compares bpm too, so it must **not** be used to decide on a flush.

Timing model (binding, it is what makes the tests deterministic):

- **Master clock.** The beat position is derived from an internal absolute sample counter, not the host
  transport. With constant bpm, `beat(t) = (t - anchorSample) * bpm / (60 * sampleRate) + anchorBeat`.
- **Tempo changes.** On a bpm change (exact float compare with the last bpm), re-anchor at the current block
  start: `anchorSample = t`, and `anchorBeat = beat(t)` under the old bpm. The tempo changes smoothly, with
  no flush and no jump. Tempo automation re-anchors every block, so block-size invariance is only required
  (and tested) at constant bpm.
- **Event timing.** An event scheduled at beat `b` happens at the **first sample `t` with `beat(t) >= b`**,
  computed in absolute samples (e.g. `anchorSample + ceil((b - anchorBeat) * 60 * sr / bpm)`, with a guard
  against floating-point edge cases). It depends only on absolute positions, so any block split gives the
  same absolute sample positions, and all times scale with the sample rate.
- **Order at one sample.** Several events can fall on the same sample. They are emitted in this order:
  1. note-off
  2. bend centre
  3. note-on
  4. the new bend or CC value
- **`notes`.** Eighth note `k` starts at beat `k/2`. It plays `scaleNotes[k % 8]` (60 62 64 65 67 69 71 72)
  at velocity 100, and its note-off is at beat `k/2 + 0.75 * 0.5`. The note-off always comes before the next
  note-on.
- **`pitchBend`.** Note-on 60 at the pattern start. The note is held, but **re-struck every 4 beats**
  (`heldNoteRetriggerBeats`): note-off + note-on 60 at the same sample, while the bend is near 0. This makes
  it audible within one bar after the user finishes routing.
  - Bend shape: a triangle with a period of 2 beats. With `p = frac(beatsSinceStart / 2)`:
    `tri = p < 0.25 ? 4p : (p < 0.75 ? 2 - 4p : 4p - 4)`, and
    `value = tri >= 0 ? lround(tri * 8191) : lround(tri * 8192)`. So the bend goes 0 -> +8191 -> 0 -> -8192 -> 0,
    and values stay within [-8192, 8191].
  - The bend is evaluated only on grid samples `segmentStart + k * g`, with `g = max(1, lround(sr / 200))`.
    It is emitted only when the value differs from the last emitted value (initially 0, so nothing is sent
    at the start).
  - At 120 bpm and 48 kHz the grid hits the extremes exactly. Elsewhere they may be missed by up to one grid
    step.
- **`controlChange`.** Note-on 60 at the pattern start, re-struck every 4 beats like `pitchBend`.
  - CC `ccNumber` follows a triangle. With `q = frac(beatsSinceStart / 4)`:
    `value = lround(127 * (q < 0.5 ? 2q : 2 - 2q))`. So it goes 0 -> 127 (2 beats) -> 0 (4 beats).
  - It is evaluated per sample (a loop bounded by numSamples) or analytically, and emitted only on change.
  - The first value, 0, is emitted at the pattern start (after the note-on). The CC reaches both 0 and 127
    in every cycle.
- **`all`.** A 32-beat cycle: 16 beats `notes`, 8 beats `pitchBend`, 8 beats `controlChange`. Each segment
  behaves exactly like its base pattern counted from the segment start, including the re-strike at segment
  beat 4.
  - At each segment boundary the previous segment is closed at that sample (note-off of a held note, bend
    centre if not 0). Then the new segment starts at the same sample (note-on 60 where applicable).
  - The CC value is not reset when a segment ends.
- **Settings change.** If pattern, channel or ccNumber differ from the last call:
  1. flush at offset 0 (on the old channel)
  2. restart the pattern from beat 0, re-anchored at the current sample
  3. generate this block

  `off` emits nothing after that flush. The very first call after `reset()` does not flush (nothing is
  active). A bpm change alone never flushes.
- **Input sanitising.** Channel is clamped to 1..16, ccNumber to 1..119, bpm to 20..400. `numSamples <= 0`
  emits nothing and does not advance the clock.
- **`flush()`.** Emits a note-off for the active note (if any), plus a pitch-bend centre if the last emitted
  bend was not 0. It emits **nothing** if nothing is active; the processor tests rely on "pattern Off ->
  zero MIDI events".
- **Event-rate bound.** The rate is bounded in time, not per block. Upper limits:

  | Source | Limit |
  |---|---|
  | Bend | <= 200/s |
  | CC at 400 bpm | <= ~424/s |
  | Notes | <= ~27/s |
  | Boundaries and flush | <= 4 per sample position |

  The processor's 4096-sample chunks therefore hold at most ~40 events at 44.1 kHz (~220 at 8 kHz).
  `VsEventBuffer<1024>` can never fill, but if it ever did, events would be dropped, not overwritten.

### 3.3 `BlockSizeHistogram.h` (header-only)

Interface:

- `record (int numSamples)`: audio thread, wait-free
- `Snapshot snapshot() const` and `reset()`: UI thread
- `static binIndex (int)` and `static binLabel (size_t)`

Bins: `[1] [2-16] [17-32] [33-64] [65-128] [129-256] [257-512] [513-1024] [>1024]`.

The snapshot holds:

- `counts[9]` and `totalBlocks`
- `emptyBlocks`: blocks with numSamples <= 0, which are not binned
- `minBlockSize`, `maxBlockSize`, `lastBlockSize`: 0 if nothing was recorded

It uses relaxed atomics with a single writer. A reset racing a record may lose one block, which is
acceptable. The processor records in `processBlock` **and** in `processBlockBypassed`.

### 3.4 `MonitorSynth.h/.cpp`

Interface:

- `prepare (sampleRate)` and `reset()`
- `setOutputEnabled (bool)`
- `template process (const VsEventBuffer<N>&, float* const* channels, int numChannels, int numSamples)`,
  which forwards to `processEvents (std::span<const VsEvent>, ...)`

Behaviour:

- A mono sine that **adds** the same signal to every channel.
- It applies noteOn/noteOff/pitchBend at their sample offsets (clamped into the block) and ignores CC.
  Pitch bend covers +-2 semitones (`bend / 8192`).
- Envelope: linear attack 5 ms, linear release 60 ms, peak gain 0.25 (about -12 dBFS).
- A new note-on while the voice is sounding retunes it without restarting the envelope from 0, so there is
  no click.
- Output is exactly 0 when idle and never NaN or Inf. The envelope snaps to 0 at the end of the release, so
  there are no denormals.
- `isActive()` exists for tests.

**Output switch.** The processor processes the synth in **every** block and passes the "Monitor Tone"
parameter to `setOutputEnabled()`. The voice therefore keeps tracking the held note and bend while muted,
and switching the tone on during a held note is audible immediately.

- The output gain ramps linearly over 5 ms in both directions.
- When disabled and fully ramped down, nothing is written to the channels.
- `reset()` sets the gain to its target value.
- The default is enabled, so the synth can be tested on its own.

## 4. Plugin (JUCE, `plugin/`)

### 4.1 Parameters (`Parameters.h`, implemented)

| ID (version 1, permanent) | Type | Range / default |
|---|---|---|
| `muteDry` | bool | false (existing) |
| `testPattern` | choice | Off, Notes, Pitch Bend, CC Sweep, All; default Off. Index == `TestPattern` |
| `midiChannel` | int | 1..16, default 1 |
| `testTempo` | float, continuous | 60..200 BPM, default 120 |
| `ccNumber` | int | 1..119, default 1 |
| `monitorTone` | bool | false |
| `virtualMidiOut` | bool | false |

The processor caches all raw atomics in its constructor. `getTestPatternSettings()` converts them (rounding
+ clamping). It may be called from any thread, including the audio thread.

### 4.2 `MidiOutputStage.h/.cpp`

Interface:

- `setVirtualPort (VirtualMidiPort*)` and `reset()`
- `template write (const VsEventBuffer<N>&, int numSamples, juce::MidiBuffer&, int startOffset = 0)`, which
  forwards to `writeEvents (std::span<const VsEvent>, ...)`
- `flushAllNotes (int sampleOffset, juce::MidiBuffer&)`
- `isNoteActive (ch, note)`, `getNumActiveNotes()`, `isBendCentred (ch)`
- `getSentCounts()`

Rules:

- **Offsets.** `numSamples` is the length of the chunk, which starts at `startOffset` in the host block.
  Each offset is clamped to `[0, numSamples - 1]`, then `startOffset` is added. With `numSamples < 1`
  nothing is written.
- **Active notes.** Tracked in `std::array<std::bitset<128>, 16>`.
  - A note-on of an already active note gets a note-off first.
  - A note-off of an inactive note is dropped.
  - Values are clamped to MIDI ranges (channel 1..16, data 0..127).
  - Use `bitset::operator[]` only, because `test()`, `set(pos)` and `reset(pos)` can throw.
- **Bend state.** A `bendNotCentred` bitset per channel. `flushAllNotes()` sends note-offs for all active
  notes **and** a bend centre on those channels. This must work after the generator was reset, because
  the processor resets the generator while the downstream synth may still hold a note.
- **Message construction.**
  - Note-on: `juce::MidiMessage::noteOn (ch, note, (juce::uint8) vel)`
  - Pitch bend: `pitchWheel (ch, bend + 8192)`
  - CC: `controllerEvent`
- **Mirror.** Every emitted message, including those from `flushAllNotes`, is pushed to the virtual port.
  `push` returns quickly when the port is off.
- **Sent counters.** `SentCounts { noteOns, noteOffs, pitchBends, controlChanges }`, kept as relaxed
  atomics with a single writer (the audio thread). They count every message added to the MidiBuffer,
  including flushes, and are never reset. The editor shows them so the user can tell "plugin did not send"
  from "FL did not route" (T1-T5).

### 4.3 `VirtualMidiPort.h/.cpp` (T7)

Lifecycle: the constructor `VirtualMidiPort (juce::String baseName, DeviceFactory factory = {})` starts a
`juce::Thread` (the "port thread"); use `Priority::high`, because the poll interval bounds the jitter. An
empty factory means `juce::MidiOutput::createNewDevice()`; tests inject a fake `VirtualMidiPort::Device`
(one virtual `sendMessageNow`). The destructor stops the thread (`stopTimeoutMs` = 5000), which closes the
device, and then destroys what is left on the message thread.

Who calls what:

- Audio thread: `setEnabled (bool)`, `push (const juce::MidiMessage&)` and `push (const uint8_t*, int)`.
- Port thread: polls `enabled`, requests a device, drains the FIFO into `Device::sendMessageNow()`, sends the
  closing note-offs, hands the device back. It makes no slow native call and never creates or destroys a
  device.
- Message thread: **creates and destroys every device** (`handleAsyncUpdate` of a private `AsyncUpdater`).
  JUCE 9 builds `MidiOutput` on `ump::Output`: its `Impl` constructor/destructor and
  `add/removeDisconnectionListener` assert the message thread (`JUCE_ASSERT_MESSAGE_THREAD`) and are not
  synchronised against it, and the native registries (`virtualEndpoints` on Windows, `virtualPorts` on
  Linux, both a `std::map`) are read on the message thread. On Windows the WinRT session is also initialised
  for the message thread only. Creating or destroying a device on another thread would race with all of
  this.
- UI: `getStatus()` (`off/open/failed`), `getOverflowCount()` and `getDeviceName()`. `getDeviceName()` uses
  a CriticalSection between the port thread and the UI only, never on the audio thread.

**SPSC FIFO.**

- `juce::AbstractFifo` (capacity 4096) plus a preallocated `std::array<Message, 4096>`, where
  `Message = { uint8_t bytes[3]; uint8_t size; }`.
- Messages longer than 3 bytes are dropped.
- A full FIFO increments `overflowCount` and never blocks.
- While the port is not open, `push()` returns false without counting an overflow.

**Handshake port thread <-> message thread.** One `handoffLock` (CriticalSection) guards a few flags and
pointer slots. It is held only to swap them, never across a native call, never on the audio thread.

- **Opening.** If `enabled` and there is no device, the port thread sets `openRequested` and triggers the
  AsyncUpdater. The message thread reserves a name (registry lock), creates the device **outside every
  lock**, rolls the name back on failure, and publishes the result (`opened` with the device, `failed`, or
  `cancelled` if the port was disabled meanwhile). The port thread adopts the device on its next poll and
  only then reports `open`.
  - `failed` (factory returned nullptr or threw, or no free name): retry at most every `retryIntervalMs`
    (3 s), never crash.
  - No answer within `retryIntervalMs` (no message loop): status `failed`; the request stays pending and is
    completed whenever the message thread runs.
  - A failed port that gets disabled goes back to `off`. A device that arrives after disabling goes
    straight back.
- **Closing.** On disable or destruction, the port thread
  1. drains what is still in the FIFO into the device
  2. sends a note-off for every note in `sentNotes` (its own bitset, updated from what it sent) and a bend
     centre where the last bend sent was not centred
  3. moves the device into the `retired` list and triggers the AsyncUpdater
  4. sets status `off` and discards the FIFO.
  The message thread then destroys the device and releases the name.
- **Destruction.** After a clean `stopThread`, the destructor destroys the retired and not-yet-adopted
  devices directly if it runs on the message thread (the plugin wrappers do that), otherwise it hands them
  over with `MessageManager::callAsync`. If the port thread had to be killed after the timeout, the devices
  are leaked on purpose (the killed thread may have been inside one) and no lock is taken.
- **Naming.** A process-wide list of names in use (static in `VirtualMidiPort.cpp`, its own lock, held only
  for the list operation). The name is baseName, or `baseName + " 2"`, `" 3"` ... up to
  `maxInstanceNumber` (16). `MidiInput/MidiOutput::getAvailableDevices()` are not used (section 10).
- **Polling.** Every `pollIntervalMs` (1 ms) while open, every `idlePollIntervalMs` (50 ms) while off,
  failed or waiting for the message thread (which wakes the port thread with `notify()`). `wait()` returns
  immediately on `stopThread`.
- **Draining.** Drain the FIFO into `device->sendMessageNow()` and update `sentNotes`. Without a device,
  discard the messages.
- **Jitter (known, documented in FL-ROUTING).** All messages of one host block are pushed at once and sent
  by the next poll. Their timing in the recording is therefore quantised to about one audio block plus
  1 ms.
- **Cost on the message thread.** Device creation can take a moment on Windows (Windows MIDI Services);
  it now blocks FL's message thread briefly when "Virtual MIDI Out" is switched on. That is the price of
  JUCE's threading contract.
- **Tests.** The real device is only tried in the `[system]` test (CI has no MIDI services, `open` and
  `failed` are both accepted). All other port tests use a fake factory: creation/destruction on the message
  thread and sending on the port thread, mirrored messages in order (also `MidiOutputStage::flushAllNotes`),
  closing note-offs and bend centre (incl. velocity-0 note-ons), numbered names, factory failure and throw,
  no message loop, disable while a request is pending, destruction while open.

### 4.4 `PluginProcessor.h/.cpp`

New members (declared in the header):

- cached param atomics
- `TestPatternGenerator generator`, `VsEventBuffer<1024> events`, `MonitorSynth monitorSynth`,
  `MidiOutputStage midiOutput`
- `VirtualMidiPort virtualMidiPort`: named per variant, wired via `midiOutput.setVirtualPort` in the
  constructor
- `std::atomic<bool> flushPending { true }`, `BlockSizeHistogram blockSizeHistogram`,
  `std::atomic<double> currentSampleRate`, `std::atomic<int> currentTestNote { -1 }`
- constant `maxChunkSamples = 4096`

Public accessors (UI thread):

- `getBlockSizeHistogram()`, `resetBlockSizeHistogram()`
- `getCurrentSampleRate()`, `getCurrentTestNote()`, `getTestPatternSettings()`
- `getVirtualMidiStatus()`, `getVirtualMidiOverflowCount()`, `getVirtualMidiDeviceName()`
- `getMidiSentCounts()`
- `requestFlush()` (any thread): the "Restart pattern" button

Rules:

- **`prepareToPlay`:** `generator.prepare(sr)`, `monitorSynth.prepare(sr)`, store `currentSampleRate`, set
  `flushPending`. Do **not** reset `midiOutput` there; it must still know which notes to turn off.
- **`reset()` and `releaseResources()`:** only set `flushPending` (and reset the input peak).
- **`processBlock`:** exactly the order in section 2.
  - `midi.clear()` always comes first.
  - With `numSamples <= 0`, record in the histogram and return; `flushPending` stays set.
  - Pending flush: `midiOutput.flushAllNotes(0, midi)` **before** anything of this block is generated,
    then `generator.reset()` + `monitorSynth.reset()`. The output stage is the single source of truth for
    what the downstream synth holds. It covers notes from before `prepareToPlay` too, and emits nothing if
    nothing is active. The restarted pattern's first note-on then lands at offset 0, after the note-offs
    (`MidiBuffer::addEvent` keeps insertion order for equal sample positions).
  - **Chunking:** the generator, `midiOutput.write (..., start)` and the monitor synth run per chunk of at
    most `maxChunkSamples`. Channel pointers come from `buffer.getWritePointer (ch, start)`, for at most
    2 channels (the layouts are mono or stereo).
- **Audio:**
  - FX: the dry voice, cleared when `muteDry`, then the monitor tone added.
  - Instrument: cleared, then the tone.
  - `monitorSynth.setOutputEnabled (monitorTone)` once per block, and `monitorSynth.process (...)` in
    **every** block (see 3.4).
- **`processBlockBypassed`:**
  - `midi.clear()`, and record in the histogram.
  - If `numSamples >= 1`: `midiOutput.flushAllNotes(0, midi)`, `generator.reset()`,
    `monitorSynth.reset()`, `flushPending = false`. Repeated bypassed blocks then emit nothing, and the
    pattern restarts after un-bypass.
  - `currentTestNote = -1`, `virtualMidiPort.setEnabled(...)` as in `processBlock`.
  - Audio as before: the FX passes through, the Instrument is silent.
- Keep `getLatencySamples() == 0` and the existing input peak metering.

### 4.5 `TestClipExporter.h/.cpp` (namespace of free functions, message thread only)

Interface:

- `getExportDirectory()`: `<temp>/VocalSynth/`, created if missing
- `renderTestPattern (const TestPatternSettings&)`: returns a `juce::MidiMessageSequence`
- `exportMidi (settings)` and `exportWav()`: return a `juce::File`, or an empty File on failure

**MIDI file.**

- **Render.** Run `TestPatternGenerator` offline at 48 kHz in blocks (e.g. 480) for exactly
  `ceil (16 * 60 * 48000 / bpm)` samples (4 bars). Then flush, and place the flush events at
  `endTick = 15360`.
- **Ticks.** `tick = round (sample * bpm / 60 / 48000 * 960)`.
- **Meta events.** A track name ("VocalSynth test pattern - <pattern>") and a tempo meta event at tick 0,
  and end-of-track at `endTick`.
- **File.** Call `updateMatchedPairs()`, then write a `juce::MidiFile` with `setTicksPerQuarterNote (960)`,
  named e.g. `VocalSynth-Test-<Pattern>.mid`.
- **Pattern Off** renders Notes.
- **Expected note-ons in 4 bars:**

  | Pattern | Note-ons |
  |---|---|
  | Notes | 32 |
  | Pitch Bend | 4 (re-struck every bar) |
  | CC Sweep | 4 |
  | All | 32 (the All cycle starts with 16 beats of notes) |

**WAV file.** 2 s, 440 Hz sine, mono, -6 dBFS, 48 kHz, 24 bit, written with `juce::WavAudioFormat`
(96000 samples) as `VocalSynth-Test-440Hz.wav`.

**File locking.** FL may still hold a previously dropped file open. Write to a temporary file and move it
into place; if that fails, use `getNonexistentSibling()`.

### 4.6 `PluginEditor.h/.cpp` (about 640 x 440)

The editor inherits `juce::AudioProcessorEditor`, `juce::DragAndDropContainer` and `juce::Timer` (30 Hz;
it reads only atomics and accessors). All UI text is English.

**Header.** "VocalSynth FX - M1 routing test build" or "VocalSynth Instrument - ...", plus
"Version " `VOCALSYNTH_VERSION_STRING` and **"Format: <x>"**:

- the format comes from `juce::AudioProcessor::getWrapperTypeDescription (processor.wrapperType)`
- `wrapperType_Undefined` is shown as "CLAP", because clap-juce-extensions calls `createPluginFilter()`
  directly and never sets a wrapper type
- this lets T5 screenshots prove which build was loaded

**Controls with APVTS attachments:**

- pattern ComboBox, channel, tempo, CC number
- Monitor Tone toggle
- Virtual MIDI toggle, with status text ("off" / "open: <name>" / "failed - see FL-ROUTING.md") and the
  overflow count
- Mute Dry (FX only)

**Displays and buttons:**

- input meter (keep), plus "Input: N ch" from `getTotalNumInputChannels()` (T3: shows whether FL enabled
  the sidechain bus)
- current test note (name + number) and sample rate
- **sent counters** (note-ons, note-offs, pitch bends, CCs)
- block-size histogram: compact text or bars, min/max/last, with a reset button
- a **"Restart pattern"** button that calls `requestFlush()`

**Drag sources** "Drag test MIDI" / "Drag test WAV": on the first `mouseDrag` of a gesture, export the file
and call `performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this)`.

## 5. Threading summary and accepted risks

| Thread | Touches |
|---|---|
| Audio | generator, events, monitorSynth, midiOutput (incl. sent counters), `virtualMidiPort.setEnabled/push`, histogram `record`, atomics |
| Port thread (one per instance) | `Device::sendMessageNow` (no create/destroy), FIFO read side, `sentNotes`, closing note-offs, status atomic, deviceName (lock with UI), handoff slots (`handoffLock`) |
| Message/UI | editor, attachments, histogram `snapshot/reset`, accessors, `requestFlush`, TestClipExporter (file I/O), virtual port device create/destroy and name list (`VirtualMidiPort::handleAsyncUpdate`) |

Accepted for M1:

- **MidiBuffer growth.** `juce::MidiBuffer::addEvent` can allocate when the host's buffer is too small.
  The VST3 wrapper and the CLAP wrapper call `ensureSize (2048)`, which is about 200 short messages, far
  above the per-block maximum here. So the risk is theoretical.
- **Virtual port enable flag.** It is updated from `processBlock`/`processBlockBypassed`. If the host stops
  calling them, the toggle applies on the next block. Without processing no MIDI is generated anyway.
- **CLAP bypass.** The CLAP wrapper (clap-juce-extensions) never calls `processBlockBypassed`; the host
  bypasses a CLAP by not calling process. Stuck notes on CLAP bypass are therefore up to FL; check them in
  T5.
- **CLAP sub-blocks.** The CLAP wrapper also splits blocks at parameter events, so the histogram shows
  smaller blocks under automation.
- **Virtual port jitter.** About one audio block (section 4.3).

## 6. Tests (Catch2)

Core (`VocalSynthCoreTests`):

- `TestPatternGeneratorTests.cpp`:
  - per pattern, the same absolute event positions for block sizes {1, 7, 64, 480, 512, 4096} as for one
    big block
  - timing scales with sample rate: at 44.1/48/96 kHz, event times in seconds match within 1 sample
  - note-on/off pairing, and no overlapping notes on a channel
  - scale order and velocity; gate 75 %
  - held-note re-strike every 4 beats (off before on at the same sample)
  - bend: values in [-8192, 8191]; exact extremes at 120 bpm / 48 kHz; at least `g` samples apart; only on
    change
  - CC: values in 0..127, only on change, reaches 0 and 127
  - `all`: cycle segment lengths
  - a pattern, channel or CC change flushes at offset 0 (old channel) and restarts; a bpm change does not
    flush
  - `off` emits nothing; flush with nothing active emits nothing
  - offsets within [0, numSamples)
  - the worst-case event count per 4096 samples at 8 kHz / 400 bpm stays below 1024
- `BlockSizeHistogramTests.cpp`:
  - every bin boundary (1, 2, 16, 17, ... 1024, 1025)
  - min/max/last/total, empty blocks, reset
- `MonitorSynthTests.cpp`:
  - silence without notes (exactly 0); adds into existing content
  - no NaN/Inf; peak <= about 0.25
  - release reaches exactly 0 after about 60 ms
  - bend changes the frequency
  - block-size invariance
  - output disabled -> writes nothing; enabled during a held note -> sound within about 5 ms

Plugin (`VocalSynthPluginTests`):

- `MidiOutputStageTests.cpp`:
  - conversion of each event type
  - offset clamping incl. `startOffset`
  - note-on/off pairing and duplicate note-on handling
  - `flushAllNotes` (notes + bend centre), reset, multiple channels
  - sent counters
- `ProcessorTests.cpp`:
  - each pattern across block sizes {1, 7, 64, 480, 512, 4096} **and one 10000-sample block** (chunking):
    offsets valid, and every note-on gets a note-off after switching the pattern off or bypassing
  - the first block after prepareToPlay/reset flushes, with note-offs before the new note-on at offset 0
  - `requestFlush()` restarts the pattern
  - Monitor Tone switched on during Pitch Bend gives sound
  - state save/restore of all new params
  - the existing M0 tests stay green
- `TestClipExporterTests.cpp`:
  - the MIDI file is readable by `juce::MidiFile`: 960 PPQ, the expected note-on counts (section 4.5), a
    tempo event, last event tick <= 15360
  - the WAV is readable: 96000 samples at 48 kHz, 24 bit

## 7. File ownership

| Agent | Files |
|---|---|
| core-gen | `core/*/VsEvent.h`, `core/*/TestPatternGenerator.*`, `tests/core/TestPatternGeneratorTests.cpp` |
| core-aux | `core/*/BlockSizeHistogram.h`, `core/*/MonitorSynth.*`, `tests/core/BlockSizeHistogramTests.cpp`, `tests/core/MonitorSynthTests.cpp` |
| midi-out | `plugin/MidiOutputStage.*`, `plugin/VirtualMidiPort.*`, `tests/plugin/MidiOutputStageTests.cpp` |
| processor | `plugin/PluginProcessor.*`, `plugin/Parameters.h`, `plugin/PluginFactory.cpp`, `tests/plugin/ProcessorTests.cpp` |
| ui | `plugin/PluginEditor.*`, `plugin/TestClipExporter.*`, `tests/plugin/TestClipExporterTests.cpp` |
| docs | `docs/FL-ROUTING.md`, `packaging/windows/LIESMICH.txt`, `README.md`, `docs/PLAN.md` |
| integration (later) | CMake files, CI workflow, anything needed to make the build green |

Dependencies between agents:

- The processor tests need core-gen, core-aux (MonitorSynth) and midi-out.
- The TestClipExporter tests need core-gen.
- Each agent can work against the scaffold interfaces in parallel; the interfaces in this document are
  frozen. An agent that needs to change a public interface reports that in its "issues" instead of editing
  another agent's file.

## 8. Other M1 items

- `VERSION` is 0.1.0 (done in the scaffold).
- **CI:** remove `--exclude 'param-set-events|param-set-no-cookies'` from the clap-validator step in
  `.github/workflows/build_and_test.yml` once those tests pass locally with the continuous `testTempo`.
  Done: both pass, the exclusion is gone (see section 10).
- **Docs (German):**
  - `docs/FL-ROUTING.md`: a step-by-step T1-T7 protocol and a results table to fill in
  - `packaging/windows/LIESMICH.txt` (keep CRLF)
  - the README status section
  - PLAN.md M1 checkboxes for what was built (the FL tests themselves stay open)
- **FL-ROUTING.md must also cover:**
  - which build and format is loaded (header "Format:")
  - setting the CC number to one the target synth responds to
  - "Restart pattern" after changing the routing
  - the "sent" counters for telling "not sent" from "not routed"
  - double notes if T1 routing and the T7 port feed the same synth at the same time
  - enabling or refreshing the virtual port in FL's MIDI settings, and multiple instances
    ("VocalSynth FX 2")
  - stuck-note checks: bypass, mixer slot disable, removing the plugin while a note sounds, separately
    for VST3 and CLAP
  - "Use fixed size buffers" on/off with the histogram
  - checking that the reported latency is 0 (FL mixer/wrapper shows no PDC)

## 9. Review log (plan:critique)

Changes against the first draft, with the reason for each:

1. **Pending-flush order.** The first draft's section 2 ran `flushAllNotes` *after* writing the new block.
   That would have killed the restarted pattern's first note while the generator still believed it was
   held. Now the order is `flushAllNotes(0)` -> reset -> generate, and `generator.flush()` is not used by
   the processor.
2. **Chunking** (`maxChunkSamples`, `MidiOutputStage::write (..., startOffset)`). Long host blocks could
   overflow the 1024-event buffer, and a dropped note-off is a stuck note. The SKILL.md rule says to
   process large blocks in chunks.
3. **Held notes re-struck every bar** (Pitch Bend, CC Sweep). Otherwise a synth routed after the pattern
   started never gets a note-on and stays silent, so T1-T5 bend/CC tests could fail falsely. This also
   changes the expected exporter counts (4 instead of 1).
4. **`MonitorSynth::setOutputEnabled`**, with the synth processed every block. Otherwise switching
   Monitor Tone on during a held note gave silence (T6).
5. **`VirtualMidiPort`:**
   - devices created and destroyed on the message thread only (JUCE 9 threading contract), handed to and
     from the port thread through a small handshake; a process-wide name list
   - note-offs before closing, and the FIFO discarded
   - 1 ms / 50 ms polling, a high-priority thread, a 5 s stop timeout
6. **Every 0-sample block** still clears the MidiBuffer, and nothing is flushed into a 0-sample block.
7. **UI diagnostics for the FL tests:** sent counters, "Format:" (T5), "Input: N ch" (T3), "Restart
   pattern".
8. **Exporter:** end-of-track at 4 bars, an exact render length, and a fallback for files locked by FL.
9. **Precise bend/CC formulas, and the order of events on one sample.** These make the core-gen tests
   unambiguous.

## 10. Integration notes (deviations found while implementing)

The implementation follows this document except for these points, each made for the reason given:

1. **`VirtualMidiPort` name selection (4.3)** does not call `MidiInput/MidiOutput::getAvailableDevices()`.
   Only the process-wide list of names in use is checked. A name owned by another process (e.g. a bridged
   FL plugin process) is not detected; a clash gives a duplicate name or `failed`.
2. **All `juce::MidiOutput` creation and destruction happens on the message thread** (a private
   `AsyncUpdater` of the port), because JUCE 9's `ump::Output` and the native backends assert the message
   thread and are not synchronised against it. The first version created the device on the port thread
   under a process-wide lock; a review found races with JUCE's message-thread code (disconnect listeners,
   endpoint maps) and a kill-while-holding-the-lock risk in the destructor. Without a running message loop
   the port reports `failed` after 3 s.
3. **processBlock order (2, 4.4):** `virtualMidiPort.setEnabled` is called at the top, so a 0-sample block
   also updates it. `monitorSynth.setOutputEnabled` is called before any `reset()`/`prepare()` of the synth
   (also in `prepareToPlay`), otherwise the gain ramps from a stale state and gives a 5 ms blip with
   Monitor Tone off.
4. **Pending flush and the monitor tone:** the flush calls the new `MonitorSynth::releaseAll()` (gate off
   and bend centre, 60 ms release) instead of `reset()`, so "Restart pattern" with an audible tone does not
   click. `reset()` is still used in `prepare()` and when bypassed (bypass renders no tone anyway).
5. **State load notifies the host:** `setStateInformation` calls
   `updateHostDisplay (ChangeDetails().withProgramChanged (true))`. clap-juce-extensions turns that into
   `params->rescan (CLAP_PARAM_RESCAN_VALUES)`, which the CLAP spec asks for after a state load and which
   clap-validator's `state-reproducibility-*` tests check. VST3 ignores it (no program parameter).
6. **Note names in the editor** follow FL Studio (MIDI note 60 = "C5"), with the MIDI number next to it.
7. **The port status text** reads "Port: off" / "Port: open: <name>" / "Port: failed - see FL-ROUTING.md";
   FL-ROUTING.md is shipped in the Windows ZIP.
8. **Editor smoke tests** live in `tests/plugin/PluginEditorTests.cpp`.
