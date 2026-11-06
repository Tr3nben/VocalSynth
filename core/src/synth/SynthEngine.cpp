#include "vocalsynth/synth/SynthEngine.h"

#include "vocalsynth/ExpressionTracker.h"
#include "vocalsynth/synth/WavetableBuilder.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth::synth
{

// The 0.4.0 synth engine (docs/SYNTH-DESIGN.md 2, 3, 10, 11, 16.4). process() walks the chunk from one
// control position to the next: at every event position the bends and the note group (VoiceAllocator ->
// voice commands), at every absolute multiple of controlTickSamples the tick (3.2), and in between the
// per-sample DSP (voices -> sum -> Chorus -> StereoDelay -> FdnReverb -> synthVolume / synthOn fade).
// Every state change sits on an absolute position, every ramp is incremental and carried in members, so
// the output is bit-identical for any split of the input into process() calls (3.5).
//
// Wavetables (12.1): setWavetable() only records the pointer. At a tick the engine hands it to every voice
// - but only while no voice's oscillator is in a table crossfade. All voices therefore share one table
// timeline, getTablesInUse() is the union of {current, previous} over the voices and never holds more than
// two pointers per oscillator (a change during a crossfade waits for its end: "queued, newest wins").

namespace
{
constexpr float snapRelative = 1.0e-5f;
constexpr float snapLog2 = 1.0e-6f;
constexpr int maxFrameDistance = 4096;

inline bool isNoteEvent (const VsEvent& e) noexcept
{
    return e.type == VsEventType::noteOn || e.type == VsEventType::noteOff;
}

/** One-pole step towards t with a snap once it is close (smoothing ends exactly, no endless tiny steps). */
inline float smoothLinear (float v, float t, float c) noexcept
{
    if (v == t)
        return t;

    const auto next = v + c * (t - v);
    return std::abs (t - next) <= snapRelative * std::max (1.0f, std::abs (t)) ? t : next;
}

/** The same in log2 (cutoff, LFO / chorus rates, delay time; values > 0). */
inline float smoothLog (float v, float t, float c) noexcept
{
    if (v == t)
        return t;

    if (! (v > 0.0f) || ! (t > 0.0f))
        return t;

    const auto lv = std::log2 (v), lt = std::log2 (t);
    const auto next = lv + c * (lt - lv);
    return std::abs (lt - next) <= snapLog2 ? t : std::exp2 (next);
}

inline float decibelsToGain (float db) noexcept
{
    return db <= limits::synthVolumeDb.min || ! std::isfinite (db) ? 0.0f : std::pow (10.0f, db / 20.0f);
}
} // namespace

SynthEngine::SynthEngine() = default;
SynthEngine::~SynthEngine() = default;

void SynthEngine::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;

    if (fallbackSine == nullptr)
        fallbackSine = WavetableBuilder::makeSineTable();

    allocator.prepare (sampleRate);

    for (std::size_t i = 0; i < voices.size(); ++i)
        voices[i].prepare (sampleRate, 0x9e3779b9u ^ static_cast<uint32_t> (i + 1));

    for (std::size_t i = 0; i < lfos.size(); ++i)
        lfos[i].prepare (sampleRate, 0x85ebca6bu ^ static_cast<uint32_t> (i + 1));

    chorus.prepare (sampleRate);
    delay.prepare (sampleRate);
    reverb.prepare (sampleRate);

    const auto tick = static_cast<double> (controlTickSamples);
    smoothingCoefficient = static_cast<float> (1.0 - std::exp (-tick / (settingsSmoothingSeconds * sampleRate)));
    bendCoefficient = static_cast<float> (1.0 - std::exp (-tick / (bendSmoothingSeconds * sampleRate)));
    restAttackCoefficient = static_cast<float> (1.0 - std::exp (-tick / (ExpressionTracker::levelAttackSeconds * sampleRate)));
    restReleaseCoefficient = static_cast<float> (1.0 - std::exp (-tick / (ExpressionTracker::levelReleaseSeconds * sampleRate)));
    onFadePerTick = static_cast<float> (tick / (synthOnFadeSeconds * sampleRate));
    reset();
}

