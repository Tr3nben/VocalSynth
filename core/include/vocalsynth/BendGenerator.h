#pragma once

#include "vocalsynth/VoiceSettings.h"
#include "vocalsynth/VsEvent.h"

#include <cstdint>

namespace vocalsynth
{

/** Residual pitch (sung pitch - note) -> 14-bit pitch bend (docs/M2-DESIGN.md 3.9).

    Driven by the NoteSegmenter, which calls it in the binding event order of a frame
    (note-off, bend, note-on; see NoteSegmenter). residualCents is always relative to the QUANTISED note
    before the octave shift (the shift moves note and bend together).

    Shaping per mode:
    - off:      target 0 cents (a bend that is still off-centre is sent back to 0 once).
    - anchored: dead zone: c = 0 if |r| <= anchoredDeadZoneCents, else r - sign (r) * anchoredDeadZoneCents
                (continuous, no jump at the edge), then a one-pole smoother per frame with time constant
                anchoredSmoothingMs (coefficient exp (-frameInterval / tau)). noteStarted() sets the
                smoother state to the shaped target (no glide-in).
    - trueBend: c = r (1:1), no smoothing.
    Value: centsToBend (c, bendRangeSemitones (range)).

    Rate limit (binding, block-size invariant): update() emits only if the value differs from the last
    emitted value AND absoluteSample - lastEmitSample >= minIntervalSamples, where
    minIntervalSamples = max (1, lround (sampleRate / maxUpdateRateHz)) host samples. noteStarted() and
    noteEnded() are not rate-limited (they emit immediately if the value changes); every emitted event
    restarts the interval. noteEnded() sends the centre (0) if the last emitted value was not 0.
    Only absolute sample positions are compared, so the output is block-size invariant. A non-finite
    residual counts as 0 cents.

    Real-time safe, no allocation. Audio thread only. */
class BendGenerator
{
public:
    static constexpr double anchoredDeadZoneCents = 30.0;
    static constexpr double anchoredSmoothingMs = 30.0;
    static constexpr double maxUpdateRateHz = 200.0;

    /** cents -> 14-bit bend for +-rangeSemitones: x = cents / (100 * range), clamped to [-1, 1];
        value = x >= 0 ? lround (x * 8191) : lround (x * 8192). Result in [-8192, 8191]. */
    static int16_t centsToBend (double cents, int rangeSemitones) noexcept;

    /** Mode shaping without smoothing (dead zone for anchored, identity for trueBend, 0 for off). */
    static double shapeCents (BendMode mode, double residualCents) noexcept;

    /** sampleRate = host rate (rate limit), frameIntervalSeconds = hop duration (smoothing). Calls reset(). */
    void prepare (double sampleRate, double frameIntervalSeconds) noexcept;

    /** Forgets everything WITHOUT emitting (last emitted value := 0). */
    void reset() noexcept;

    /** Takes effect at the next call. Does not emit by itself. */
    void setMode (BendMode mode, BendRange range) noexcept;

    /** A note starts (from silence or on a note change): computes the bend for residualCents, resets the
        smoother and emits it if it differs from the last emitted value (not rate-limited). */
    void noteStarted (double residualCents, int64_t absoluteSample, int sampleOffset, uint8_t channel,
                      VsEventSink out) noexcept;

    /** Called on every frame while a note is held (rate-limited, see above). */
    void update (double residualCents, int64_t absoluteSample, int sampleOffset, uint8_t channel,
                 VsEventSink out) noexcept;

    /** The note ends: emits the centre if the last emitted value was not 0 (not rate-limited). */
    void noteEnded (int64_t absoluteSample, int sampleOffset, uint8_t channel, VsEventSink out) noexcept;

    /** Last emitted bend value (0 = centre). */
    int16_t getLastValue() const noexcept { return lastValue; }

private:
    /** Shaped (and for anchored: smoothed) cents for this frame; restartSmoother sets the smoother to
        the target (note start). */
    double shapedOutputCents (double residualCents, bool restartSmoother) noexcept;
    void emit (int16_t value, int64_t absoluteSample, int sampleOffset, uint8_t channel, VsEventSink out) noexcept;

    double sampleRate = 48000.0;
    double smoothingCoefficient = 0.0; ///< exp (-frameInterval / tau)
    int64_t minIntervalSamples = 240;

    BendMode mode = BendMode::off;
    int rangeSemitones = 2;

    double smoothedCents = 0.0;        ///< cents of the bend being produced (anchored: smoother state)
    int16_t lastValue = 0;
    int64_t lastEmitSample = 0;
    bool hasEmitted = false;           ///< false: the rate limit does not apply to the first update
};

} // namespace vocalsynth
