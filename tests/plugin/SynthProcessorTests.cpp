// Processor with the synth (docs/SYNTH-DESIGN.md 17, 21): synth audible in FX and Instrument, dryLevel / muteDry,
// 0 dB dry bit-identical to 0.3.1, processor output block-size invariant, no allocation in processBlock, nothing
// hangs on bypass / flush / source change, tables published when setStateInformation returns, state round trip
// with an embedded user table, preset load sets no voice parameter, CLAP program change after a preset load,
// the 0.4.0 state fixtures (tests/data/state-0.4.0-*), and the testable acceptance points of ROADMAP-0.4 4.1.
// Owner: integration.

#include "Parameters.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "SynthDataTestUtils.h"
#include "SynthParameters.h"
#include "SynthState.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace vocalsynth;
namespace testdata = vocalsynth::testdata;

//==============================================================================
// Allocation counter for processBlock (as tests/core/synth/SynthEngineTests.cpp): glibc lets an executable define
// malloc / calloc / realloc and forward to __libc_*; counting is armed per thread around the real-time calls only.
// Not available on other C libraries or under sanitizers: the test then reports a skip.
#if defined(__GLIBC__) && ! defined(__SANITIZE_ADDRESS__) && ! defined(__SANITIZE_THREAD__)
 #if defined(__has_feature)
  #if ! __has_feature(address_sanitizer) && ! __has_feature(thread_sanitizer) && ! __has_feature(memory_sanitizer)
   #define VOCALSYNTH_COUNT_MALLOC 1
  #endif
 #else
  #define VOCALSYNTH_COUNT_MALLOC 1
 #endif
#endif

#if defined(VOCALSYNTH_COUNT_MALLOC)
extern "C" void* __libc_malloc (std::size_t size) noexcept;
extern "C" void* __libc_calloc (std::size_t count, std::size_t size) noexcept;
extern "C" void* __libc_realloc (void* p, std::size_t size) noexcept;

namespace
{
constinit thread_local bool countingMallocs = false;
constinit thread_local std::size_t mallocCount = 0;
} // namespace

extern "C" void* malloc (std::size_t size) noexcept
{
    if (countingMallocs)
        ++mallocCount;

    return __libc_malloc (size);
}

extern "C" void* calloc (std::size_t count, std::size_t size) noexcept
{
    if (countingMallocs)
        ++mallocCount;

    return __libc_calloc (count, size);
}

extern "C" void* realloc (void* p, std::size_t size) noexcept
{
    if (countingMallocs)
        ++mallocCount;

    return __libc_realloc (p, size);
}
#endif

namespace
{
#if defined(VOCALSYNTH_COUNT_MALLOC)
struct CountMallocs
{
    CountMallocs() noexcept
    {
        mallocCount = 0;
        countingMallocs = true;
    }
    ~CountMallocs() { countingMallocs = false; }
    CountMallocs (const CountMallocs&) = delete;
    CountMallocs& operator= (const CountMallocs&) = delete;
    std::size_t count() const noexcept { return mallocCount; }
};
#endif

juce::File dataFile (const juce::String& name)
{
    // UTF-8: the checkout path may contain umlauts (Windows)
    return juce::File (juce::String (juce::CharPointer_UTF8 (VOCALSYNTH_TEST_DATA_DIR))).getChildFile (name);
}

juce::RangedAudioParameter& param (VocalSynthProcessor& p, const juce::String& id)
{
    auto* parameter = p.getParameters().getParameter (id);
    REQUIRE (parameter != nullptr);
    return *parameter;
}

void setPlain (VocalSynthProcessor& p, const juce::String& id, float value)
{
    auto& parameter = param (p, id);
    parameter.setValueNotifyingHost (parameter.convertTo0to1 (value));
}

float getPlain (VocalSynthProcessor& p, const juce::String& id)
{
    auto& parameter = param (p, id);
    return parameter.convertFrom0to1 (parameter.getValue());
}

/** A sung phrase: three notes (A3, C4, E4) with harmonics, a light vibrato and gaps, then 1 s of silence. */
std::vector<float> sungPhrase (double sr, double amplitude = 0.3)
{
    const double notes[] = { 220.0, 261.63, 329.63 };
    const auto noteSamples = static_cast<int> (0.45 * sr);
    const auto gapSamples = static_cast<int> (0.12 * sr);
    std::vector<float> out;
    double phase = 0.0;

    for (const auto f0 : notes)
    {
        for (int i = 0; i < noteSamples; ++i)
        {
            const auto t = i / sr;
            const auto f = f0 * std::pow (2.0, 0.15 / 12.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.0 * t));
            phase += 2.0 * juce::MathConstants<double>::pi * f / sr;
            const auto env = std::min (1.0, std::min (t / 0.02, (noteSamples - i) / sr / 0.02));
            out.push_back (static_cast<float> (amplitude * env
                                               * (std::sin (phase) + 0.4 * std::sin (2.0 * phase) + 0.2 * std::sin (3.0 * phase))
                                               / 1.6));
        }

        out.insert (out.end(), static_cast<std::size_t> (gapSamples), 0.0f);
    }