void SynthEngine::reset() noexcept
{
    samplePosition = 0;
    tables.fill (nullptr);
    applied.fill (fallbackSine.get());
    clearSounding();
    state = RunState::off;
    hasSettings = false;
    target = {};
    smoothed = {};
    voiceSettings = {};
    allocatorSettings = {};
    cachedVolumeDb = 1000.0f;
    volumeLinear = volumeGain = volumeTarget = volumeStep = 0.0f;
    onGain = onTarget = onStep = 0.0f;
    outputRampLeft = 0;
    bendRaw = 0;
    bendSmoothed = 0.0f;
    frames = nullptr;
    frameIndex = 0;
    carried = {};
    lastFramePosition = 0;
    hasLastFrame = false;
    expressionValue = expressionTarget = expressionStep = {};
    expressionRemaining = 0;
    releasePending = false;
}

void SynthEngine::clearSounding() noexcept
{
    // Voices, allocator, LFOs and FX to silence; the voices keep the applied tables (reset() forgets them first)
    allocator.reset();

    for (auto& v : voices)
    {
        v.reset();
        v.setTables (applied[0], applied[1]);
    }

    for (auto& l : lfos)
        l.reset();

    chorus.reset();
    delay.reset();
    reverb.reset();
    globalSources.fill (0.0f);
    lfoRateOctaves.fill (0.0f);
    chorusMixMod = delayMixMod = reverbMixMod = 0.0f;
    groupSize = 0;
    commands.clear();
}

void SynthEngine::releaseAll() noexcept
{
    releasePending = true;

    // A flush sends the MIDI port a centre bend as well (MidiOutputStage::flushAllNotes; MonitorSynth::releaseAll
    // does the same), and the restarted sources never send one again (Bend Mode Off): centre here too, or every
    // later note plays with the old bend (review 0.4 R1-1). The releasing tails slide back with the 5 ms smoother.
    bendRaw = 0;
}

void SynthEngine::setWavetable (int oscIndex, const Wavetable* table) noexcept
{
    if (oscIndex == 0 || oscIndex == 1)
        tables[static_cast<std::size_t> (oscIndex)] = table;
}

TablesInUse SynthEngine::getTablesInUse (int oscIndex) const noexcept
{
    if (oscIndex != 0 && oscIndex != 1)
        return {};

    TablesInUse result;
    const Wavetable* sine = fallbackSine.get();

    const auto add = [&result, sine] (const Wavetable* t) noexcept
    {
        if (t == nullptr || t == sine || t == result.current || t == result.previous)
            return;

        if (result.current == nullptr)
            result.current = t;
        else if (result.previous == nullptr)
            result.previous = t;
        // A third pointer cannot occur: tables reach the voices only between crossfades (forwardTables)
    };

    for (const auto& v : voices)
    {
        const auto inUse = v.getTablesInUse (oscIndex);
        add (inUse.current);
        add (inUse.previous);
    }

    return result;
}

float SynthEngine::bendSemitones (int raw) const noexcept
{
    return static_cast<float> (raw) / 8192.0f * static_cast<float> (target.bendRangeSemitones);
}

void SynthEngine::updateDerivedSettings() noexcept
{
    voiceSettings = smoothed;

    if (smoothed.playMode == PlayMode::poly)
        for (auto& o : voiceSettings.osc)
            o.unison = std::min (o.unison, polyMaxUnison);

    allocatorSettings.playMode = target.playMode;
    allocatorSettings.glideMode = target.glideMode;
    allocatorSettings.glideEnabled = target.glideTimeMs > 0.0f; // latched, not smoothed (10.2)

    if (smoothed.synthVolumeDb != cachedVolumeDb)
    {
        cachedVolumeDb = smoothed.synthVolumeDb;
        volumeLinear = decibelsToGain (cachedVolumeDb);
    }
}

