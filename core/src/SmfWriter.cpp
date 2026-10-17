#include "vocalsynth/SmfWriter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace vocalsynth
{

namespace
{
constexpr uint32_t maxVariableLength = 0x0FFFFFFFu;

void appendU16 (std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back (static_cast<uint8_t> ((v >> 8) & 0xffu));
    out.push_back (static_cast<uint8_t> (v & 0xffu));
}

void appendU32 (std::vector<uint8_t>& out, uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back (static_cast<uint8_t> ((v >> shift) & 0xffu));
}

uint8_t channelNibble (uint8_t channel) noexcept
{
    return static_cast<uint8_t> (std::clamp<int> (channel, 1, 16) - 1);
}

uint8_t data7 (int value) noexcept
{
    return static_cast<uint8_t> (std::clamp (value, 0, 127));
}

uint32_t tempoMicroseconds (double bpm) noexcept
{
    if (! std::isfinite (bpm) || ! (bpm > 0.0))
        return 500000u; // 120 BPM

    const auto us = std::llround (std::min (60.0e6 / bpm, 1.0e9));
    return static_cast<uint32_t> (std::clamp<long long> (us, 1, 0xFFFFFF));
}

/** log2 of the power of two nearest to denominator, clamped to 0..7 (1..128). */
uint8_t denominatorPower (int denominator) noexcept
{
    const auto d = std::clamp (denominator, 1, 128);
    auto best = 0;
    auto bestDistance = std::numeric_limits<int>::max();

    for (int p = 0; p <= 7; ++p)
    {
        const auto distance = std::abs ((1 << p) - d);

        if (distance < bestDistance) // ties go to the smaller power (3 -> 2, 6 -> 4, 12 -> 8)
        {
            best = p;
            bestDistance = distance;
        }
    }

    return static_cast<uint8_t> (best);
}

class TrackWriter
{
public:
    explicit TrackWriter (std::vector<uint8_t>& target) : out (target) {}

    void event (int64_t tick, std::initializer_list<uint8_t> bytes)
    {
        delta (tick);
        out.insert (out.end(), bytes.begin(), bytes.end());
    }

    void delta (int64_t tick)
    {
        const auto t = std::max (tick, lastTick);
        SmfWriter::appendVariableLength (out, static_cast<uint32_t> (std::min<int64_t> (t - lastTick, maxVariableLength)));
        lastTick = t;
    }

private:
    std::vector<uint8_t>& out;
    int64_t lastTick = 0;
};

void appendTempo (TrackWriter& w, int64_t tick, double bpm)
{
    const auto us = tempoMicroseconds (bpm);
    w.event (tick, { 0xFF, 0x51, 0x03, static_cast<uint8_t> ((us >> 16) & 0xffu), static_cast<uint8_t> ((us >> 8) & 0xffu),
                     static_cast<uint8_t> (us & 0xffu) });
}

void appendTimeSignature (TrackWriter& w, int64_t tick, int numerator, int denominator)
{
    w.event (tick, { 0xFF, 0x58, 0x04, static_cast<uint8_t> (std::clamp (numerator, 1, 255)), denominatorPower (denominator),
                     0x18, 0x08 });
}
} // namespace

void SmfWriter::appendVariableLength (std::vector<uint8_t>& out, uint32_t value)
{
    const auto v = std::min (value, maxVariableLength);
    auto groups = 1;

    while (groups < 4 && (v >> (7 * groups)) != 0)
        ++groups;

    for (int i = groups - 1; i >= 0; --i)
        out.push_back (static_cast<uint8_t> (((v >> (7 * i)) & 0x7fu) | (i > 0 ? 0x80u : 0x00u)));
}

std::vector<uint8_t> SmfWriter::write (const RenderedTake& take, const SmfOptions& options)
{
    std::vector<uint8_t> track;
    track.reserve (64 + take.events.size() * 5);
    TrackWriter w (track);

    // --- tick 0, M1 order: track name, tempo, time signature, RPN 0 ---------------------------------
    if (! options.trackName.empty())
    {
        w.delta (0);
        track.push_back (0xFF);
        track.push_back (0x03);
        appendVariableLength (track, static_cast<uint32_t> (std::min<std::size_t> (options.trackName.size(), maxVariableLength)));

        for (const auto c : options.trackName)
        {
            const auto u = static_cast<unsigned char> (c);
            track.push_back (u >= 0x20 && u < 0x7f ? static_cast<uint8_t> (u) : static_cast<uint8_t> ('?'));
        }
    }

    const auto firstTempo = take.tempos.empty() ? TakeTimeline::fallbackBpm : take.tempos.front().bpm;
    appendTempo (w, 0, firstTempo);

    const auto firstSignature = take.timeSignatures.empty() ? TickTimeSignature {} : take.timeSignatures.front();
    appendTimeSignature (w, 0, firstSignature.numerator, firstSignature.denominator);

    if (take.bendRangeSemitones > 0)
    {
        std::array<bool, 16> used {};

        for (const auto& e : take.events)
            used[channelNibble (e.channel)] = true;

        const auto range = data7 (take.bendRangeSemitones);

        for (uint8_t ch = 0; ch < 16; ++ch)
        {
            if (! used[ch])
                continue;

            const auto status = static_cast<uint8_t> (0xB0u | ch);
            w.event (0, { status, 0x65, 0x00 });
            w.event (0, { status, 0x64, 0x00 });
            w.event (0, { status, 0x06, range });
            w.event (0, { status, 0x26, 0x00 });
            w.event (0, { status, 0x65, 0x7F });
            w.event (0, { status, 0x64, 0x7F });
        }
    }

    // --- remaining meta changes and channel events, merged by tick (meta first) ----------------------
    std::size_t tempoIndex = take.tempos.empty() ? 0 : 1;
    std::size_t signatureIndex = take.timeSignatures.empty() ? 0 : 1;
    std::size_t eventIndex = 0;
    int64_t lastTick = 0;

    while (tempoIndex < take.tempos.size() || signatureIndex < take.timeSignatures.size() || eventIndex < take.events.size())
    {
        constexpr auto none = std::numeric_limits<int64_t>::max();
        const auto tempoTick = tempoIndex < take.tempos.size() ? take.tempos[tempoIndex].tick : none;
        const auto signatureTick = signatureIndex < take.timeSignatures.size() ? take.timeSignatures[signatureIndex].tick : none;
        const auto eventTick = eventIndex < take.events.size() ? take.events[eventIndex].tick : none;

        if (tempoTick <= signatureTick && tempoTick <= eventTick)
        {
            appendTempo (w, std::max (tempoTick, lastTick), take.tempos[tempoIndex].bpm);
            lastTick = std::max (tempoTick, lastTick);
            ++tempoIndex;
            continue;
        }

        if (signatureTick <= eventTick)
        {
            const auto& ts = take.timeSignatures[signatureIndex];
            appendTimeSignature (w, std::max (signatureTick, lastTick), ts.numerator, ts.denominator);
            lastTick = std::max (signatureTick, lastTick);
            ++signatureIndex;
            continue;
        }

        const auto& e = take.events[eventIndex++];
        const auto tick = std::max (e.tick, lastTick);
        const auto ch = channelNibble (e.channel);
        lastTick = tick;

        switch (e.type)
        {
            case TickEventType::noteOn:
                w.event (tick, { static_cast<uint8_t> (0x90u | ch), data7 (e.data1), data7 (std::max<int> (1, e.data2)) });
                break;

            case TickEventType::noteOff:
                w.event (tick, { static_cast<uint8_t> (0x80u | ch), data7 (e.data1), 0x00 });
                break;

            case TickEventType::controlChange:
                w.event (tick, { static_cast<uint8_t> (0xB0u | ch), data7 (e.data1), data7 (e.data2) });
                break;

            case TickEventType::pitchBend:
            {
                const auto value = static_cast<uint32_t> (std::clamp<int> (e.bend, -8192, 8191) + 8192);
                w.event (tick, { static_cast<uint8_t> (0xE0u | ch), static_cast<uint8_t> (value & 0x7fu),
                                 static_cast<uint8_t> ((value >> 7) & 0x7fu) });
                break;
            }
        }
    }

    w.event (std::max (take.endTick, lastTick), { 0xFF, 0x2F, 0x00 });

    // --- file -----------------------------------------------------------------------------------------
    std::vector<uint8_t> file;
    file.reserve (22 + track.size());
    file.insert (file.end(), { 'M', 'T', 'h', 'd' });
    appendU32 (file, 6);
    appendU16 (file, options.format == SmfFormat::type0 ? 0u : 1u);
    appendU16 (file, 1);
    appendU16 (file, static_cast<uint32_t> (std::clamp (take.ticksPerQuarterNote, 1, 0x7FFF)));
    file.insert (file.end(), { 'M', 'T', 'r', 'k' });
    appendU32 (file, static_cast<uint32_t> (track.size()));
    file.insert (file.end(), track.begin(), track.end());
    return file;
}

} // namespace vocalsynth
