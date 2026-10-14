---
name: juce-audio-plugin-dev
description: >
  Project rules for VocalSynth, a JUCE 8 / C++ VST3 plugin for FL Studio that turns live voice into
  MIDI (pitch tracking to notes, beatbox hits to drum triggers). Use for any work on the
  AudioProcessor, processBlock, DSP (pitch detection, onset detection, classification), parameters
  (APVTS), state save/restore, MIDI output, the editor/UI thread boundary, CMake/JUCE build setup,
  unit tests, pluginval, or FL Studio testing and MIDI routing. Also use when someone asks about CPU,
  latency, or "is this safe on the audio thread?". For a structured review of finished code, also
  run the juce-review and audio-dsp-review skills.
---

# VocalSynth: JUCE audio plugin development rules

The plugin is an audio effect (FL mixer insert) that takes mic audio in and sends MIDI out.
Correctness on the audio thread comes first, then latency, then features.

## 1. Real-time audio thread rules (non-negotiable)

`processBlock` and everything it calls must finish in a bounded time. Inside it:

- **No allocation**: no `new`/`delete`, `make_unique`/`make_shared`, `std::vector::push_back`/`resize`
  past reserved capacity, `std::string`/`juce::String` building, `std::function` with heap captures,
  `AudioBuffer::setSize`, constructing `juce::dsp::FFT` or `MidiMessage` sysex. Allocate everything
  in `prepareToPlay` (or the constructor) at the worst-case size.
- **No locks or waiting**: no `std::mutex` (not even `try_lock`), `CriticalSection`, `ScopedLock`,
  condition variables, futures, `MessageManagerLock`, `callFunctionOnMessageThread`.
- **No system calls or I/O**: no `DBG`, `Logger`, `printf`/`std::cout`, files, sockets, sleep, `callAsync`.
- **No exceptions** thrown or caught, and no unbounded loops (worst case, not average case, counts).
- Start the callback with `juce::ScopedNoDenormals noDenormals;`.
- Never assume `buffer.getNumSamples() == samplesPerBlock`. Hosts (FL Studio especially) send
  smaller and changing block sizes, including 0 or 1 sample. Treat `samplesPerBlock` as a hint,
  and process larger blocks in chunks if they ever arrive.
- Results must not depend on block size: the DSP should run on its own hop/frame clock with
  internal ring buffers, and not assume one analysis frame per block.

**Moving data between threads**

| Need | Use |
|---|---|
| One scalar (detected pitch, level, confidence) audio -> UI | `std::atomic<float>` / `std::atomic<int>` with `memory_order_relaxed`; UI reads it from a `juce::Timer` (30-60 Hz) |
| A stream of events (detected hits for a UI log/meter) | `juce::AbstractFifo` + preallocated `std::array`; the audio thread is the only writer, the UI thread is the only reader (SPSC) |
| A larger config (classifier templates, trained profile) UI -> audio | Build it on the message or background thread, publish with an atomic pointer swap or an SPSC queue of pointers, and free the old one off the audio thread (never `delete` on the audio thread) |
| Parameters | APVTS raw atomics (section 2) |

Add `static_assert(std::atomic<float>::is_always_lock_free);` once to document the assumption.
Do not use `juce::SpinLock::enter()` on the audio thread; at most use `tryEnter()` with a fallback.

## 2. AudioProcessor patterns

**Parameters (APVTS)**
- Use one `juce::AudioProcessorValueTreeState apvts { *this, nullptr, "PARAMS", createLayout() };`.
- Use `juce::ParameterID { "threshold", 1 }` with a version hint. **Parameter IDs are permanent**:
  renaming one breaks saved FL projects and automation. Add new parameters, never repurpose old ones.
- In the constructor, cache `std::atomic<float>* thresholdParam = apvts.getRawParameterValue("threshold");`
  and read `->load(std::memory_order_relaxed)` in `processBlock`. Never call `apvts.getParameter()`,
  `apvts.state`, or `ValueTree` from the audio thread.
