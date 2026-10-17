#include "vocalsynth/TakeRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace vocalsynth
{

namespace
{
constexpr std::size_t noIndex = std::numeric_limits<std::size_t>::max();

double takeSampleRate (const Take& take) noexcept
{
    return take.sampleRate > 0.0 && std::isfinite (take.sampleRate) ? take.sampleRate : 48000.0;
}

double freeBpmOf (const Take& take) noexcept
{
    return take.freeBpm > 0.0 && std::isfinite (take.freeBpm) ? take.freeBpm : TakeTimeline::fallbackBpm;
}

int64_t msToSamples (double ms, double sampleRate) noexcept
{
    return static_cast<int64_t> (std::llround (ms * sampleRate / 1000.0));
}

/** Tick 0 of a take: shared by render() and computeAudioSpan() (Raw, Clean, every quantize setting and the
    WAV use the same origin, docs/M3-DESIGN.md 9.1). */
struct Origin
{
    bool synced = false;
    int64_t roundTrip = 0;
    double sampleRate = 48000.0;
    // synced
    double beats = 0.0;
    int64_t bar = 0;
    // free
    int64_t freeStart = 0;   ///< s0 (capture clock)
    double freeTicksPerSample = 0.0;
    int ppq = 960;
};

/** Sample of the first note-on (lowest emit sample; synthetic ones count): refined if set, else estimated. */
bool firstNoteOnSample (const Take& take, int64_t& sample) noexcept
{
    const CapturedEvent* first = nullptr;

    for (const auto& e : take.events)
        if (e.event.type == VsEventType::noteOn && e.event.data2 > 0 && (first == nullptr || e.sample < first->sample))
            first = &e;

    if (first == nullptr)
        return false;

    sample = first->refinedSample != CapturedEvent::noSample ? first->refinedSample : first->estimatedSample;
    return true;
}

Origin computeOrigin (const Take& take, const RenderOptions& o)
{
    Origin origin;
    origin.synced = take.mode == TakeMode::synced;
    origin.roundTrip = origin.synced ? o.roundTripSamples : 0;
    origin.sampleRate = takeSampleRate (take);
    origin.ppq = o.ticksPerQuarterNote;

    int64_t first = take.startSample;
    const auto hasNote = firstNoteOnSample (take, first);

    if (origin.synced)
    {
        const auto hostSample = std::clamp (first - origin.roundTrip, take.startSample, std::max (take.startSample, take.endSample));
        const auto bar = take.timeline.barAt (static_cast<double> (hostSample));
        origin.beats = bar.startBeats;
        origin.bar = bar.number;
    }
    else
    {
        origin.freeStart = (hasNote ? first : take.startSample) - msToSamples (o.freePreRollMs, origin.sampleRate);
        origin.freeTicksPerSample = freeBpmOf (take) / 60.0 * static_cast<double> (o.ticksPerQuarterNote) / origin.sampleRate;
    }

    return origin;
}

int64_t tickAt (const Take& take, const Origin& origin, int64_t sample) noexcept
{
    double ticks = 0.0;

    if (origin.synced)
        ticks = (take.timeline.beatsAt (static_cast<double> (sample)) - origin.beats) * static_cast<double> (origin.ppq);
    else
        ticks = static_cast<double> (sample - origin.freeStart) * origin.freeTicksPerSample;

    if (! (ticks > 0.0)) // also NaN
        return 0;

    return static_cast<int64_t> (std::llround (std::min (ticks, 1.0e15)));
}

AudioSpan audioSpanFor (const Take& take, const Origin& origin)
{
    AudioSpan span;

    if (origin.synced)
    {
        const auto start = static_cast<int64_t> (std::llround (take.timeline.sampleAtBeats (origin.beats))) + origin.roundTrip;
        span.startSample = start;
        span.numSamples = std::max<int64_t> (0, take.endSample + origin.roundTrip - start);
    }
    else
    {
        span.startSample = origin.freeStart;
        span.numSamples = std::max<int64_t> (0, take.endSample - origin.freeStart);
    }

    return span;
}

//==============================================================================
enum class CtrlRole : uint8_t
{
    start,   ///< sent right before its note's note-on (moves with the note start)
    update,  ///< while its note sounds (moves with a quantized start, clamped into the note)
    release, ///< sent right after its note's note-off (sits at the note end)
    orphan   ///< no note owns it: kept at its own time
};

struct Note
{
    uint8_t channel = 1;
    uint8_t pitch = 0;
    uint8_t velocity = 100;
    int64_t start = 0;            ///< compensated sample (synced: host time; free: capture clock)
    int64_t end = 0;
    bool closed = false;
    int64_t onEmit = 0;
    int64_t offEmit = 0;
    uint8_t onFlags = 0;
    uint8_t offFlags = 0;
    bool offSynthetic = false;    ///< ended by the segmenter (take end, gap), not by a sent note-off
    std::size_t onOrder = 0;      ///< index of the note-on in take.events
    std::size_t offOrder = noIndex;
    int64_t startTick = 0;
    int64_t endTick = 0;
    int64_t startShiftTicks = 0;  ///< quantize: how far the start moved (owned updates move with it)
    bool dropped = false;
};

struct Ctrl
{
    TickEventType type = TickEventType::pitchBend;
    uint8_t channel = 1;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    int16_t bend = 0;
    int64_t time = 0;
    std::size_t order = 0;
    std::size_t owner = noIndex;
    CtrlRole role = CtrlRole::orphan;
    int64_t tick = 0;
};

double durationMs (const Note& n, double sampleRate) noexcept
{
    return static_cast<double> (n.end - n.start) * 1000.0 / sampleRate;
}

uint8_t clampChannel (uint8_t channel) noexcept
{
    return static_cast<uint8_t> (std::clamp<int> (channel, 1, 16));
}

/** Pairs note-ons with note-offs in emission order and assigns bends / CCs to the note they belong to. */
void buildNotes (const Take& take, bool useRefined, int64_t roundTrip, std::vector<Note>& notes, std::vector<Ctrl>& ctrls)
{
    std::array<std::array<std::size_t, 128>, 16> open {};
    std::array<std::size_t, 16> sounding {}; // newest started note per channel that still sounds
    std::array<std::size_t, 16> lastEnded {};
    std::array<std::vector<std::size_t>, 16> pendingStart {}; // start values waiting for their note-on

    for (auto& ch : open)
        ch.fill (noIndex);

    sounding.fill (noIndex);
    lastEnded.fill (noIndex);

    const auto& events = take.events;

    const auto timeOf = [roundTrip] (const CapturedEvent& e, bool refined) noexcept
    {
        const auto t = refined && e.refinedSample != CapturedEvent::noSample ? e.refinedSample : e.estimatedSample;
        return t - roundTrip;
    };

    std::array<std::array<std::size_t, 128>, 16> lastEndedPitch {}; // newest ended note per channel + pitch

    for (auto& ch : lastEndedPitch)
        ch.fill (noIndex);

    const auto endNote = [&] (std::size_t index, int64_t time, int64_t emit, uint8_t flags, std::size_t order, bool synthetic)
    {
        auto& n = notes[index];
        n.end = std::max (time, n.start);
        n.closed = true;
        n.offEmit = emit;
        n.offFlags = flags;
        n.offOrder = order;
        n.offSynthetic = synthetic;
        lastEndedPitch[static_cast<std::size_t> (n.channel - 1)][n.pitch] = index;
        const auto ch = static_cast<std::size_t> (n.channel - 1);
        open[ch][n.pitch] = noIndex;
        lastEnded[ch] = index;

        if (sounding[ch] == index)
        {
            // the newest other note still sounding on this channel (normally none: the voice is monophonic)
            sounding[ch] = noIndex;

            for (std::size_t p = 0; p < 128; ++p)
                if (open[ch][p] != noIndex && (sounding[ch] == noIndex || open[ch][p] > sounding[ch]))
                    sounding[ch] = open[ch][p];
        }
    };

    for (std::size_t i = 0; i < events.size(); ++i)
    {
        const auto& ce = events[i];
        const auto& e = ce.event;
        const auto channel = clampChannel (e.channel);
        const auto ch = static_cast<std::size_t> (channel - 1);

        if (e.type == VsEventType::noteOn && e.data2 > 0)
        {
            const auto pitch = static_cast<uint8_t> (std::min<int> (e.data1, 127));

            // A resync note-on (the tap re-states a note that is still on in the MIDI output after a ring
            // overflow) was never sent: the note continues. The segmenter ended it at the gap (synthetic
            // note-off at the same sample): re-open it instead of splitting it there.
            if ((ce.timing.flags & EventTimingFlags::resync) != 0)
            {
                const auto ended = lastEndedPitch[ch][pitch];

                if (open[ch][pitch] == noIndex && ended != noIndex && notes[ended].offSynthetic
                    && notes[ended].offEmit == ce.sample)
                {
                    auto& n = notes[ended];
                    n.closed = false;
                    n.end = n.start;
                    n.offOrder = noIndex;
                    n.offSynthetic = false;
                    open[ch][pitch] = ended;
                    sounding[ch] = ended;
                    lastEndedPitch[ch][pitch] = noIndex;
                    pendingStart[ch].clear();
                    continue;
                }

                if (open[ch][pitch] != noIndex)
                    continue; // still open here: nothing to re-state
            }

            if (open[ch][pitch] != noIndex) // a repeated note-on ends the earlier note (MidiOutputStage rule)
                endNote (open[ch][pitch], timeOf (ce, false), ce.sample, ce.timing.flags, i, false);

            Note n;
            n.channel = channel;
            n.pitch = pitch;
            n.velocity = static_cast<uint8_t> (std::clamp<int> (e.data2, 1, 127));
            n.start = timeOf (ce, useRefined);
            n.end = n.start;
            n.onEmit = ce.sample;
            n.onFlags = ce.timing.flags;
            n.onOrder = i;
            notes.push_back (n);
            open[ch][pitch] = notes.size() - 1;
            sounding[ch] = notes.size() - 1;

            for (const auto c : pendingStart[ch])
                ctrls[c].owner = notes.size() - 1;

            pendingStart[ch].clear();
            continue;
        }

        if (e.type == VsEventType::noteOff || e.type == VsEventType::noteOn) // velocity 0 = note-off
        {
            const auto pitch = static_cast<uint8_t> (std::min<int> (e.data1, 127));

            if (open[ch][pitch] != noIndex) // unpaired note-offs are dropped
                endNote (open[ch][pitch], timeOf (ce, false), ce.sample, ce.timing.flags, i, ce.synthetic);

            continue;
        }

        Ctrl c;
        c.type = e.type == VsEventType::pitchBend ? TickEventType::pitchBend : TickEventType::controlChange;
        c.channel = channel;
        c.data1 = static_cast<uint8_t> (std::min<int> (e.data1, 127));
        c.data2 = static_cast<uint8_t> (std::min<int> (e.data2, 127));
        c.bend = static_cast<int16_t> (std::clamp<int> (e.bend, -8192, 8191));
        c.time = timeOf (ce, false);
        c.order = i;

        // Sent right before a note-on of the same channel at the same sample: that note's start value
        auto startsNote = false;

        for (auto j = i + 1; j < events.size() && events[j].sample == ce.sample; ++j)
        {
            const auto& next = events[j].event;

            if (clampChannel (next.channel) != channel)
                continue;

            if (next.type == VsEventType::noteOn && next.data2 > 0)
                startsNote = true;

            if (next.type != VsEventType::noteOff)
                break;
        }

        if (startsNote)
        {
            c.role = CtrlRole::start; // owner: the note the coming note-on creates
            pendingStart[ch].push_back (ctrls.size());
        }
        else if (sounding[ch] != noIndex)
        {
            c.role = CtrlRole::update;
            c.owner = sounding[ch];
        }
        else if (lastEnded[ch] != noIndex && notes[lastEnded[ch]].offEmit == ce.sample)
        {
            c.role = CtrlRole::release;
            c.owner = lastEnded[ch];
        }

        ctrls.push_back (c);
    }

    // Notes still sounding at the end of the take end there
    const auto takeEnd = take.mode == TakeMode::synced ? take.endSample : take.captureEndSample - roundTrip;

    for (auto& n : notes)
    {
        if (! n.closed)
        {
            n.end = std::max (n.start, takeEnd);
            n.offEmit = std::numeric_limits<int64_t>::max();
            n.offOrder = noIndex;
        }
    }

    // A start value whose note-on never came (cannot happen: the scan saw it) keeps its own time
    for (auto& c : ctrls)
        if (c.role == CtrlRole::start && c.owner == noIndex)
            c.role = CtrlRole::orphan;
}

void dropNote (std::vector<Note>& notes, std::size_t index)
{
    notes[index].dropped = true;
}

/** Clean, first stage (docs/M3-SPEC.md 7, TakeRenderer class comment). Works on compensated samples. */
void applyClean (std::vector<Note>& notes, const Take& take, const RenderOptions& o)
{
    const auto sr = takeSampleRate (take);

    // 1. Provisional corrections: the corrected note takes the provisional start, the provisional is dropped
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        auto& corrected = notes[i];

        if (corrected.dropped || (corrected.onFlags & EventTimingFlags::correction) == 0)
            continue;

        for (std::size_t j = 0; j < notes.size(); ++j)
        {
            auto& provisional = notes[j];

            if (j == i || provisional.dropped || provisional.channel != corrected.channel || ! provisional.closed
                || provisional.offEmit != corrected.onEmit || (provisional.offFlags & EventTimingFlags::correction) == 0
                || provisional.onOrder > corrected.onOrder)
                continue;

            corrected.start = std::min (corrected.start, provisional.start);
            dropNote (notes, j);
            break;
        }
    }

    // Neighbours in time order (the voice is monophonic)
    const auto ordered = [&notes]
    {
        std::vector<std::size_t> idx;

        for (std::size_t i = 0; i < notes.size(); ++i)
            if (! notes[i].dropped)
                idx.push_back (i);

        std::stable_sort (idx.begin(), idx.end(), [&notes] (std::size_t a, std::size_t b)
                          { return notes[a].start < notes[b].start || (notes[a].start == notes[b].start && notes[a].onOrder < notes[b].onOrder); });
        return idx;
    };

    // 2. Transition notes: short, between two notes (legato or a gap <= 15 ms), pitch between them ->
    //    their time goes to the following note
    {
        const auto idx = ordered();
        const auto maxGap = msToSamples (TakeRenderer::cleanTransitionMaxGapMs, sr);

        for (std::size_t k = 1; k + 1 < idx.size(); ++k)
        {
            auto& prev = notes[idx[k - 1]];
            auto& t = notes[idx[k]];
            auto& next = notes[idx[k + 1]];

            if (prev.dropped || t.dropped || next.dropped || prev.channel != t.channel || next.channel != t.channel)
                continue;

            const auto lo = std::min (prev.pitch, next.pitch);
            const auto hi = std::max (prev.pitch, next.pitch);

            if (durationMs (t, sr) < TakeRenderer::cleanTransitionMaxMs && t.start - prev.end <= maxGap
                && next.start - t.end <= maxGap && t.pitch > lo && t.pitch < hi)
            {
                next.start = std::min (next.start, t.start);
                dropNote (notes, idx[k]);
            }
        }
    }

    // 3. Ghost notes
    {
        const auto minMs = std::max (o.cleanMinNoteMs, static_cast<double> (take.settings.minNoteLengthMs));

        for (std::size_t i = 0; i < notes.size(); ++i)
            if (! notes[i].dropped && durationMs (notes[i], sr) < minMs)
                dropNote (notes, i);
    }

    // 4. Octave strays: short and >= 11 semitones from both neighbours
    {
        const auto idx = ordered();
        std::vector<std::size_t> strays;

        for (std::size_t k = 1; k + 1 < idx.size(); ++k)
        {
            const auto& prev = notes[idx[k - 1]];
            const auto& s = notes[idx[k]];
            const auto& next = notes[idx[k + 1]];

            if (durationMs (s, sr) < TakeRenderer::cleanOctaveStrayMaxMs
                && std::abs (static_cast<int> (s.pitch) - static_cast<int> (prev.pitch)) >= TakeRenderer::cleanOctaveStraySemitones
                && std::abs (static_cast<int> (s.pitch) - static_cast<int> (next.pitch)) >= TakeRenderer::cleanOctaveStraySemitones)
                strays.push_back (idx[k]);
        }

        for (const auto i : strays)
            dropNote (notes, i);
    }
}

/** Nearest multiple of grid (ties away from zero, like llround). */
int64_t nearestGridLine (int64_t tick, int64_t grid) noexcept
{
    const auto q = tick / grid;
    const auto r = tick - q * grid;
    return 2 * r >= grid ? (q + 1) * grid : q * grid;
}

void applyQuantize (std::vector<Note>& notes, const RenderOptions& o)
{
    const auto grid = quantizeGridTicks (o.quantize, o.ticksPerQuarterNote);

    if (grid <= 0)
        return;

    const auto strength = o.quantizeStrengthPercent / 100.0;
    const auto halfGrid = (grid + 1) / 2;

    for (auto& n : notes)
    {
        if (n.dropped)
            continue;

        const auto length = n.endTick - n.startTick;
        const auto target = nearestGridLine (n.startTick, grid);
        const auto start = n.startTick + static_cast<int64_t> (std::llround (strength * static_cast<double> (target - n.startTick)));
        n.startShiftTicks = start - n.startTick;
        n.startTick = start;

        if (o.quantizeEnds)
        {
            auto end = nearestGridLine (n.endTick, grid);

            if (end - start < halfGrid)
            {
                // the first grid line at least half a step after the start
                const auto minEnd = start + halfGrid;
                end = (minEnd + grid - 1) / grid * grid;
            }

            n.endTick = end;
        }
        else if (n.endTick <= start)
        {
            n.endTick = start + std::max<int64_t> (1, length); // moved past its end: keeps its length
        }
    }
}

/** Same pitch + channel never overlaps (the earlier note ends at the later start; a note left without
    length merges into the later one), every note at least one tick long. */
void applyPairing (std::vector<Note>& notes)
{
    std::vector<std::size_t> idx;

    for (std::size_t i = 0; i < notes.size(); ++i)
        if (! notes[i].dropped)
            idx.push_back (i);

    std::stable_sort (idx.begin(), idx.end(), [&notes] (std::size_t a, std::size_t b)
    {
        const auto& x = notes[a];
        const auto& y = notes[b];

        if (x.channel != y.channel)
            return x.channel < y.channel;

        if (x.pitch != y.pitch)
            return x.pitch < y.pitch;

        return x.startTick < y.startTick || (x.startTick == y.startTick && x.onOrder < y.onOrder);
    });

    for (std::size_t k = 0; k + 1 < idx.size(); ++k)
    {
        auto& a = notes[idx[k]];
        auto& b = notes[idx[k + 1]];

        if (a.channel != b.channel || a.pitch != b.pitch || a.endTick <= b.startTick)
            continue;

        if (b.startTick - a.startTick < 1)
        {
            b.endTick = std::max (a.endTick, b.endTick);
            b.startTick = a.startTick;
            a.dropped = true;
            continue;
        }

        a.endTick = b.startTick;
    }

    for (auto& n : notes)
        if (! n.dropped)
            n.endTick = std::max (n.endTick, n.startTick + 1);
}

int typeRank (TickEventType type) noexcept
{
    return static_cast<int> (type);
}

/** First bar line at or after tick (bars counted from tick 0 with the time-signature map; a signature
    change starts a bar at its own tick), at least one bar. */
int64_t barLineAtOrAfter (const std::vector<TickTimeSignature>& signatures, int ppq, int64_t tick)
{
    const auto barTicks = [ppq] (const TickTimeSignature& ts)
    {
        const auto beats = static_cast<double> (std::max (1, ts.numerator)) * 4.0 / static_cast<double> (std::max (1, ts.denominator));
        return std::max<int64_t> (1, static_cast<int64_t> (std::llround (beats * static_cast<double> (ppq))));
    };

    for (std::size_t i = 0; i < signatures.size(); ++i)
    {
        const auto last = i + 1 == signatures.size();
        const auto from = signatures[i].tick;
        const auto to = last ? std::numeric_limits<int64_t>::max() : signatures[i + 1].tick;

        if (! last && tick > to)
            continue;

        const auto bar = barTicks (signatures[i]);
        const auto k = std::max<int64_t> (1, (tick - from + bar - 1) / bar);
        const auto line = from + k * bar;
        return last ? line : std::min (line, to);
    }

    return std::max<int64_t> (tick, static_cast<int64_t> (4) * ppq);
}
} // namespace

