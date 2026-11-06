#include "vocalsynth/synth/SynthVoice.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vocalsynth::synth
{

// One synth voice (docs/SYNTH-DESIGN.md 3.3, 10.3). Every target is computed for the END of the current ramp
// segment (a tick, or the rest of a tick after a mid-tick command) and every module ramps linearly to it.
// The envelopes and the glide are always one segment ahead: at a control position they are advanced to the
// segment end, and the amp / modulation ramps run from the value at the segment start (envFrom / pitchFrom)
// to that end value (envTo / pitch). A command inside a segment therefore finds the CURRENT values on these
// ramps (rampDone / rampLength), restarts what it restarts from them and re-targets everything over the
// remaining samples; every position involved is absolute, so any split of the host block gives the same
// floating-point operations (3.5).
//
// Retrigger (start on a sounding voice): the envelopes restart from their current values and the oscillator
// phases run on (a restart would reset the oscillator gains to 0 and click); a clean start with new phases,
// a cleared filter and envelopes from 0 happens only from silence and after a steal fade.
//
// Modules that are silent are skipped: an oscillator that is off, a sub / noise at level 0 are neither
// ticked nor rendered (one more tick with level 0 after they were audible, so they ramp down first).

namespace
{

constexpr float centreNote = 60.0f;
constexpr float cutoffSmoothingSeconds = 0.001f; ///< log2 one-pole per tick on top of the 20 ms parameter smoothing

inline float clamp01 (float v) noexcept
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float finiteOr (float v, float fallback) noexcept
{
    return std::isfinite (v) ? v : fallback;
}

inline double incrementFor (double pitchSemitones, double sampleRate) noexcept
{
    return 440.0 * std::exp2 ((pitchSemitones - 69.0) / 12.0) / sampleRate;
}

} // namespace

void SynthVoice::prepare (double newSampleRate, uint32_t newSeed)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    seed = newSeed != 0 ? newSeed : 1u;
    random = seed;

    for (auto& o : osc)
        o.prepare (sampleRate);

    sub.prepare (sampleRate);
    noise.prepare (sampleRate, seed);
    filter.prepare (sampleRate);

    for (auto& e : env)
        e.prepare (sampleRate);

    stealFadeLength = std::max (1, static_cast<int> (std::ceil (VoiceAllocator::stealFadeSeconds * sampleRate)));
    cutoffCoefficient = static_cast<float> (
        1.0 - std::exp (-static_cast<double> (controlTickSamples) / (cutoffSmoothingSeconds * sampleRate)));
    reset();
}

void SynthVoice::reset() noexcept
{
    for (auto& o : osc)
        o.reset();

    tablesSet.fill (nullptr);

    sub.reset();
    noise.reset();
    filter.reset();

    for (auto& e : env)
        e.reset();

    note = -1;
    velocity = 0.0f;
    randomValue = 0.0f;
    random = seed;
    pitch = 0.0f;
    glideStart = 0.0f;
    glideTarget = 0.0f;
    glideProgress = 1.0;
    ampGain = ampStep = 0.0f;
    ampRight = ampRightStep = 0.0f;
    ampTargetLeft = ampTargetRight = 0.0f;
    stealFadeSamples = 0;
    hasPendingStart = false;
    fadeGain = 1.0f;
    fadeStep = 0.0f;
    active = false;
    released = false;
    startedFromSilence = false;
    rampLength = controlTickSamples;
    rampDone = controlTickSamples;
    envFrom.fill (0.0f);
    envTo.fill (0.0f);
    pitchFrom = 0.0f;
    cutoffLog2 = 14.0f;
    cachedPan = 0.0f;
    panLeft = panRight = 1.0f;
    oscLevelSent.fill (0.0f);
    oscRunning.fill (false);
    subLevelSent = noiseLevelSent = 0.0f;
    subRunning = noiseRunning = filterRunning = false;
    voiceSources.fill (0.0f);
    restarted.fill (false);
}