    out.insert (out.end(), static_cast<std::size_t> (sr), 0.0f);
    return out;
}

struct RunResult
{
    std::vector<float> left, right;
    int noteOns = 0;
    int noteOffs = 0;
};

/** Processes mono input (copied to every input channel) in host blocks of blockSize; collects the output and
    counts the MIDI notes. */
RunResult run (VocalSynthProcessor& p, const std::vector<float>& input, int blockSize)
{
    const auto numChannels = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buffer (numChannels, blockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (8192);
    RunResult r;
    r.left.reserve (input.size());
    r.right.reserve (input.size());

    for (std::size_t pos = 0; pos < input.size(); pos += static_cast<std::size_t> (blockSize))
    {
        const auto n = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (blockSize), input.size() - pos));
        buffer.setSize (numChannels, n, false, false, true);

        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < n; ++i)
                buffer.setSample (ch, i, ch < p.getTotalNumInputChannels() ? input[pos + static_cast<std::size_t> (i)] : 0.0f);

        midi.clear();
        p.processBlock (buffer, midi);

        for (const auto m : midi)
        {
            r.noteOns += m.getMessage().isNoteOn() ? 1 : 0;
            r.noteOffs += m.getMessage().isNoteOff() ? 1 : 0;
        }

        for (int i = 0; i < n; ++i)
        {
            r.left.push_back (buffer.getSample (0, i));
            r.right.push_back (buffer.getSample (juce::jmin (1, buffer.getNumChannels() - 1), i));
        }
    }

    return r;
}

double rms (const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX)
{
    to = std::min (to, x.size());
    double sum = 0.0;

    for (auto i = from; i < to; ++i)
        sum += static_cast<double> (x[i]) * x[i];

    return to > from ? std::sqrt (sum / static_cast<double> (to - from)) : 0.0;
}

std::vector<float> difference (const std::vector<float>& a, const std::vector<float>& b)
{
    std::vector<float> d (a.size());

    for (std::size_t i = 0; i < a.size(); ++i)
        d[i] = a[i] - b[i];

    return d;
}

bool bitIdentical (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}

int factoryIndexOf (PresetManager& presets, const juce::String& name)
{
    const auto list = presets.getPresets();

    for (const auto& preset : list)
        if (preset.isFactory && preset.name == name)
            return preset.factoryIndex;

    FAIL ("no factory preset " << name);
    return -1;
}

juce::ValueTree stateTree (VocalSynthProcessor& p)
{
    juce::MemoryBlock block;
    p.getStateInformation (block);
    const auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), static_cast<int> (block.getSize()));
    REQUIRE (xml != nullptr);
    return juce::ValueTree::fromXml (*xml);
}

struct ProgramChangeCounter final : juce::AudioProcessorListener
{
    int programChanges = 0;

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
    {
        programChanges += details.programChanged ? 1 : 0;
    }
};
} // namespace

//==============================================================================
TEST_CASE ("SynthProcessor: a new instance plays the synth with the default tables and Init", "[plugin][synth][processor]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    VocalSynthProcessor p (variant);

    CHECK (getPlain (p, params::synthOn.getParamID()) == 1.0f);          // decision E4 / E5
    CHECK (param (p, params::synthOn.getParamID()).getDefaultValue() == 1.0f); // = the initial value (clap-validator)
    CHECK (p.getPresetManager().getCurrentName() == "Init");
    CHECK_FALSE (p.getPresetManager().isModified());
    CHECK (p.getEditorScalePercent() == 100);

    // Both tables are published before the first block (never the fallback sine)
    for (int s = 0; s < WavetableStore::numSlots; ++s)
    {
        CHECK (p.getWavetableStore().acquire (s) != nullptr);
        CHECK (p.getWavetableWorker().getRequestedReference (s).reference.toRefString() == "factory:basic-shapes");
    }

    // Acceptance: reported latency 0, no tail; the plugin takes no MIDI input (the Instrument plays the sidechain mic)
    CHECK (p.getLatencySamples() == 0);
    CHECK (p.getTailLengthSeconds() == 0.0);
    CHECK_FALSE (p.acceptsMidi());
    CHECK (p.producesMidi());
}

TEST_CASE ("SynthProcessor: the sung voice makes MIDI and synth audio in FX and Instrument", "[plugin][synth][processor]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    const auto synthOn = GENERATE (true, false);
    CAPTURE (variant == Variant::fx ? "fx" : "instrument", synthOn);

    const double sr = 48000.0;
    VocalSynthProcessor p (variant);
    p.prepareToPlay (sr, 256);
    setPlain (p, params::synthOn.getParamID(), synthOn ? 1.0f : 0.0f);

    const auto input = sungPhrase (sr);
    const auto out = run (p, input, 256);

    // The MIDI path is unchanged: three sung notes
    CHECK (out.noteOns >= 3);
    CHECK (out.noteOffs == out.noteOns);

    const auto synthLeft = variant == Variant::fx ? difference (out.left, input) : out.left;
    const auto synthRight = variant == Variant::fx ? difference (out.right, input) : out.right;
    const auto phraseEnd = input.size() - static_cast<std::size_t> (sr);

    if (synthOn)
    {
        // Audible without any routing, in both variants, stereo
        CHECK (rms (synthLeft, 0, phraseEnd) > 0.01);
        CHECK (rms (synthRight, 0, phraseEnd) > 0.01);

        // Nothing hangs: the last second of silence ends in exact 0 after the release
        const auto tail = input.size() - static_cast<std::size_t> (0.2 * sr);
        CHECK (rms (synthLeft, tail) == 0.0);
        CHECK (rms (synthRight, tail) == 0.0);
    }
    else
    {
        // Synth off: FX = the dry voice bit for bit, Instrument = silence (monitor tone off by default)
        if (variant == Variant::fx)
        {
            CHECK (bitIdentical (out.left, input));
            CHECK (bitIdentical (out.right, input));
        }
        else
        {
            CHECK (rms (out.left) == 0.0);
            CHECK (rms (out.right) == 0.0);
        }
    }

    p.releaseResources();
}