RenderedTake TakeRenderer::render (const Take& take, const RenderOptions& options)
{
    const auto o = sanitise (options);
    const auto origin = computeOrigin (take, o);
    const auto clean = o.version == ExportVersion::clean && ! take.settings.testPattern; // the pattern is exact

    RenderedTake r;
    r.ticksPerQuarterNote = o.ticksPerQuarterNote;
    r.originBar = origin.synced ? origin.bar : 0;
    r.originBeats = origin.synced ? origin.beats : 0.0;
    r.audio = audioSpanFor (take, origin);

    // --- 1. compensation ------------------------------------------------------------------------------
    std::vector<Note> notes;
    std::vector<Ctrl> ctrls;
    notes.reserve (take.events.size() / 4 + 4);
    ctrls.reserve (take.events.size());
    buildNotes (take, clean, origin.roundTrip, notes, ctrls);

    if (origin.synced)
    {
        // Clamp into the host-time span: notes sung before the transport start or after Stop / the split
        // (they arrived in the capture tail) are dropped or cut.
        const auto lo = take.startSample;
        const auto hi = std::max (take.startSample, take.endSample);

        for (auto& n : notes)
        {
            if (n.end <= lo || n.start >= hi)
            {
                n.dropped = true;
                continue;
            }

            n.start = std::clamp (n.start, lo, hi);
            n.end = std::clamp (n.end, lo, hi);
        }

        for (auto& c : ctrls)
            c.time = std::clamp (c.time, lo, hi);
    }

    // --- 2. version -----------------------------------------------------------------------------------
    if (clean)
        applyClean (notes, take, o);

    for (auto& n : notes)
    {
        n.startTick = tickAt (take, origin, n.start);
        n.endTick = std::max (n.startTick, tickAt (take, origin, n.end));
    }

    // --- 3. quantize (synced only) --------------------------------------------------------------------
    if (origin.synced && o.quantize != QuantizeGrid::off)
        applyQuantize (notes, o);

    // --- 4. pairing / clamping ------------------------------------------------------------------------
    applyPairing (notes);

    const auto exportBends = bendsEnabled (o.bends, take.settings.bendMode);
    r.bendRangeSemitones = exportBends ? std::max (1, take.settings.bendRangeSemitones) : 0;

    struct Sortable
    {
        TickEvent event;
        std::size_t order = 0;
    };

    std::vector<Sortable> out;
    out.reserve (notes.size() * 2 + ctrls.size() + 16);

    for (const auto& n : notes)
    {
        if (n.dropped)
            continue;

        out.push_back ({ TickEvent { n.startTick, TickEventType::noteOn, n.channel, n.pitch, n.velocity, 0 }, n.onOrder });
        out.push_back ({ TickEvent { n.endTick, TickEventType::noteOff, n.channel, n.pitch, 0, 0 }, n.offOrder });
    }

    for (const auto& c : ctrls)
    {
        if (c.type == TickEventType::pitchBend ? ! exportBends : ! o.controlChanges)
            continue;

        int64_t tick = 0;

        if (c.owner != noIndex)
        {
            const auto& n = notes[c.owner];

            if (n.dropped)
                continue; // removed with its note

            switch (c.role)
            {
                case CtrlRole::start: tick = n.startTick; break;
                case CtrlRole::release: tick = n.endTick; break;
                case CtrlRole::update:
                case CtrlRole::orphan:
                default:
                    tick = std::clamp (tickAt (take, origin, c.time) + n.startShiftTicks, n.startTick, n.endTick);
                    break;
            }
        }
        else
        {
            tick = tickAt (take, origin, c.time);
        }

        out.push_back ({ TickEvent { tick, c.type, c.channel, c.data1, c.data2, c.bend }, c.order });
    }

    const auto sortEvents = [&out]
    {
        std::stable_sort (out.begin(), out.end(), [] (const Sortable& a, const Sortable& b)
        {
            if (a.event.tick != b.event.tick)
                return a.event.tick < b.event.tick;

            if (a.event.type != b.event.type)
                return typeRank (a.event.type) < typeRank (b.event.type);

            return a.order < b.order;
        });
    };

    sortEvents();

    // Bends back to centre after the last note (per channel whose last bend is not 0)
    if (exportBends)
    {
        std::array<int16_t, 16> lastBend {};
        std::array<int64_t, 16> lastTick {};
        std::array<bool, 16> used {};

        for (const auto& s : out)
        {
            const auto ch = static_cast<std::size_t> (s.event.channel - 1);
            used[ch] = true;
            lastTick[ch] = std::max (lastTick[ch], s.event.tick);

            if (s.event.type == TickEventType::pitchBend)
                lastBend[ch] = s.event.bend;
        }

        auto added = false;

        for (std::size_t ch = 0; ch < 16; ++ch)
        {
            if (used[ch] && lastBend[ch] != 0)
            {
                out.push_back ({ TickEvent { lastTick[ch], TickEventType::pitchBend, static_cast<uint8_t> (ch + 1), 0, 0, 0 }, noIndex });
                added = true;
            }
        }

        if (added)
            sortEvents();
    }

    r.events.reserve (out.size());

    for (const auto& s : out)
    {
        r.events.push_back (s.event);

        if (s.event.type == TickEventType::noteOn)
            ++r.noteCount;
    }

    const auto lastEventTick = r.events.empty() ? int64_t { 0 } : r.events.back().tick;

    // --- tempo / time-signature maps and the end of the track ----------------------------------------
    if (origin.synced)
    {
        const auto endBeats = std::max (origin.beats, take.timeline.beatsAt (static_cast<double> (std::max (take.endSample, take.startSample))));
        const auto toTick = [&] (double beats)
        {
            const auto t = (beats - origin.beats) * static_cast<double> (o.ticksPerQuarterNote);
            return t > 0.0 ? static_cast<int64_t> (std::llround (t)) : int64_t { 0 };
        };

        for (const auto& p : take.timeline.tempoMap (origin.beats, endBeats))
        {
            const auto tick = toTick (p.beats);
            const auto bpm = p.bpm > 0.0 && std::isfinite (p.bpm) ? p.bpm : TakeTimeline::fallbackBpm;

            if (! r.tempos.empty() && r.tempos.back().tick == tick)
                r.tempos.back().bpm = bpm;
            else if (r.tempos.empty() || r.tempos.back().bpm != bpm)
                r.tempos.push_back ({ tick, bpm });
        }

        for (const auto& p : take.timeline.timeSignatureMap (origin.beats, endBeats))
        {
            const auto tick = toTick (p.beats);
            const TickTimeSignature ts { tick, std::max (1, p.numerator), std::max (1, p.denominator) };

            if (! r.timeSignatures.empty() && r.timeSignatures.back().tick == tick)
                r.timeSignatures.back() = ts;
            else if (r.timeSignatures.empty() || r.timeSignatures.back().numerator != ts.numerator
                     || r.timeSignatures.back().denominator != ts.denominator)
                r.timeSignatures.push_back (ts);
        }
    }

    if (r.tempos.empty() || r.tempos.front().tick != 0)
        r.tempos.insert (r.tempos.begin(), TickTempo { 0, origin.synced ? take.timeline.bpmAt (static_cast<double> (take.startSample)) : freeBpmOf (take) });

    if (r.timeSignatures.empty() || r.timeSignatures.front().tick != 0)
        r.timeSignatures.insert (r.timeSignatures.begin(), TickTimeSignature { 0, 4, 4 });

    if (origin.synced)
    {
        r.endTick = barLineAtOrAfter (r.timeSignatures, o.ticksPerQuarterNote, lastEventTick);

        // Meta changes at or after the end of the track are meaningless
        const auto beforeEnd = [&r] (int64_t tick) { return tick == 0 || tick < r.endTick; };
        std::erase_if (r.tempos, [&] (const TickTempo& t) { return ! beforeEnd (t.tick); });
        std::erase_if (r.timeSignatures, [&] (const TickTimeSignature& t) { return ! beforeEnd (t.tick); });
    }
    else
    {
        r.endTick = std::max<int64_t> (1, lastEventTick);
    }

    return r;
}