void SynthVoice::deactivate() noexcept
{
    // Silent: the next note starts clean (startFromSilence clears the DSP states). The pitch stays for a
    // glide "Always" from silence. The oscillators forget every table pointer and take the newest one set
    // (docs/SYNTH-DESIGN.md 12.1): an oscillator that stopped inside a table crossfade (switched off mid-fade,
    // or the note ended) would otherwise report the faded-out table for ever. Not the oscillator's current
    // table: a setTables() earlier in this tick is still queued in it.
    for (std::size_t i = 0; i < osc.size(); ++i)
    {
        osc[i].reset();
        osc[i].setTable (tablesSet[i]);
    }

    active = false;
    released = false;
    hasPendingStart = false;
    stealFadeSamples = 0;
    fadeGain = 1.0f;
    fadeStep = 0.0f;
    ampGain = ampStep = ampRight = ampRightStep = 0.0f;
    ampTargetLeft = ampTargetRight = 0.0f;
    envFrom.fill (0.0f);
    envTo.fill (0.0f);
    rampLength = rampDone = controlTickSamples;
    oscLevelSent.fill (0.0f);
    oscRunning.fill (false);
    subLevelSent = noiseLevelSent = 0.0f;
    subRunning = noiseRunning = filterRunning = false;
    note = -1;
}

void SynthVoice::setTables (const Wavetable* oscA, const Wavetable* oscB) noexcept
{
    tablesSet = { oscA, oscB };
    osc[0].setTable (oscA);
    osc[1].setTable (oscB);
}

TablesInUse SynthVoice::getTablesInUse (int oscIndex) const noexcept
{
    return osc[oscIndex == 0 ? 0u : 1u].getTablesInUse();
}

float SynthVoice::nextRandom() noexcept
{
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return static_cast<float> (static_cast<double> (random) * (2.0 / 4294967295.0) - 1.0);
}

float SynthVoice::currentEnvelopeValue (int index) const noexcept
{
    const auto i = static_cast<std::size_t> (index);

    if (rampDone >= rampLength)
        return envTo[i];

    return envFrom[i] + (envTo[i] - envFrom[i]) * static_cast<float> (rampDone) / static_cast<float> (rampLength);
}

float SynthVoice::currentPitch() const noexcept
{
    if (rampDone >= rampLength)
        return pitch;

    return pitchFrom + (pitch - pitchFrom) * static_cast<float> (rampDone) / static_cast<float> (rampLength);
}

void SynthVoice::advanceGlide (int samples, float glideTimeMs) noexcept
{
    if (glideProgress >= 1.0)
        return;

    const auto length = std::isfinite (glideTimeMs) ? static_cast<double> (glideTimeMs) * 0.001 * sampleRate : 0.0;
    glideProgress = length > 0.0 ? std::min (1.0, glideProgress + static_cast<double> (samples) / length) : 1.0;
    pitch = glideProgress >= 1.0 ? glideTarget
                                 : static_cast<float> (static_cast<double> (glideStart)
                                                       + static_cast<double> (glideTarget - glideStart) * glideProgress);
}

void SynthVoice::setPitchTarget (const VoiceCommand& command, const VoiceContext& context, float fromPitch) noexcept
{
    const auto target = static_cast<float> (command.note);
    pitchRestarted = true;
    const bool glide = command.glide && command.type != VoiceCommandType::correction
                       && context.settings->glideTimeMs > 0.0f;

    if (glide)
    {
        glideStart = command.glideFromNote >= 0 ? static_cast<float> (command.glideFromNote) : fromPitch;
        glideTarget = target;
        glideProgress = 0.0;
        pitch = glideStart;
    }
    else
    {
        glideStart = glideTarget = target;
        glideProgress = 1.0;
        pitch = target;
    }
}