TEST_CASE ("SynthProcessor: the voice expression reaches the meters", "[plugin][synth][processor]")
{
    const double sr = 48000.0;
    VocalSynthProcessor p (Variant::fx);
    p.prepareToPlay (sr, 512);

    const auto input = sungPhrase (sr);
    const std::vector<float> phrase (input.begin(), input.begin() + static_cast<std::ptrdiff_t> (0.3 * sr));
    (void) run (p, phrase, 512);

    const auto e = p.getExpressionPublisher().read();
    CHECK (e.level > 0.1f);
    CHECK (e.brightness > 0.0f);

    // Bypass publishes the rest values
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    buffer.clear();
    p.processBlockBypassed (buffer, midi);
    CHECK (p.getExpressionPublisher().read().level == 0.0f);
}

TEST_CASE ("SynthProcessor: dry level, Mute Dry and the 0 dB bit-identical path", "[plugin][synth][processor]")
{
    const double sr = 48000.0;

    // Low-level input below the gate (no notes), with negative zeros: synth on but silent -> bit-identical
    std::vector<float> quiet (static_cast<std::size_t> (sr * 0.5));
    for (std::size_t i = 0; i < quiet.size(); ++i)
        quiet[i] = i % 7 == 0 ? -0.0f : 1.0e-4f * static_cast<float> (std::sin (0.01 * static_cast<double> (i)));

    {
        VocalSynthProcessor p (Variant::fx);
        p.prepareToPlay (sr, 480);
        REQUIRE (getPlain (p, params::synthOn.getParamID()) == 1.0f);
        const auto out = run (p, quiet, 480);
        CHECK (out.noteOns == 0);
        CHECK (bitIdentical (out.left, quiet));
        CHECK (bitIdentical (out.right, quiet));
    }

    // dryLevel: -6 dB scales the dry voice (after the 20 ms ramp), -60 dB is silent; changes ramp
    std::vector<float> tone (static_cast<std::size_t> (sr * 0.3));
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.001f * static_cast<float> (std::sin (0.05 * static_cast<double> (i))); // below the gate

    {
        VocalSynthProcessor p (Variant::fx);
        p.prepareToPlay (sr, 512);
        setPlain (p, params::synthOn.getParamID(), 0.0f);
        setPlain (p, params::dryLevel.getParamID(), -6.0f);
        auto out = run (p, tone, 512);
        const auto gain = juce::Decibels::decibelsToGain (-6.0f);

        // The first block after prepare jumps to the target (no ramp from 0 dB)
        for (std::size_t i = 0; i < tone.size(); i += 97)
            CHECK (out.left[i] == Catch::Approx (tone[i] * gain).margin (1e-9));

        // A change ramps linearly over 20 ms (960 samples): no jump at the change
        setPlain (p, params::dryLevel.getParamID(), 0.0f);
        out = run (p, tone, 512);
        CHECK (out.left[0] == Catch::Approx (tone[0] * gain).margin (1e-6));
        CHECK (std::abs (out.left[480] / tone[480] - 0.5f * (gain + 1.0f)) < 0.01f);
        CHECK (bitIdentical (std::vector<float> (out.left.begin() + 1024, out.left.end()),
                             std::vector<float> (tone.begin() + 1024, tone.end()))); // back at 0 dB: untouched

        setPlain (p, params::dryLevel.getParamID(), -60.0f);
        out = run (p, tone, 512);
        CHECK (rms (out.left, 1024) == 0.0);

        // Mute Dry: silence at once (as in 0.3.1)
        setPlain (p, params::dryLevel.getParamID(), 0.0f);
        setPlain (p, params::muteDry.getParamID(), 1.0f);
        out = run (p, tone, 512);
        CHECK (rms (out.left) == 0.0);
        CHECK (rms (out.right) == 0.0);
    }

    // Instrument: the dry sidechain never reaches the output, whatever dryLevel / muteDry say
    {
        VocalSynthProcessor p (Variant::instrument);
        p.prepareToPlay (sr, 512);
        setPlain (p, params::synthOn.getParamID(), 0.0f);
        setPlain (p, params::dryLevel.getParamID(), 0.0f);
        const auto out = run (p, tone, 512);
        CHECK (rms (out.left) == 0.0);
    }

    CHECK (DryGain::gainForDb (0.0f) == 1.0f);
    CHECK (DryGain::gainForDb (-60.0f) == 0.0f);
    CHECK (DryGain::gainForDb (-6.0f) == Catch::Approx (0.501187f));
}