void SynthEngine::smoothSettings() noexcept
{
    // Everything that is not listed here (choices, ints, bools) switches at the tick
    auto next = target;
    const auto c = smoothingCoefficient;
    const auto lin = [c] (float v, float t) noexcept { return smoothLinear (v, t, c); };
    const auto log = [c] (float v, float t) noexcept { return smoothLog (v, t, c); };
    const auto& s = smoothed;

    next.synthVolumeDb = lin (s.synthVolumeDb, target.synthVolumeDb);
    next.dryLevelDb = lin (s.dryLevelDb, target.dryLevelDb);
    next.glideTimeMs = lin (s.glideTimeMs, target.glideTimeMs);
    next.ampVelocityPercent = lin (s.ampVelocityPercent, target.ampVelocityPercent);
    next.ampGainDb = lin (s.ampGainDb, target.ampGainDb);

    for (std::size_t i = 0; i < next.osc.size(); ++i)
    {
        auto& o = next.osc[i];
        const auto& so = s.osc[i];
        const auto& to = target.osc[i];
        o.positionPercent = lin (so.positionPercent, to.positionPercent);
        o.fineCents = lin (so.fineCents, to.fineCents);
        o.levelPercent = lin (so.levelPercent, to.levelPercent);
        o.panPercent = lin (so.panPercent, to.panPercent);
        o.detuneCents = lin (so.detuneCents, to.detuneCents);
        o.spreadPercent = lin (so.spreadPercent, to.spreadPercent);
        o.phaseRandPercent = lin (so.phaseRandPercent, to.phaseRandPercent);
    }

    next.subLevelPercent = lin (s.subLevelPercent, target.subLevelPercent);
    next.noiseLevelPercent = lin (s.noiseLevelPercent, target.noiseLevelPercent);
    next.noiseColorPercent = lin (s.noiseColorPercent, target.noiseColorPercent);
    next.filterCutoffHz = log (s.filterCutoffHz, target.filterCutoffHz);
    next.filterResonancePercent = lin (s.filterResonancePercent, target.filterResonancePercent);
    next.filterDriveDb = lin (s.filterDriveDb, target.filterDriveDb);
    next.filterKeytrackPercent = lin (s.filterKeytrackPercent, target.filterKeytrackPercent);
    next.filterEnvAmountPercent = lin (s.filterEnvAmountPercent, target.filterEnvAmountPercent);

    for (std::size_t i = 0; i < next.env.size(); ++i)
    {
        next.env[i].attackMs = lin (s.env[i].attackMs, target.env[i].attackMs);
        next.env[i].decayMs = lin (s.env[i].decayMs, target.env[i].decayMs);
        next.env[i].sustainPercent = lin (s.env[i].sustainPercent, target.env[i].sustainPercent);
        next.env[i].releaseMs = lin (s.env[i].releaseMs, target.env[i].releaseMs);
    }

    for (std::size_t i = 0; i < next.lfo.size(); ++i)
    {
        next.lfo[i].rateHz = log (s.lfo[i].rateHz, target.lfo[i].rateHz);
        next.lfo[i].fadeMs = lin (s.lfo[i].fadeMs, target.lfo[i].fadeMs);
    }

    for (std::size_t i = 0; i < next.mod.size(); ++i)
        next.mod[i].amountPercent = lin (s.mod[i].amountPercent, target.mod[i].amountPercent);

    next.chorus.rateHz = log (s.chorus.rateHz, target.chorus.rateHz);
    next.chorus.depthPercent = lin (s.chorus.depthPercent, target.chorus.depthPercent);
    next.chorus.mixPercent = lin (s.chorus.mixPercent, target.chorus.mixPercent);
    next.delay.timeMs = log (s.delay.timeMs, target.delay.timeMs);
    next.delay.feedbackPercent = lin (s.delay.feedbackPercent, target.delay.feedbackPercent);
    next.delay.mixPercent = lin (s.delay.mixPercent, target.delay.mixPercent);
    next.reverb.sizePercent = lin (s.reverb.sizePercent, target.reverb.sizePercent);
    next.reverb.dampingPercent = lin (s.reverb.dampingPercent, target.reverb.dampingPercent);
    next.reverb.mixPercent = lin (s.reverb.mixPercent, target.reverb.mixPercent);

    smoothed = next;
}