void SynthVoice::startFromSilence (const VoiceCommand& command, const VoiceContext& context) noexcept
{
    const auto& s = *context.settings;
    const auto previousPitch = pitch; // the last note's pitch (glide "Always" from silence)

    filter.reset();
    sub.restart();

    for (std::size_t i = 0; i < osc.size(); ++i)
    {
        nextRandom();
        osc[i].restart (random, clamp01 (finiteOr (s.osc[i].phaseRandPercent, 0.0f) * 0.01f));
    }

    for (auto& e : env)
    {
        e.reset();
        e.noteOn (0.0f);
    }

    envFrom.fill (0.0f);
    envTo.fill (0.0f);
    restarted.fill (true);
    note = command.note;
    velocity = static_cast<float> (command.velocity) / 127.0f;
    randomValue = nextRandom();
    setPitchTarget (command, context, previousPitch);
    pitchFrom = pitch;
    ampGain = ampStep = ampRight = ampRightStep = 0.0f;
    ampTargetLeft = ampTargetRight = 0.0f;
    fadeGain = 1.0f;
    fadeStep = 0.0f;
    stealFadeSamples = 0;
    hasPendingStart = false;
    oscLevelSent.fill (0.0f);
    oscRunning.fill (false);
    subLevelSent = noiseLevelSent = 0.0f;
    subRunning = noiseRunning = filterRunning = false;
    rampLength = controlTickSamples;
    rampDone = controlTickSamples;
    active = true;
    released = false;
    startedFromSilence = true;
}

void SynthVoice::changeNote (const VoiceCommand& command, const VoiceContext& context, bool restartEnvelopes,
                             bool newVelocity) noexcept
{
    if (restartEnvelopes)
    {
        for (std::size_t i = 0; i < env.size(); ++i)
        {
            env[i].noteOn (currentEnvelopeValue (static_cast<int> (i)));
            restarted[i] = true;
        }

        released = false;
    }

    if (newVelocity)
    {
        velocity = static_cast<float> (command.velocity) / 127.0f;
        randomValue = nextRandom();
    }

    note = command.note;
    setPitchTarget (command, context, currentPitch());
}

void SynthVoice::apply (const VoiceCommand& command, const VoiceContext& context, int samplesToNextTick) noexcept
{
    if (context.settings == nullptr || context.global == nullptr)
        return;

    const int r = std::clamp (samplesToNextTick, 1, controlTickSamples);
    const bool atTick = r >= controlTickSamples;

    // Values at this sample, before the command changes anything (the ramps restart from them)
    std::array<float, numEnvelopes> now {};

    for (int i = 0; i < numEnvelopes; ++i)
        now[static_cast<std::size_t> (i)] = currentEnvelopeValue (i);

    const auto pitchNow = currentPitch();
    restarted.fill (false);
    pitchRestarted = false;
    bool changed = true;

    switch (command.type)
    {
        case VoiceCommandType::start:
            if (hasPendingStart)
                pendingStart = command; // the note of the running steal fade starts again: take the newest
            else if (! active)
                startFromSilence (command, context);
            else if (fadeStep > 0.0f)
            {
                // A steal fade whose pending note was released (or that ended, waiting for its tick) runs to
                // silence: the note starts clean after it instead of being retriggered inside it and then
                // deactivated with the fade end (review 0.4 R1-3)
                pendingStart = command;
                hasPendingStart = true;
                released = false;
                changed = false;
            }
            else
                changeNote (command, context, true, true);
            break;

        case VoiceCommandType::steal:
            if (! active)
            {
                auto start = command;
                start.type = VoiceCommandType::start;
                startFromSilence (start, context);
            }
            else
            {
                // Fade out what sounds (a second steal during the fade only replaces the pending note)
                pendingStart = command;
                pendingStart.type = VoiceCommandType::start;

                if (! hasPendingStart && fadeStep <= 0.0f) // no fade running or ended without its tick yet
                {
                    stealFadeSamples = stealFadeLength;
                    fadeStep = fadeGain / static_cast<float> (stealFadeLength);
                }

                hasPendingStart = true;
                changed = false;
            }
            break;

        case VoiceCommandType::legato:
        case VoiceCommandType::correction:
        {
            const bool correction = command.type == VoiceCommandType::correction;

            if (hasPendingStart)
            {
                pendingStart.note = command.note;

                if (! correction)
                    pendingStart.velocity = command.velocity;

                changed = false;
            }
            else if (! active)
            {
                auto start = command;
                start.type = VoiceCommandType::start;
                startFromSilence (start, context);
            }
            else
            {
                changeNote (command, context, false, ! correction);
            }
            break;
        }

        case VoiceCommandType::release:
            if (hasPendingStart)
            {
                hasPendingStart = false; // the pending note ended before it started: the fade ends in silence
                released = true;
                changed = false;
            }
            else if (active && ! released)
            {
                for (std::size_t i = 0; i < env.size(); ++i)
                {
                    env[i].noteOff (now[i]);
                    restarted[i] = true;
                }

                released = true;
            }
            else
            {
                changed = false;
            }
            break;
    }

    if (atTick || ! changed || ! active)
        return; // at a tick boundary controlTick() follows at once and advances everything

    // Mid-tick: what restarted advances to the tick end, every ramp restarts from its current value
    const auto& s = *context.settings;

    for (std::size_t i = 0; i < env.size(); ++i)
    {
        envFrom[i] = now[i];

        if (restarted[i])
        {
            envFrom[i] = env[i].getValue();
            envTo[i] = env[i].advance (s.env[i], r);
        }
    }

    pitchFrom = startedFromSilence ? pitch : pitchNow;

    if (pitchRestarted)
        advanceGlide (r, s.glideTimeMs); // a glide that started here (a running one is at the tick end already)

    retarget (context, r, false);
}