TEST_CASE ("SynthProcessor: processor output is bit-identical for every host block size", "[plugin][synth][processor][invariance]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);
    const auto input = sungPhrase (sr);

    const auto render = [&] (int blockSize)
    {
        VocalSynthProcessor p (Variant::fx);
        p.getPresetManager().setUserFolder (juce::File()); // no user presets
        REQUIRE (p.getPresetManager().loadFactory (factoryIndexOf (p.getPresetManager(), "Warm Pad"))); // Poly + FX
        setPlain (p, params::dryLevel.getParamID(), -6.0f);
        setPlain (p, params::glideTime.getParamID(), 40.0f);
        p.prepareToPlay (sr, blockSize);
        auto r = run (p, input, blockSize);
        p.releaseResources();
        return r;
    };

    const auto reference = render (512);
    REQUIRE (reference.noteOns >= 3);
    REQUIRE (rms (difference (reference.left, input)) > 0.005);

    for (const auto blockSize : { 1, 7, 64, 480, 4096 })
    {
        CAPTURE (blockSize);
        const auto r = render (blockSize);
        CHECK (r.noteOns == reference.noteOns);
        CHECK (bitIdentical (r.left, reference.left));
        CHECK (bitIdentical (r.right, reference.right));
    }
}

TEST_CASE ("SynthProcessor: no allocation in processBlock with the synth playing", "[plugin][synth][processor]")
{
#if defined(VOCALSYNTH_COUNT_MALLOC)
    const double sr = 48000.0;
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    VocalSynthProcessor p (variant);
    p.getPresetManager().setUserFolder (juce::File());
    REQUIRE (p.getPresetManager().loadFactory (factoryIndexOf (p.getPresetManager(), "Warm Pad")));
    setPlain (p, params::oscParameters[0].unison.getParamID(), 8.0f);
    setPlain (p, params::delayOn.getParamID(), 1.0f);
    p.prepareToPlay (sr, 4096);

    const auto input = sungPhrase (sr);
    juce::AudioBuffer<float> buffer (2, 4096);
    juce::MidiBuffer midi;
    midi.ensureSize (16384);
    std::size_t allocations = 0;
    int noteOns = 0;

    for (const auto blockSize : { 128, 4096, 7 })
    {
        CAPTURE (blockSize);

        for (std::size_t pos = 0; pos + static_cast<std::size_t> (blockSize) <= input.size() / 2; pos += static_cast<std::size_t> (blockSize))
        {
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, blockSize);

            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::copy (view.getWritePointer (ch), input.data() + pos, blockSize);

            midi.clear();

            {
                CountMallocs counter;
                p.processBlock (view, midi);
                allocations += counter.count();
            }

            for (const auto m : midi)
                noteOns += m.getMessage().isNoteOn() ? 1 : 0;

            if (pos == 48000)
            {
                CountMallocs counter;
                p.processBlockBypassed (view, midi);
                allocations += counter.count();
            }
        }
    }

    CHECK (noteOns >= 3);
    CHECK (allocations == 0);
    p.releaseResources();

    // The counter itself works (otherwise the check above would pass vacuously)
    std::vector<float>* volatile probe = nullptr; // volatile: the allocation cannot be elided
    std::size_t counted = 0;

    {
        CountMallocs counter;
        probe = new std::vector<float> (100);
        counted = counter.count();
    }

    delete probe;
    CHECK (counted >= 2);
#else
    SKIP ("allocation counting needs glibc's malloc (not available on this platform / under sanitizers)");
#endif
}