- Smooth gain-like values with `juce::SmoothedValue` (call `reset(sampleRate, seconds)` in `prepareToPlay`).
- Do not write parameters from the audio thread. Publish analysis results through atomics instead.
- UI controls use `SliderAttachment`/`ButtonAttachment`/`ComboBoxAttachment`. The editor must not
  hold raw pointers into DSP objects without thinking about lifetime; the editor can be destroyed at any time.

**State**
```cpp
void getStateInformation (juce::MemoryBlock& dest) override {
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}
void setStateInformation (const void* data, int size) override {
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}
```
Keep non-parameter state (such as a trained beatbox profile) as a child `ValueTree` in `apvts.state`
with a `version` property, and migrate older versions on load. Loading must tolerate missing or extra data.

**Lifecycle**
- `prepareToPlay(sampleRate, maxBlock)` can be called repeatedly, with different rates. In it,
  (re)allocate, call `prepare(spec)` + `reset()` on every DSP object, clear ring buffers and onset
  state, and recompute sample-rate-dependent constants. No hard-coded 44100.
- `releaseResources()` may free memory. `reset()` clears state without reallocating.
- `getTailLengthSeconds()` returns 0. Call `setLatencySamples()` only if audio output is actually delayed.
- `isBusesLayoutSupported`: accept mono or stereo input with the same layout on the output. Pass the
  audio through (or offer a "mute dry" parameter) so the mixer insert stays usable.

**MIDI output**
- CMake: `NEEDS_MIDI_OUTPUT TRUE`, `NEEDS_MIDI_INPUT FALSE`, `IS_SYNTH FALSE`, `IS_MIDI_EFFECT FALSE`
  (a JUCE MIDI-effect has no audio input, and we need the mic). Then `producesMidi()` returns true,
  `acceptsMidi()` returns false, and `isMidiEffect()` returns false.
- `MidiBuffer& midi` is in/out: `midi.clear()` first, then add events with
  `midi.addEvent (juce::MidiMessage::noteOn (ch, note, (juce::uint8) vel), sampleOffset)`, where
  `0 <= sampleOffset < numSamples` (put it at the detected onset sample, not at 0).
  Channels are 1-16. Don't modify a `MidiBuffer` while iterating over it.
- **No stuck notes**: track the active note(s), and always send the note-off before a new pitched
  note-on. Flush note-offs on bypass (`processBlockBypassed`), on parameter changes of channel or
  mode, in `reset()`/`prepareToPlay`, and when the input drops below the gate. Drum triggers send a
  note-on followed by a note-off a few ms later (track the countdown across blocks).
- VST3 note on/off is reliable. CC, pitch bend and aftertouch out go through VST3 "legacy MIDI CC out"
  events: host support varies, so check them in FL Studio before relying on them.

## 3. VocalSynth DSP guidance

- Pitch: use a time-domain detector (YIN or MPM/NSDF) on a ring buffer. The window must hold at
  least 2 periods of the lowest pitch (80 Hz -> ~25 ms, ~1200 samples at 48 kHz). Use hop sizes of
  64-256 samples. Precompute windows and FFT plans in `prepareToPlay`.
- Notes: convert with `69 + 12*log2(f/440)`. Use a confidence/RMS gate, hysteresis (stay on the
  current note until the pitch is ~50-70 cents past the boundary for N ms), and a minimum note length.
  Keep raw pitch for an optional pitch-bend mode.
- Beatbox: use energy/spectral-flux onset detection with a short hop (<= 128 samples), adaptive
  threshold and a refractory period. Classify (kick/snare/hat...) on a short window after the onset
  using precomputed features (band energies, spectral centroid, ZCR). Keep onset-to-MIDI under ~10-15 ms.
- DSP classes are plain C++ (no `AudioProcessor` dependency) so they can be tested headless.

## 4. CPU and latency budgets

- Block time = `numSamples / sampleRate` (256 @ 48 kHz = 5.33 ms). Keep the **worst-case**
  `processBlock` time under ~20% of the block time on a mid-range laptop. Measure in Release builds with
  `juce::AudioProcessLoadMeasurer` or a timer around the callback, logged via atomics.