void SynthVoice::controlTick (const VoiceContext& context) noexcept
{
    if (! active || context.settings == nullptr || context.global == nullptr)
        return;

    const auto& s = *context.settings;

    // End of a steal fade (decided here at the tick): the pending note starts clean, or the voice ends
    if (fadeStep > 0.0f && stealFadeSamples <= 0)
    {
        if (hasPendingStart)
        {
            const auto start = pendingStart;
            startFromSilence (start, context);
        }
        else
        {
            deactivate();
            return;
        }
    }

    for (std::size_t i = 0; i < env.size(); ++i)
    {
        envFrom[i] = envTo[i];
        envTo[i] = env[i].advance (s.env[i], controlTickSamples);
    }

    pitchFrom = pitch;
    advanceGlide (controlTickSamples, s.glideTimeMs);

    // Inactive once Env 1 is idle and its ramp has reached 0 (one tick after the envelope ended)
    if (! env[0].isActive() && envFrom[0] == 0.0f && ampGain == 0.0f && ampRight == 0.0f && ! hasPendingStart)
    {
        deactivate();
        return;
    }

    retarget (context, controlTickSamples, true);
}

void SynthVoice::retarget (const VoiceContext& context, int n, bool newTick) noexcept
{
    const auto& s = *context.settings;

    // Per-voice sources at the segment end + the global ones
    voiceSources = *context.global;
    voiceSources[static_cast<std::size_t> (ModSource::env2)] = envTo[1];
    voiceSources[static_cast<std::size_t> (ModSource::env3)] = envTo[2];
    voiceSources[static_cast<std::size_t> (ModSource::velocity)] = velocity;
    voiceSources[static_cast<std::size_t> (ModSource::note)]
        = std::clamp ((static_cast<float> (note) - centreNote) / 60.0f, -1.0f, 1.0f);
    voiceSources[static_cast<std::size_t> (ModSource::randomPerNote)] = randomValue;

    ModDestValues mod {};
    ModMatrix::computePerVoice (s.mod, voiceSources, mod);
    const auto m = [&mod] (ModDest d) { return mod[static_cast<std::size_t> (d)]; };

    const auto bend = finiteOr (context.bendSemitones, 0.0f);
    const auto notePitch = pitch + bend + m (ModDest::pitch); // the voice's pitch: oscillators, sub, keytrack

    // Oscillators
    constexpr ModDest pitchDest[] = { ModDest::oscAPitch, ModDest::oscBPitch };
    constexpr ModDest positionDest[] = { ModDest::oscAPosition, ModDest::oscBPosition };
    constexpr ModDest levelDest[] = { ModDest::oscALevel, ModDest::oscBLevel };
    constexpr ModDest detuneDest[] = { ModDest::oscADetune, ModDest::oscBDetune };

    for (std::size_t i = 0; i < osc.size(); ++i)
    {
        const auto& o = s.osc[i];
        const auto level = o.on ? clamp01 (o.levelPercent * 0.01f + m (levelDest[i])) : 0.0f;
        const bool needed = o.on || oscLevelSent[i] > 0.0f;

        if (newTick)
            oscRunning[i] = needed;
        else if (needed)
            oscRunning[i] = true;

        if (! oscRunning[i])
            continue;

        OscillatorTick t;
        const auto p = static_cast<double> (notePitch) + 12.0 * o.octave + o.semitones
                       + static_cast<double> (o.fineCents) * 0.01 + static_cast<double> (m (pitchDest[i]));
        t.increment = incrementFor (p, sampleRate);
        t.position = clamp01 (o.positionPercent * 0.01f + m (positionDest[i]));
        t.level = level;
        t.pan = std::clamp (o.panPercent * 0.01f, -1.0f, 1.0f);
        t.detuneCents = std::clamp (o.detuneCents + m (detuneDest[i]), 0.0f, 200.0f);
        t.spread = clamp01 (o.spreadPercent * 0.01f);
        t.unison = o.unison;
        osc[i].beginTick (t, n);
        oscLevelSent[i] = level;
    }

    // Sub and noise
    {
        const auto level = clamp01 (s.subLevelPercent * 0.01f + m (ModDest::subLevel));
        const bool needed = level > 0.0f || subLevelSent > 0.0f;
        subRunning = newTick ? needed : (subRunning || needed);

        if (subRunning)
        {
            const auto octaves = s.subOctave == SubOctave::down2 ? 24.0 : 12.0;
            sub.beginTick (incrementFor (static_cast<double> (notePitch) - octaves, sampleRate), level, s.subShape, n);
            subLevelSent = level;
        }
    }
    {
        const auto level = clamp01 (s.noiseLevelPercent * 0.01f + m (ModDest::noiseLevel));
        const bool needed = level > 0.0f || noiseLevelSent > 0.0f;
        noiseRunning = newTick ? needed : (noiseRunning || needed);

        if (noiseRunning)
        {
            noise.beginTick (level, clamp01 (s.noiseColorPercent * 0.01f), n);
            noiseLevelSent = level;
        }
    }

    // Filter: cutoff in log2, smoothed per tick (a start from silence jumps)
    if (s.filterOn)
    {
        if (! filterRunning)
            filter.reset(); // switched on (again): states cleared, coefficients jump

        filterRunning = true;
        const auto keytrack = s.filterKeytrackPercent * 0.01f * (notePitch - centreNote);
        const auto envelope = s.filterEnvAmountPercent * 0.01f * 60.0f * envTo[1];
        const auto semitones = keytrack + envelope + m (ModDest::filterCutoff);
        const auto target = std::log2 (std::max (s.filterCutoffHz, 1.0f)) + semitones / 12.0f;

        if (std::isfinite (target))
        {
            if (startedFromSilence || ! std::isfinite (cutoffLog2))
                cutoffLog2 = target;
            else if (newTick)
                cutoffLog2 += cutoffCoefficient * (target - cutoffLog2);
        }

        FilterTick f;
        f.mode = s.filterMode;
        f.cutoffHz = std::exp2 (std::clamp (cutoffLog2, 4.0f, 16.0f)); // the filter clamps to 20 Hz .. 0.49 fs
        f.resonance = clamp01 (s.filterResonancePercent * 0.01f + m (ModDest::filterResonance));
        f.driveDb = std::clamp (s.filterDriveDb + m (ModDest::filterDrive), 0.0f, 24.0f);
        filter.beginTick (f, n);
    }
    else
    {
        filterRunning = false;
    }

    // Amp: Env 1 x velocity x patch gain x Amp Level mod, then the voice pan (equal power)
    if (s.ampGainDb != cachedAmpGainDb)
    {
        cachedAmpGainDb = s.ampGainDb;
        ampGainLinear = std::isfinite (s.ampGainDb) ? std::pow (10.0f, s.ampGainDb / 20.0f) : 1.0f;
    }

    const auto velocityGain = 1.0f - clamp01 (s.ampVelocityPercent * 0.01f) * (1.0f - velocity);
    const auto ampMod = std::clamp (1.0f + m (ModDest::ampLevel), 0.0f, 2.0f);
    const auto gain = envTo[0] * velocityGain * ampGainLinear * ampMod;
    const auto pan = std::clamp (m (ModDest::pan), -1.0f, 1.0f);

    if (pan != cachedPan)
    {
        cachedPan = pan;
        const auto angle = (static_cast<double> (pan) + 1.0) * std::numbers::pi * 0.25;
        panLeft = static_cast<float> (std::numbers::sqrt2 * std::cos (angle));
        panRight = static_cast<float> (std::numbers::sqrt2 * std::sin (angle));
    }

    ampTargetLeft = finiteOr (gain * panLeft, 0.0f);
    ampTargetRight = finiteOr (gain * panRight, 0.0f);
    const auto inv = 1.0f / static_cast<float> (n);
    ampStep = (ampTargetLeft - ampGain) * inv;
    ampRightStep = (ampTargetRight - ampRight) * inv;
    rampLength = n;
    rampDone = 0;
    startedFromSilence = false;
}