TEST_CASE ("SynthProcessor: nothing hangs on bypass, flush and source change", "[plugin][synth][processor]")
{
    const double sr = 48000.0;
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    VocalSynthProcessor p (variant);
    p.prepareToPlay (sr, 512);

    const auto input = sungPhrase (sr);
    const std::vector<float> held (input.begin(), input.begin() + static_cast<std::ptrdiff_t> (0.3 * sr)); // inside note 1
    const std::vector<float> silence (static_cast<std::size_t> (sr * 0.8), 0.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    // 1. Bypass while a note sounds: bypassed blocks are dry / silent at once, afterwards silence stays silent
    (void) run (p, held, 512);
    buffer.clear();
    p.processBlockBypassed (buffer, midi);
    CHECK (buffer.getMagnitude (0, 512) == 0.0f);
    auto out = run (p, silence, 512);
    CHECK (rms (out.left) == 0.0);

    // 2. Restart pattern (flush) while a note sounds: the voices release, the tail ends in exact 0
    (void) run (p, held, 512);
    p.requestFlush();
    out = run (p, silence, 512);
    CHECK (out.noteOffs >= 1);
    CHECK (rms (out.left, out.left.size() - 9600) == 0.0);

    // 3. Source change while a note sounds: the same
    (void) run (p, held, 512);
    setPlain (p, params::source.getParamID(), static_cast<float> (params::Source::testPattern)); // pattern Off
    out = run (p, silence, 512);
    CHECK (out.noteOffs >= 1);
    CHECK (rms (out.left, out.left.size() - 9600) == 0.0);

    // 4. releaseResources / prepareToPlay (buffer size change during playback)
    setPlain (p, params::source.getParamID(), static_cast<float> (params::Source::voice));
    (void) run (p, held, 512);
    p.releaseResources();
    p.prepareToPlay (sr, 128);
    out = run (p, silence, 128);
    CHECK (rms (out.left) == 0.0);
    p.releaseResources();
}

TEST_CASE ("SynthProcessor: Test Pattern plays the synth with the pattern's bend range", "[plugin][synth][processor]")
{
    const double sr = 44100.0;
    VocalSynthProcessor p (Variant::instrument);
    p.prepareToPlay (sr, 512);
    setPlain (p, params::source.getParamID(), static_cast<float> (params::Source::testPattern));
    setPlain (p, params::testPattern.getParamID(), static_cast<float> (TestPattern::all));
    setPlain (p, params::bendRange.getParamID(), 3.0f); // +-48 in Voice mode only

    CHECK (p.getSynthSettings().bendRangeSemitones == 2);
    const auto out = run (p, std::vector<float> (static_cast<std::size_t> (sr * 2.0), 0.0f), 512);
    CHECK (out.noteOns >= 1);
    CHECK (rms (out.left) > 0.01); // the Instrument plays the synth from the pattern (no monitor tone)

    setPlain (p, params::source.getParamID(), static_cast<float> (params::Source::voice));
    CHECK (p.getSynthSettings().bendRangeSemitones == 48);
    p.releaseResources();
}

TEST_CASE ("SynthProcessor: transport for the LFO / delay sync follows the host time signature", "[plugin][synth][processor]")
{
    TransportInfo info;
    CHECK (synthTransportFor (info).bpm == 120.0);
    CHECK (synthTransportFor (info).quarterNotesPerBar == 4.0);

    info.hasBpm = true;
    info.bpm = 93.5;
    info.hasTimeSignature = true;
    info.timeSigNumerator = 6;
    info.timeSigDenominator = 8;
    CHECK (synthTransportFor (info).bpm == 93.5);
    CHECK (synthTransportFor (info).quarterNotesPerBar == 3.0);

    info.timeSigNumerator = 7;
    info.timeSigDenominator = 4;
    CHECK (synthTransportFor (info).quarterNotesPerBar == 7.0);

    info.bpm = 0.0;
    info.timeSigDenominator = 0;
    CHECK (synthTransportFor (info).bpm == 120.0);
    CHECK (synthTransportFor (info).quarterNotesPerBar == 4.0);
}

TEST_CASE ("SynthProcessor: state round trip with an embedded user table, preset name and editor scale", "[plugin][synth][processor][state]")
{
    testdata::ScopedTempFolder temp;
    const auto userData = testdata::makeUserData (8, 3, juce::String::fromUTF8 ("Meine Tabelle \xc3\xa4.wav"));

    VocalSynthProcessor a (Variant::fx);
    a.getPresetManager().setUserFolder (temp.folder);
    REQUIRE (a.getPresetManager().loadFactory (factoryIndexOf (a.getPresetManager(), "Voice Lead")));
    setPlain (a, params::filterCutoff.getParamID(), 3333.0f);                  // modified
    setPlain (a, params::voiceRange.getParamID(), 1.0f);                       // a voice parameter
    setPlain (a, params::dryLevel.getParamID(), -12.0f);
    juce::String error;
    REQUIRE (a.getWavetableWorker().loadEmbeddedNow (1, userData, error));
    REQUIRE (a.getWavetableWorker().loadFactoryNow (0, "formant-vowel", error));
    a.setEditorScalePercent (150);
    CHECK (a.getPresetManager().isModified());

    juce::MemoryBlock blob;
    a.getStateInformation (blob);
    const auto saved = stateTree (a);
    CHECK (SynthState::getStateVersion (saved) == 4);
    CHECK (saved.getChildWithName (SynthState::treeType).isValid());

    VocalSynthProcessor b (Variant::fx);
    b.setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));

    // Tables are published when setStateInformation returns (an offline render could start at once)
    const auto slot0 = b.getWavetableStore().getSlot (0);
    const auto slot1 = b.getWavetableStore().getSlot (1);
    CHECK (slot0.reference.toRefString() == "factory:formant-vowel");
    CHECK (slot1.reference.toRefString() == "user:" + userData->hash);
    REQUIRE (slot1.userData != nullptr);
    CHECK (slot1.userData->pcm == userData->pcm);
    CHECK (slot1.userData->name == userData->name);
    CHECK (b.getWavetableStore().acquire (0) == slot0.table.get());
    CHECK (b.getWavetableStore().acquire (1) == slot1.table.get());

    CHECK (b.getPresetManager().getCurrentName() == "Voice Lead");
    CHECK (b.getPresetManager().isModified());
    CHECK (b.getEditorScalePercent() == 150);
    CHECK (getPlain (b, params::filterCutoff.getParamID()) == Catch::Approx (3333.0f).epsilon (1e-4));
    CHECK (getPlain (b, params::voiceRange.getParamID()) == 1.0f);
    CHECK (getPlain (b, params::dryLevel.getParamID()) == Catch::Approx (-12.0f));
    CHECK (getPlain (b, params::synthOn.getParamID()) == 1.0f);

    // getStateInformation right after setStateInformation (clap-validator, FL undo / autosave) gives the same state
    CHECK (stateTree (b).isEquivalentTo (saved));

    // Loading the same state twice keeps one SYNTH / CAPTURE child
    b.setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));
    const auto again = stateTree (b);
    int synthChildren = 0, captureChildren = 0;

    for (const auto& child : again)
    {
        synthChildren += child.hasType (SynthState::treeType) ? 1 : 0;
        captureChildren += child.hasType (CaptureState::treeType) ? 1 : 0;
    }

    CHECK (synthChildren == 1);
    CHECK (captureChildren == 1);
    CHECK (again.isEquivalentTo (saved));
}