void SynthEngine::activate() noexcept
{
    // synthOn while off: start from silence with the settings as they are (no 20 ms glide from stale values)
    smoothed = target;
    hasSettings = true;
    updateDerivedSettings();
    state = RunState::running;
    onGain = onTarget = 1.0f;
    onStep = 0.0f;
    volumeGain = volumeTarget = volumeLinear;
    volumeStep = 0.0f;
    outputRampLeft = 0;
    bendSmoothed = bendSemitones (bendRaw);
}

void SynthEngine::forwardTables() noexcept
{
    for (int o = 0; o < numOscillators; ++o)
    {
        const auto oi = static_cast<std::size_t> (o);
        const Wavetable* wanted = tables[oi] != nullptr ? tables[oi] : fallbackSine.get();

        if (wanted == applied[oi])
            continue;

        bool fading = false;

        for (const auto& v : voices)
            fading = fading || v.getTablesInUse (o).previous != nullptr;

        if (fading)
            continue; // the running crossfade ends first (<= 5 ms); the newest table follows then

        applied[oi] = wanted;

        for (auto& v : voices)
            v.setTables (applied[0], applied[1]);
    }
}

void SynthEngine::consumeFrame (int index, PendingFrame& out) noexcept
{
    const auto& f = (*frames)[static_cast<std::size_t> (index)];
    const auto offset = std::clamp (f.sampleOffset, 0, std::max (0, callSamples - 1));
    const auto position = callStart + offset;
    // Sanitised: non-finite -> 0, every value in its range (16.1)
    const auto unit = [] (float v) noexcept { return std::isfinite (v) ? std::clamp (v, 0.0f, 1.0f) : 0.0f; };
    out.frame = f;
    out.frame.level = unit (f.level);
    out.frame.brightness = unit (f.brightness);
    out.frame.noisiness = unit (f.noisiness);
    out.frame.pitchCents = std::isfinite (f.pitchCents) ? std::clamp (f.pitchCents, -100.0f, 100.0f) : 0.0f;
    out.position = position;
    out.distance = hasLastFrame ? static_cast<int> (std::clamp<int64_t> (position - lastFramePosition, controlTickSamples,
                                                                         maxFrameDistance))
                                : controlTickSamples;
    out.valid = true;
    lastFramePosition = position;
    hasLastFrame = true;
}

void SynthEngine::updateExpression (int64_t tickPosition) noexcept
{
    if (frames == nullptr)
    {
        // Test Pattern: rest values, the level smoothed like the tracker's (attack 5 ms, release 50 ms)
        const auto rest = restExpression (state != RunState::off && allocator.getNumHeldNotes() > 0);
        const auto c = rest.level > expressionValue.level ? restAttackCoefficient : restReleaseCoefficient;
        const auto level = expressionValue.level + c * (rest.level - expressionValue.level);
        expressionValue = rest;
        expressionValue.level = std::abs (rest.level - level) < 1.0e-5f ? rest.level : level;
        expressionRemaining = 0;
        return;
    }

    // Latch the newest frame at or before this tick
    PendingFrame latched;

    if (carried.valid && carried.position <= tickPosition)
    {
        latched = carried;
        carried.valid = false;
    }

    while (frameIndex < static_cast<int> (frames->size()))
    {
        const auto offset = std::clamp ((*frames)[static_cast<std::size_t> (frameIndex)].sampleOffset, 0,
                                        std::max (0, callSamples - 1));

        if (callStart + offset > tickPosition)
            break;

        consumeFrame (frameIndex++, latched);
    }

    if (latched.valid)
    {
        // Linear ramp from the current value to the frame over the distance to the previous frame
        const auto ticks = static_cast<float> (controlTickSamples) / static_cast<float> (latched.distance);
        expressionTarget = latched.frame;
        expressionStep.level = (expressionTarget.level - expressionValue.level) * ticks;
        expressionStep.brightness = (expressionTarget.brightness - expressionValue.brightness) * ticks;
        expressionStep.noisiness = (expressionTarget.noisiness - expressionValue.noisiness) * ticks;
        expressionStep.pitchCents = (expressionTarget.pitchCents - expressionValue.pitchCents) * ticks;
        expressionRemaining = latched.distance;
    }

    if (expressionRemaining > controlTickSamples)
    {
        expressionValue.level += expressionStep.level;
        expressionValue.brightness += expressionStep.brightness;
        expressionValue.noisiness += expressionStep.noisiness;
        expressionValue.pitchCents += expressionStep.pitchCents;
        expressionRemaining -= controlTickSamples;
    }
    else if (expressionRemaining > 0)
    {
        expressionValue = expressionTarget;
        expressionRemaining = 0;
    }

    expressionValue.sampleOffset = 0;
}