void SynthVoice::render (float* left, float* right, int numSamples) noexcept
{
    if (! active)
        return;

    // numSamples <= the samples left in the segment (protocol); longer calls are cut into tick-sized pieces
    while (numSamples > 0)
    {
        const int n = std::min (numSamples, controlTickSamples);
        renderSegment (left, right, n);
        left += n;
        right += n;
        numSamples -= n;
    }
}

void SynthVoice::renderSegment (float* left, float* right, int n) noexcept
{
    std::array<float, controlTickSamples> l {}, r {};

    for (std::size_t i = 0; i < osc.size(); ++i)
        if (oscRunning[i])
            osc[i].render (l.data(), r.data(), n);

    if (subRunning)
        sub.render (l.data(), r.data(), n);

    if (noiseRunning)
        noise.render (l.data(), r.data(), n);

    if (filterRunning)
        filter.process (l.data(), r.data(), n);

    auto gl = ampGain, gr = ampRight;
    const auto sl = ampStep, sr = ampRightStep;

    if (fadeStep > 0.0f)
    {
        auto fade = fadeGain;
        const auto fs = fadeStep;

        for (int i = 0; i < n; ++i)
        {
            gl += sl;
            gr += sr;
            fade = std::max (0.0f, fade - fs);
            left[i] += l[static_cast<std::size_t> (i)] * gl * fade;
            right[i] += r[static_cast<std::size_t> (i)] * gr * fade;
        }

        fadeGain = fade;
        stealFadeSamples -= n;
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            gl += sl;
            gr += sr;
            left[i] += l[static_cast<std::size_t> (i)] * gl;
            right[i] += r[static_cast<std::size_t> (i)] * gr;
        }
    }

    rampDone += n;

    if (rampDone >= rampLength)
    {
        gl = ampTargetLeft; // the ramp lands exactly on its target (an ended voice is exactly 0)
        gr = ampTargetRight;
        ampStep = ampRightStep = 0.0f;
    }

    ampGain = gl;
    ampRight = gr;
}

bool SynthVoice::isActive() const noexcept
{
    return active;
}

bool SynthVoice::isReleased() const noexcept
{
    return active && released;
}

float SynthVoice::getSourceValue (ModSource source) const noexcept
{
    const auto i = static_cast<int> (source);

    if (i <= 0 || i >= numModSources || ! isPerVoiceSource (source))
        return 0.0f;

    return voiceSources[static_cast<std::size_t> (i)];
}

uint32_t SynthVoice::getNanResetCount() const noexcept
{
    return filter.getNanResetCount();
}

} // namespace vocalsynth::synth