TEST_CASE ("SynthProcessor: a preset load sets no voice, mix or capture parameter", "[plugin][synth][processor]")
{
    VocalSynthProcessor p (Variant::fx);
    p.getPresetManager().setUserFolder (juce::File());
    setPlain (p, params::voiceRange.getParamID(), 0.0f);
    setPlain (p, params::bendRange.getParamID(), 2.0f);
    setPlain (p, params::synthOn.getParamID(), 0.0f);
    setPlain (p, params::synthVolume.getParamID(), -20.0f);
    setPlain (p, params::dryLevel.getParamID(), -9.0f);
    setPlain (p, params::captureEnabled.getParamID(), 0.0f);

    for (const auto* name : { "Warm Pad", "Acid Bass", "Init" })
    {
        CAPTURE (name);
        REQUIRE (p.getPresetManager().loadFactory (factoryIndexOf (p.getPresetManager(), name)));
        CHECK (p.getPresetManager().getCurrentName() == name);
        CHECK (getPlain (p, params::voiceRange.getParamID()) == 0.0f);
        CHECK (getPlain (p, params::bendRange.getParamID()) == 2.0f);
        CHECK (getPlain (p, params::synthOn.getParamID()) == 0.0f);
        CHECK (getPlain (p, params::synthVolume.getParamID()) == Catch::Approx (-20.0f));
        CHECK (getPlain (p, params::dryLevel.getParamID()) == Catch::Approx (-9.0f));
        CHECK (getPlain (p, params::captureEnabled.getParamID()) == 0.0f);
    }
}

TEST_CASE ("SynthProcessor: a preset load from the editor tells the host the program changed (CLAP)", "[plugin][synth][processor][editor]")
{
    VocalSynthProcessor p (Variant::fx);
    p.getPresetManager().setUserFolder (juce::File());
    ProgramChangeCounter listener;
    p.addListener (&listener);

    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        auto* vsEditor = dynamic_cast<VocalSynthEditor*> (editor.get());
        REQUIRE (vsEditor != nullptr);
        vsEditor->showPage (VocalSynthEditor::Page::synth);

        const auto before = listener.programChanges;
        vsEditor->getSynthPanel().getPresetBar().pressNext();
        CHECK (p.getPresetManager().getCurrentName() != "Init");
        CHECK (listener.programChanges == before + 1);

        vsEditor->getSynthPanel().getPresetBar().pressPrevious();
        CHECK (p.getPresetManager().getCurrentName() == "Init");
        CHECK (listener.programChanges == before + 2);
    }

    // A state load does it too (as in 0.3.1)
    const auto before = listener.programChanges;
    juce::MemoryBlock blob;
    p.getStateInformation (blob);
    p.setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));
    CHECK (listener.programChanges == before + 1);
    p.removeListener (&listener);
}

TEST_CASE ("SynthProcessor: a 0.3.1 project loads with the synth off and sounds as before", "[plugin][synth][processor][fixtures]")
{
    const auto [variant, name] = GENERATE (std::pair { Variant::fx, juce::String ("state-0.3.1-fx") },
                                           std::pair { Variant::instrument, juce::String ("state-0.3.1-instrument") });
    CAPTURE (name);

    juce::MemoryBlock blob;
    REQUIRE (dataFile (name + ".bin").loadFileAsData (blob));

    const double sr = 48000.0;
    VocalSynthProcessor p (variant);
    p.prepareToPlay (sr, 512);
    p.setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));
    CHECK (getPlain (p, params::synthOn.getParamID()) == 0.0f);
    CHECK (p.getPresetManager().getCurrentName() == "Init");

    // Only the old audio path: with the monitor tone off, the output is the dry voice (or silence when muted /
    // Instrument) bit for bit while the voice still makes MIDI
    setPlain (p, params::monitorTone.getParamID(), 0.0f);
    setPlain (p, params::source.getParamID(), static_cast<float> (params::Source::voice));
    const auto muted = getPlain (p, params::muteDry.getParamID()) >= 0.5f || variant == Variant::instrument;
    const auto input = sungPhrase (sr);
    const auto out = run (p, input, 512);
    CHECK (out.noteOns >= 3);

    if (muted)
        CHECK (rms (out.left) == 0.0);
    else
        CHECK (bitIdentical (out.left, input));

    p.releaseResources();
}