void SynthEngine::applyCommands (int64_t position) noexcept
{
    const auto phase = static_cast<int> (position % controlTickSamples);
    const int samplesToNextTick = controlTickSamples - phase;
    const VoiceContext context { &voiceSettings, &globalSources, bendSmoothed };

    for (const auto& c : commands.view())
    {
        if (c.voice >= maxVoices)
            continue;

        // Envelope-starting note starts retrigger the LFOs (8.2); legato / correction do not
        if (c.type == VoiceCommandType::start || c.type == VoiceCommandType::steal)
            for (std::size_t i = 0; i < lfos.size(); ++i)
                lfos[i].retrigger (smoothed.lfo[i]);

        voices[c.voice].apply (c, context, samplesToNextTick);
    }

    commands.clear();
}

void SynthEngine::handleGroup (int64_t position) noexcept
{
    if (groupSize <= 0)
        return;

    if (state == RunState::off && target.synthOn)
        activate();

    if (state == RunState::fadingOut)
    {
        // Fading out: note-ons are ignored, note-offs still reach the allocator - synthOn may come back before
        // the fade ends (automation), and a dropped note-off would leave a hung note (review 0.4 R1-2)
        int kept = 0;

        for (int i = 0; i < groupSize; ++i)
            if (! group[static_cast<std::size_t> (i)].isNoteOn)
                group[static_cast<std::size_t> (kept++)] = group[static_cast<std::size_t> (i)];

        groupSize = kept;

        if (groupSize == 0)
            return;
    }
    else if (state != RunState::running)
    {
        groupSize = 0; // synth off: notes are ignored, a held note sounds from the next note-on
        return;
    }

    std::array<bool, maxVoices> active {};
    bool anyActive = false;

    for (std::size_t v = 0; v < voices.size(); ++v)
    {
        active[v] = voices[v].isActive();
        anyActive = anyActive || active[v];
    }

    // A note from silence starts on its bend (no slide from the previous note's bend, 3.4)
    if (! anyActive)
        for (int i = 0; i < groupSize; ++i)
            if (group[static_cast<std::size_t> (i)].isNoteOn)
                bendSmoothed = bendSemitones (bendRaw);

    commands.clear();
    allocator.processGroup (std::span<const NoteInput> (group.data(), static_cast<std::size_t> (groupSize)), position,
                            allocatorSettings, active, commands);
    groupSize = 0;
    applyCommands (position);
}