- Spread expensive work across hops so no single block has a CPU spike.
- Live feel: aim for <= 20 ms total from voice to sound (ASIO buffer + analysis window + synth).
  Report analysis delay in the UI or docs. Don't add audio latency through `setLatencySamples` just for analysis.

## 5. Build (CMake + JUCE 8)

- CMake >= 3.22, C++20 (C++17 is the JUCE 8 minimum). Pin JUCE to an exact tag (git submodule
  or `FetchContent`), never a moving branch.
- `juce_add_plugin(VocalSynth COMPANY_NAME ... PLUGIN_MANUFACTURER_CODE Xxxx PLUGIN_CODE Xxxx
  FORMATS VST3 Standalone PRODUCT_NAME "VocalSynth" NEEDS_MIDI_OUTPUT TRUE IS_SYNTH FALSE
  VST3_CATEGORIES Fx MICROPHONE_PERMISSION_ENABLED TRUE)`. The 4-char codes must never change after release
  (the manufacturer code needs at least one uppercase letter).
- `target_compile_definitions(... PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)`.
- Link `juce::juce_audio_utils juce::juce_dsp` PRIVATE, and `juce::juce_recommended_config_flags
  juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags` PUBLIC.
- Put DSP in a separate static library target (e.g. `VocalSynthDSP`) shared by the plugin and the tests.
- Windows output: `build/VocalSynth_artefacts/Release/VST3/VocalSynth.vst3`. `COPY_PLUGIN_AFTER_BUILD`
  copies it to `C:\Program Files\Common Files\VST3` and needs admin rights, so keep it off by default.
- JUCE 8 is AGPLv3 or commercial-licensed. Check the license before distributing binaries.

## 6. Testing

- **Unit tests** (Catch2 v3 via `FetchContent`, link `Catch2::Catch2WithMain`, `catch_discover_tests`;
  GoogleTest with `gtest_discover_tests` is fine too) on the DSP library, no host needed:
  - Sines at known frequencies -> detected pitch within +-10 cents. Also octave-error checks and
    harmonics-rich signals (saw, recorded vowels).
  - Silence/noise -> no notes. Impulses/recorded hits -> exactly one trigger at the right sample (+- hop).
  - Run every test at 44.1/48/96 kHz and block sizes {1, 7, 64, 480, 512, 4096}: results must match.
  - MIDI invariants: every note-on has a note-off, offsets are in range, and there are no duplicate note-ons.
  - Keep audio fixtures small (short WAVs in `tests/data`), with fixed seeds for noise.
- **pluginval** (see the `validate-pluginval` skill): `pluginval --strictness-level 5 --validate <path>.vst3`
  locally. In CI, use `--skip-gui-tests` and work toward level 10. Fix every failure; state and
  parameter tests catch real FL Studio bugs.
- Also test in the Standalone build with a real microphone before testing in a DAW.

## 7. FL Studio testing notes

- Install the `.vst3` to `C:\Program Files\Common Files\VST3`, then in the Plugin Manager run
  "Find installed plugins". Mark it as an effect (it should be detected as one through `Fx`).
- Load VocalSynth on a **mixer insert** whose input is the mic (ASIO driver, 128-256 sample buffer).
- **MIDI routing**: in VocalSynth's wrapper settings (gear icon, MIDI section) set a **MIDI output port**
  (e.g. 1). On the target channel-rack instrument, set the same **MIDI input port** (VST wrapper
  settings, or the port field in the channel's Miscellaneous settings for native generators). Several
  instruments can listen on one port, and MIDI channels separate the pitched notes from the drum triggers.
- To record, arm recording of notes and play. FL records the incoming notes into the target
  channel's pattern or piano roll.
- Check: changing block size/sample rate in Audio Settings mid-session, the "Use fixed size buffers" wrapper
  option on and off, bypass/disable while notes sound (no stuck notes), save and reload the project (state),
  automation of every parameter, multiple instances, and removing the plugin while audio runs.