//==============================================================================
// The 0.4.0 state fixtures (docs/SYNTH-DESIGN.md 13.3): getStateInformation() of the 0.4.0 code with non-default
// voice, synth and mix parameters, a modified factory preset, an embedded user table (FX: osc B) and an editor
// scale. Written once by the hidden test below (run it with "[.writefixture]" and review the diff), read by every
// later release.

namespace
{
struct FixtureSpec
{
    Variant variant;
    juce::String name;
};

void configureFixture (VocalSynthProcessor& p, Variant variant)
{
    p.getPresetManager().setUserFolder (juce::File());
    juce::String error;

    if (variant == Variant::fx)
    {
        REQUIRE (p.getPresetManager().loadFactory (factoryIndexOf (p.getPresetManager(), "Warm Pad")));
        setPlain (p, params::filterCutoff.getParamID(), 2750.0f); // modified preset
        setPlain (p, params::voiceRange.getParamID(), 1.0f);
        setPlain (p, params::bendRange.getParamID(), 1.0f);
        setPlain (p, params::minNoteLength.getParamID(), 70.0f);
        setPlain (p, params::synthVolume.getParamID(), -8.5f);
        setPlain (p, params::dryLevel.getParamID(), -3.0f);
        REQUIRE (p.getWavetableWorker().loadFactoryNow (0, "formant-vowel", error));
        REQUIRE (p.getWavetableWorker().loadEmbeddedNow (1, testdata::makeUserData (4, 11, juce::String::fromUTF8 ("Eigene Tabelle \xc3\xb6.wav")), error));
        p.setEditorScalePercent (125);
    }
    else
    {
        REQUIRE (p.getPresetManager().loadFactory (factoryIndexOf (p.getPresetManager(), "Acid Bass")));
        setPlain (p, params::synthOn.getParamID(), 0.0f); // saved off stays off
        setPlain (p, params::voiceRange.getParamID(), 4.0f);
        setPlain (p, params::key.getParamID(), 9.0f);
        setPlain (p, params::scale.getParamID(), 2.0f);
        REQUIRE (p.getWavetableWorker().loadFactoryNow (0, "fm-2op", error));
        p.setEditorScalePercent (150);
    }
}
} // namespace

TEST_CASE ("SynthProcessor: write the 0.4.0 state fixtures", "[.writefixture]")
{
    for (const auto& spec : { FixtureSpec { Variant::fx, "state-0.4.0-fx" }, FixtureSpec { Variant::instrument, "state-0.4.0-instrument" } })
    {
        VocalSynthProcessor p (spec.variant);
        configureFixture (p, spec.variant);

        juce::MemoryBlock blob;
        p.getStateInformation (blob);
        REQUIRE (dataFile (spec.name + ".bin").replaceWithData (blob.getData(), blob.getSize()));

        const auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), static_cast<int> (blob.getSize()));
        REQUIRE (xml != nullptr);
        REQUIRE (xml->writeTo (dataFile (spec.name + ".xml")));
    }
}

TEST_CASE ("0.4.0 state fixtures load with every value, the tables and the synth state", "[plugin][state][fixtures]")
{
    const auto [variant, name] = GENERATE (std::pair { Variant::fx, juce::String ("state-0.4.0-fx") },
                                           std::pair { Variant::instrument, juce::String ("state-0.4.0-instrument") });
    CAPTURE (name);

    juce::MemoryBlock blob;
    REQUIRE (dataFile (name + ".bin").loadFileAsData (blob));
    const auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), static_cast<int> (blob.getSize()));
    REQUIRE (xml != nullptr);
    const auto saved = juce::ValueTree::fromXml (*xml);
    CHECK (SynthState::getStateVersion (saved) == 4);

    // The readable copy is the same state
    const auto text = juce::parseXML (dataFile (name + ".xml"));
    REQUIRE (text != nullptr);
    CHECK (juce::ValueTree::fromXml (*text).isEquivalentTo (saved));

    const auto synthState = SynthState::fromValueTree (saved.getChildWithName (SynthState::treeType));

    VocalSynthProcessor p (variant);
    p.getPresetManager().setUserFolder (juce::File());
    p.setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));

    for (const auto& child : saved)
    {
        if (! child.hasType ("PARAM"))
            continue;

        const auto id = child.getProperty ("id").toString();
        CAPTURE (id);
        CHECK (getPlain (p, id) == Catch::Approx (static_cast<float> (static_cast<double> (child.getProperty ("value")))));
    }

    CHECK (p.getPresetManager().getCurrentName() == synthState.presetName);
    CHECK (p.getPresetManager().isModified() == synthState.presetModified);
    CHECK (p.getEditorScalePercent() == synthState.editorScalePercent);

    for (int s = 0; s < WavetableStore::numSlots; ++s)
    {
        CAPTURE (s);
        const auto& table = synthState.tables[static_cast<std::size_t> (s)];
        const auto slot = p.getWavetableStore().getSlot (s);
        CHECK (slot.reference.toRefString() == table.reference.toRefString());
        CHECK (slot.table != nullptr);

        if (table.reference.kind == WavetableReference::Kind::user)
        {
            REQUIRE (slot.userData != nullptr);
            REQUIRE (table.embedded != nullptr);
            CHECK (slot.userData->pcm == table.embedded->pcm);
        }
    }

    if (variant == Variant::fx)
    {
        CHECK (synthState.presetName == "Warm Pad");
        CHECK (synthState.presetModified);
        CHECK (synthState.tables[1].reference.kind == WavetableReference::Kind::user);
        CHECK (getPlain (p, params::synthOn.getParamID()) == 1.0f);
    }
    else
    {
        CHECK (synthState.presetName == "Acid Bass");
        CHECK (getPlain (p, params::synthOn.getParamID()) == 0.0f);
    }

    // Saving again gives the same state (exactly one SYNTH child, the user table data once)
    CHECK (stateTree (p).isEquivalentTo (saved));
}