void SynthEngine::controlTick (int64_t tickPosition) noexcept
{
    if (state == RunState::fadingOut && onGain <= 0.0f)
    {
        // Fade ended (decided here, at the tick): clear everything, keep the clock and the tables
        clearSounding();
        state = RunState::off;
        hasSettings = false;
        volumeGain = volumeTarget = volumeStep = 0.0f;
        onGain = onTarget = onStep = 0.0f;
        outputRampLeft = 0;
    }

    if (state == RunState::off)
    {
        if (! target.synthOn)
        {
            updateExpression (tickPosition); // the meters follow the voice while the synth is off
            return;
        }

        activate();
    }

    // 1. Settings (20 ms smoothing, choices switch); a play-mode change releases every voice
    const auto previousMode = smoothed.playMode;

    if (! hasSettings)
    {
        smoothed = target;
        hasSettings = true;
    }
    else
    {
        smoothSettings();
    }

    updateDerivedSettings();

    if (smoothed.playMode != previousMode)
    {
        commands.clear();
        allocator.releaseAll (commands);
        applyCommands (tickPosition);
    }

    if (state == RunState::running && ! target.synthOn)
        state = RunState::fadingOut;
    else if (state == RunState::fadingOut && target.synthOn)
        state = RunState::running;

    // 2. Expression frames / rest values; bend smoother (5 ms one-pole per tick)
    updateExpression (tickPosition);

    const auto bendGoal = bendSemitones (bendRaw);
    bendSmoothed = smoothLinear (bendSmoothed, bendGoal, bendCoefficient);

    // 3. LFOs with the previous tick's rate modulation
    for (std::size_t i = 0; i < lfos.size(); ++i)
        globalSources[static_cast<std::size_t> (ModSource::lfo1) + i]
            = lfos[i].advanceTick (smoothed.lfo[i], transport, lfoRateOctaves[i], controlTickSamples);

    // 4. Global sources and destinations (with the per-voice sources of the newest voice)
    globalSources[static_cast<std::size_t> (ModSource::voiceLevel)] = expressionValue.level;
    globalSources[static_cast<std::size_t> (ModSource::voiceBrightness)] = expressionValue.brightness;
    globalSources[static_cast<std::size_t> (ModSource::voiceNoisiness)] = expressionValue.noisiness;
    globalSources[static_cast<std::size_t> (ModSource::voicePitch)] = std::clamp (expressionValue.pitchCents * 0.01f, -1.0f, 1.0f);

    std::array<bool, maxVoices> active {};

    for (std::size_t v = 0; v < voices.size(); ++v)
        active[v] = voices[v].isActive();

    auto sources = globalSources;
    const auto newest = allocator.getNewestVoice (active);

    if (newest >= 0)
        for (const auto source : { ModSource::env2, ModSource::env3, ModSource::velocity, ModSource::note,
                                   ModSource::randomPerNote })
            sources[static_cast<std::size_t> (source)] = voices[static_cast<std::size_t> (newest)].getSourceValue (source);

    ModDestValues global {};
    ModMatrix::computeGlobal (smoothed.mod, sources, global);
    const auto dest = [&global] (ModDest d) { return global[static_cast<std::size_t> (d)]; };
    lfoRateOctaves[0] = dest (ModDest::lfo1Rate);
    lfoRateOctaves[1] = dest (ModDest::lfo2Rate);
    chorusMixMod = dest (ModDest::chorusMix);
    delayMixMod = dest (ModDest::delayMix);
    reverbMixMod = dest (ModDest::reverbMix);

    // 5. Tables (between crossfades only), then the voices
    forwardTables();
    const VoiceContext context { &voiceSettings, &globalSources, bendSmoothed };

    for (auto& v : voices)
        if (v.isActive())
            v.controlTick (context);

    // 6. FX
    chorus.beginTick (smoothed.chorus, chorusMixMod, controlTickSamples);
    delay.beginTick (smoothed.delay, transport, delayMixMod, controlTickSamples);
    reverb.beginTick (smoothed.reverb, reverbMixMod, controlTickSamples);

    // Output ramps: synthVolume and the synthOn fade
    volumeGain = volumeTarget;
    onGain = onTarget;
    volumeTarget = volumeLinear;
    onTarget = state == RunState::running ? std::min (1.0f, onGain + onFadePerTick) : std::max (0.0f, onGain - onFadePerTick);
    const auto inv = 1.0f / static_cast<float> (controlTickSamples);
    volumeStep = (volumeTarget - volumeGain) * inv;
    onStep = (onTarget - onGain) * inv;
    outputRampLeft = controlTickSamples;
}

