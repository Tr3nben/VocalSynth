#include "TunerDisplayModel.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace vocalsynth
{

namespace
{
constexpr double a4Hz = 440.0;
constexpr double a4Note = 69.0;

double noteNumberOf (double hz) noexcept
{
    return a4Note + 12.0 * std::log2 (hz / a4Hz);
}

double hzOf (double noteNumber) noexcept
{
    return a4Hz * std::exp2 ((noteNumber - a4Note) / 12.0);
}
} // namespace

float TunerDisplayModel::smoothingCoefficient (double elapsedMs, double timeConstantMs) noexcept
{
    if (elapsedMs <= 0.0)
        return 0.0f;

    if (timeConstantMs <= 0.0)
        return 1.0f;

    return static_cast<float> (1.0 - std::exp (-elapsedMs / timeConstantMs));
}

void TunerDisplayModel::reset() noexcept
{
    state = {};
    candidateNote = -1;
    candidateMs = 0.0;
    sinceConfirmedMs = 0.0;
    sinceVoicedMs = 0.0;
    sinceTextMs = 0.0;
    smoothedPitch = 0.0;
    needleNeedsSnap = true;
    textNeedsSnap = false;
}

void TunerDisplayModel::updateNote (const Input& input, bool hasPitch, double elapsedMs, bool& appeared,
                                    bool& changed) noexcept
{
    int confirmed = -1;
    bool segmented = false;

    if (input.sungNote >= 0)
    {
        confirmed = input.sungNote;
        segmented = true;
        candidateNote = -1;
        candidateMs = 0.0;
    }
    else if (hasPitch)
    {
        if (input.nearestNote == candidateNote)
        {
            candidateMs += elapsedMs;
        }
        else
        {
            candidateNote = input.nearestNote;
            candidateMs = 0.0;
        }

        // The note already shown continues at once (e.g. after its note-off while still singing it)
        if (candidateMs >= noteStableMs || (state.note >= 0 && input.nearestNote == state.note))
            confirmed = input.nearestNote;
    }
    else
    {
        candidateNote = -1;
        candidateMs = 0.0;
    }

    if (confirmed >= 0)
    {
        appeared = state.note < 0;
        changed = ! appeared && confirmed != state.note;
        state.note = confirmed;
        state.noteIsSegmented = segmented;
        state.noteAlpha = 1.0f;
        sinceConfirmedMs = 0.0;
        return;
    }

    state.noteIsSegmented = false;

    if (state.note < 0)
        return;

    sinceConfirmedMs += elapsedMs;

    if (sinceConfirmedMs <= noteHoldMs)
        return;

    const auto fade = 1.0 - (sinceConfirmedMs - noteHoldMs) / noteFadeMs;

    if (fade > 0.0)
    {
        state.noteAlpha = static_cast<float> (fade);
        return;
    }

    // Faded out: everything that belongs to the note goes
    state.note = -1;
    state.noteAlpha = 0.0f;
    state.needleCents = 0.0f;
    state.displayHz = 0.0f;
    state.displayCents = 0;
    smoothedPitch = 0.0;
}

const TunerDisplayModel::State& TunerDisplayModel::update (const Input& input, double elapsedMs) noexcept
{
    if (! input.active)
    {
        reset();
        return state;
    }

    const auto dt = std::clamp (elapsedMs, 0.0, maxStepMs);
    const bool hasPitch = input.frequencyHz > 0.0f && std::isfinite (input.frequencyHz) && input.nearestNote >= 0;

    bool appeared = false;
    bool changed = false;
    updateNote (input, hasPitch, dt, appeared, changed);

    if (appeared)
        needleNeedsSnap = true;

    // A new note: the Hz / cents texts start from the new pitch at once (the needle still glides).
    // With the lagged smoothed pitch, "A4, 260 Hz" stood for 200 ms after A3 -> A4 (v0.2.1 review).
    if (changed)
        textNeedsSnap = true;

    bool refreshText = appeared;
    std::optional<int> snappedCents; // cents text of a note change (the needle is still gliding)

    // Needle and smoothed pitch follow only while there is a pitch; otherwise they hold
    if (state.note >= 0 && hasPitch)
    {
        const auto pitch = noteNumberOf (static_cast<double> (input.frequencyHz));
        const auto target = std::clamp (static_cast<float> ((pitch - state.note) * 100.0), -maxNeedleCents,
                                        maxNeedleCents);

        if (needleNeedsSnap)
        {
            state.needleCents = target;
            smoothedPitch = pitch;
            needleNeedsSnap = false;
            textNeedsSnap = false;
        }
        else
        {
            const auto k = smoothingCoefficient (dt, needleTimeConstantMs);
            state.needleCents += (target - state.needleCents) * k;
            smoothedPitch += (pitch - smoothedPitch) * static_cast<double> (k);
        }

        if (textNeedsSnap)
        {
            smoothedPitch = pitch;
            snappedCents = static_cast<int> (std::lround (target));
            textNeedsSnap = false;
            refreshText = true;
        }
    }

    // Stepped texts
    if (state.note >= 0)
    {
        sinceTextMs += dt;

        if (refreshText || state.displayHz <= 0.0f || sinceTextMs >= textIntervalMs)
        {
            if (smoothedPitch > 0.0)
            {
                state.displayHz = static_cast<float> (hzOf (smoothedPitch));
                state.displayCents = snappedCents.value_or (static_cast<int> (std::lround (state.needleCents)));
                sinceTextMs = 0.0;
            }
        }
    }
    else
    {
        sinceTextMs = 0.0;
    }

    // Voiced LED with hold
    if (input.voiced)
    {
        state.ledOn = true;
        sinceVoicedMs = 0.0;
    }
    else if (state.ledOn)
    {
        sinceVoicedMs += dt;
        state.ledOn = sinceVoicedMs < ledHoldMs;
    }

    // Confidence bar: fast attack, slower release
    const auto targetConfidence = std::clamp (std::isfinite (input.confidence) ? input.confidence : 0.0f, 0.0f, 1.0f);
    const auto k = smoothingCoefficient (dt, targetConfidence > state.confidence ? confidenceAttackMs
                                                                                : confidenceReleaseMs);
    state.confidence += (targetConfidence - state.confidence) * k;

    return state;
}

} // namespace vocalsynth