//==============================================================================
// Processor-level CPU (docs/SYNTH-DESIGN.md 18): whole processBlock (voice analysis + synth + capture) at 48 kHz,
// block 128, Release. Hidden; run with "[.cpu]" and record the numbers in the design document.
TEST_CASE ("SynthProcessor: processBlock CPU at 48 kHz / 128", "[.cpu]")
{
    const double sr = 48000.0;
    const int block = 128;
    const auto input = sungPhrase (sr);

    struct Case
    {
        const char* name;
        std::function<void (VocalSynthProcessor&)> setup;
    };

    const auto loadPreset = [] (const char* preset)
    {
        return [preset] (VocalSynthProcessor& p)
        {
            p.getPresetManager().setUserFolder (juce::File());
            REQUIRE (p.getPresetManager().loadFactory (factoryIndexOf (p.getPresetManager(), preset)));
        };
    };

    const Case cases[] = {
        { "synth off (0.3.1 path)", [] (VocalSynthProcessor& p) { setPlain (p, params::synthOn.getParamID(), 0.0f); } },
        { "Init", loadPreset ("Init") },
        { "Voice Lead", loadPreset ("Voice Lead") },
        { "Supersaw Lead", loadPreset ("Supersaw Lead") },
        { "Warm Pad", loadPreset ("Warm Pad") },
        { "Choir Pad", loadPreset ("Choir Pad") },
        { "typical (Legato 2x4 LP24 chorus delay reverb)",
          [] (VocalSynthProcessor& p)
          {
              for (const auto& osc : params::oscParameters)
              {
                  setPlain (p, osc.on.getParamID(), 1.0f);
                  setPlain (p, osc.unison.getParamID(), 4.0f);
              }
              setPlain (p, params::chorusOn.getParamID(), 1.0f);
              setPlain (p, params::delayOn.getParamID(), 1.0f);
              setPlain (p, params::reverbOn.getParamID(), 1.0f);
          } },
        { "worst (Poly 2x8 unison sub noise drive all FX, long release)",
          [] (VocalSynthProcessor& p)
          {
              setPlain (p, params::playMode.getParamID(), 2.0f);
              for (const auto& osc : params::oscParameters)
              {
                  setPlain (p, osc.on.getParamID(), 1.0f);
                  setPlain (p, osc.unison.getParamID(), 8.0f);
              }
              setPlain (p, params::subLevel.getParamID(), 50.0f);
              setPlain (p, params::noiseLevel.getParamID(), 20.0f);
              setPlain (p, params::filterDrive.getParamID(), 12.0f);
              setPlain (p, params::envelopeParameters[0].release.getParamID(), 4000.0f);
              setPlain (p, params::chorusOn.getParamID(), 1.0f);
              setPlain (p, params::delayOn.getParamID(), 1.0f);
              setPlain (p, params::reverbOn.getParamID(), 1.0f);
          } },
    };

    for (const auto& c : cases)
    {
        VocalSynthProcessor p (Variant::fx);
        c.setup (p);
        p.prepareToPlay (sr, block);
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        midi.ensureSize (4096);
        std::vector<double> times;

        for (int rep = 0; rep < 3; ++rep)
            for (std::size_t pos = 0; pos + block <= input.size(); pos += block)
            {
                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::copy (buffer.getWritePointer (ch), input.data() + pos, block);

                midi.clear();
                const auto t0 = std::chrono::steady_clock::now();
                p.processBlock (buffer, midi);
                times.push_back (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
            }

        const auto blockSeconds = block / sr;
        double sum = 0.0;
        for (const auto t : times)
            sum += t;

        std::sort (times.begin(), times.end());
        const auto mean = sum / static_cast<double> (times.size()) / blockSeconds * 100.0;
        const auto p99 = times[times.size() * 99 / 100] / blockSeconds * 100.0;
        const auto synth = p.getSynthDspLoad();
        std::printf ("CPU %-60s mean %5.2f %%  p99 %5.2f %%  max %6.2f %%  (synth engine avg %5.2f %%)\n", c.name, mean, p99,
                     times.back() / blockSeconds * 100.0, 100.0 * synth.averageProportion);
        p.releaseResources();
    }

    SUCCEED();
}