void SynthEngine::renderSegment (float* left, float* right, int numSamples) noexcept
{
    if (state == RunState::off)
        return; // the buffers are already 0

    for (auto& v : voices)
        if (v.isActive())
            v.render (left, right, numSamples);

    chorus.process (left, right, numSamples);
    delay.process (left, right, numSamples);
    reverb.process (left, right, numSamples);

    float vol = volumeGain, on = onGain;
    const int ramp = std::min (numSamples, outputRampLeft);
    const float sv = volumeStep, so = onStep;

    for (int i = 0; i < ramp; ++i)
    {
        vol += sv;
        on += so;
        const auto g = vol * on;
        left[i] *= g;
        right[i] *= g;
    }

    if (ramp > 0)
    {
        outputRampLeft -= ramp;

        if (outputRampLeft == 0)
        {
            vol = volumeTarget; // exactly on the target at the tick end
            on = onTarget;
        }
    }

    const auto g = vol * on;

    for (int i = ramp; i < numSamples; ++i)
    {
        left[i] *= g;
        right[i] *= g;
    }

    volumeGain = vol;
    onGain = on;
}

void SynthEngine::process (std::span<const VsEvent> events, std::span<const VsEventTiming> timings,
                           const ExpressionBuffer* expression, const SynthSettings& settings,
                           const SynthTransport& newTransport, float* left, float* right, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    if (left == nullptr || right == nullptr)
    {
        samplePosition += numSamples;
        return;
    }

    std::fill (left, left + numSamples, 0.0f);
    std::fill (right, right + numSamples, 0.0f);

    target = settings.sanitised();
    transport = newTransport;
    frames = expression;
    frameIndex = 0;
    callStart = samplePosition;
    callSamples = numSamples;

    if (expression == nullptr)
        carried.valid = false;

    if (releasePending)
    {
        releasePending = false;

        if (state != RunState::off)
        {
            commands.clear();
            allocator.releaseAll (commands);
            applyCommands (samplePosition);
        }
    }

    const auto numEvents = events.size();
    std::size_t eventIndex = 0;
    const auto offsetOf = [&events, numSamples] (std::size_t i) noexcept
    { return std::clamp (events[i].sampleOffset, 0, numSamples - 1); };

    int pos = 0;

    while (pos < numSamples)
    {
        const auto absolute = samplePosition + pos;

        // Events at this position (late or unsorted ones are taken here too): bends first, then the notes
        while (eventIndex < numEvents && offsetOf (eventIndex) <= pos)
        {
            const auto& e = events[eventIndex];

            if (e.type == VsEventType::pitchBend)
            {
                bendRaw = std::clamp (static_cast<int> (e.bend), -8192, 8191);
            }
            else if (isNoteEvent (e))
            {
                if (groupSize >= static_cast<int> (group.size()))
                    handleGroup (absolute); // more note events at one sample than a group holds (never in practice)

                NoteInput in;
                in.isNoteOn = e.type == VsEventType::noteOn && e.data2 > 0;
                in.note = static_cast<uint8_t> (e.data1 & 0x7f);
                in.velocity = static_cast<uint8_t> (std::clamp (static_cast<int> (e.data2), 1, 127));
                in.correction = eventIndex < timings.size()
                                && (timings[eventIndex].flags & EventTimingFlags::correction) != 0;
                group[static_cast<std::size_t> (groupSize++)] = in;
            }

            ++eventIndex;
        }

        handleGroup (absolute);

        const auto phase = static_cast<int> (absolute % controlTickSamples);

        if (phase == 0)
            controlTick (absolute);

        int end = std::min (numSamples, pos + (controlTickSamples - phase));

        if (eventIndex < numEvents)
            end = std::min (end, offsetOf (eventIndex));

        renderSegment (left + pos, right + pos, end - pos);
        pos = end;
    }

    // Frames after the call's last tick: the newest waits for the next tick (newest wins)
    if (frames != nullptr)
        while (frameIndex < static_cast<int> (frames->size()))
            consumeFrame (frameIndex++, carried);

    frames = nullptr;
    samplePosition += numSamples;
}

SynthEngineStats SynthEngine::getStats() const noexcept
{
    SynthEngineStats stats;

    for (const auto& v : voices)
    {
        stats.activeVoices += v.isActive() ? 1 : 0;
        stats.nanResets += v.getNanResetCount();
    }

    stats.nanResets += chorus.getNanResetCount() + delay.getNanResetCount() + reverb.getNanResetCount();
    return stats;
}

} // namespace vocalsynth::synth