int64_t TakeRenderer::roundTripFor (const Take& take, const CaptureOffsetSettings& offset) noexcept
{
    if (take.mode != TakeMode::synced)
        return 0;

    return computeRoundTripSamples (offset, take.maxBlockSize, take.sampleRate);
}

AudioSpan TakeRenderer::computeAudioSpan (const Take& take, const RenderOptions& options)
{
    const auto o = sanitise (options);
    return audioSpanFor (take, computeOrigin (take, o));
}

void TakeRenderer::refineNoteOnsets (Take& take, std::span<const float> audio, int64_t audioStartSample)
{
    const auto sr = takeSampleRate (take);
    const auto quietRun = std::max<int64_t> (2, msToSamples (refineQuietMs, sr));
    const auto window = msToSamples (onsetRefineWindowMs, sr);
    const auto audioEnd = audioStartSample + static_cast<int64_t> (audio.size());
    const auto quietFactor = std::pow (10.0, -refineQuietDb / 20.0);
    const auto at = [&] (int64_t s) noexcept
    {
        return std::abs (static_cast<double> (audio[static_cast<std::size_t> (s - audioStartSample)]));
    };

    for (auto& ce : take.events)
    {
        ce.refinedSample = CapturedEvent::noSample;

        if (ce.event.type != VsEventType::noteOn || ce.event.data2 == 0 || ce.synthetic
            || (ce.timing.kind != EventTimingKind::measured && ce.timing.kind != EventTimingKind::model)
            || (ce.timing.flags & (EventTimingFlags::resync | EventTimingFlags::correction | EventTimingFlags::noteChange)) != 0)
            continue;

        // The onset must lie in [estimate - window, emit]; the quiet run before it may start earlier
        const auto earliest = std::min (ce.estimatedSample, ce.sample) - window;
        const auto from = earliest - quietRun;
        const auto to = ce.sample;

        if (from < audioStartSample || to > audioEnd || to - earliest < 2)
            continue;

        auto peak = 0.0;

        for (auto s = std::max (from, to - quietRun); s < to; ++s)
            peak = std::max (peak, at (s));

        if (! (peak > 0.0) || ! std::isfinite (peak))
            continue;

        // Newest run of quietRun samples below the threshold, scanning back from the emit sample: the onset
        // is the loud sample right after it
        const auto threshold = peak * quietFactor;
        int64_t run = 0;
        int64_t onset = -1;

        for (auto s = to - 1; s >= from; --s)
        {
            if (at (s) >= threshold)
            {
                run = 0;
                continue;
            }

            if (++run >= quietRun)
            {
                onset = s + run; // first sample after the run
                break;
            }
        }

        if (onset < 0 || onset >= to)
            continue; // no pause before the emit sample: no clear onset

        // Back to where the signal left the noise floor of the run (a tone that starts at a zero crossing
        // passes the threshold a few samples after its start), at most half the run
        double floorPower = 0.0;
        const auto floorFrom = onset - quietRun;
        const auto floorTo = onset - quietRun / 2;

        for (auto s = floorFrom; s < floorTo; ++s)
            floorPower += at (s) * at (s);

        const auto floorAmplitude = 3.0 * std::sqrt (floorPower / static_cast<double> (std::max<int64_t> (1, floorTo - floorFrom))) + 1.0e-7;
        const auto limit = onset - quietRun / 2;

        while (onset > limit && at (onset - 1) > floorAmplitude)
            --onset;

        if (onset >= earliest && onset <= ce.sample)
            ce.refinedSample = onset;
    }
}

} // namespace vocalsynth
