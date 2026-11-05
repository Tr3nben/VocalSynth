#include "vocalsynth/synth/VoiceAllocator.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth::synth
{

// Note -> voice rules of docs/SYNTH-DESIGN.md 10.1 / 10.2 (binding text in the header).
//
// Command budget: a group never produces more than VoiceCommandList::capacity commands. Every note-off
// yields at most one command, every note-on exactly one, Mono / Legato add at most one deferred release.
// A note-on is only accepted while there is room for its command AND for a release of every note-off still
// to come in the group, so a release is never lost (nothing hangs); a dropped note-on changes no state, and
// its note-off is then an unknown note-off (ignored).

namespace
{

// Pairing runs for groups of up to 32 events (a group is 1..3 events in practice): then even with every
// correction falling through (2 commands per event) the group fits the command list. Larger groups are
// processed without pairing (every note-on a start, every note-off a release).
constexpr std::size_t maxPairedEvents = VoiceCommandList::capacity / 2;

int countNoteOffs (std::span<const NoteInput> group, std::size_t from) noexcept
{
    int n = 0;

    for (auto i = from; i < group.size(); ++i)
        n += group[i].isNoteOn ? 0 : 1;

    return n;
}

bool hasRoom (const VoiceCommandList& out, int commands, int remainingNoteOffs) noexcept
{
    return out.size() + static_cast<std::size_t> (commands + remainingNoteOffs) <= VoiceCommandList::capacity;
}

} // namespace

void VoiceAllocator::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    correctionWindowSamples = static_cast<int64_t> (std::floor (correctionWindowSeconds * sampleRate));
    reset();
}

void VoiceAllocator::reset() noexcept
{
    voiceNote.fill (-1);
    voiceStart.fill (0);
    voiceRelease.fill (-1);
    voiceOrder.fill (0);
    stack.fill (0);
    stackVelocity.fill (0);
    held.fill (false);
    orderCounter = 0;
    lastSample = 0;
    stackSize = 0;
    lastNote = -1;
    newestVoice = -1;
}

void VoiceAllocator::stackRemove (int note) noexcept
{
    int w = 0;

    for (int r = 0; r < stackSize; ++r)
    {
        if (stack[static_cast<std::size_t> (r)] != note)
        {
            stack[static_cast<std::size_t> (w)] = stack[static_cast<std::size_t> (r)];
            stackVelocity[static_cast<std::size_t> (w)] = stackVelocity[static_cast<std::size_t> (r)];
            ++w;
        }
    }

    stackSize = w;
}

void VoiceAllocator::stackPush (int note, uint8_t velocity) noexcept
{
    stackRemove (note);

    if (stackSize >= maxHeldNotes)
    {
        // Full: the oldest held note drops out of the priority list (it is not the sounding one)
        std::move (stack.begin() + 1, stack.end(), stack.begin());
        std::move (stackVelocity.begin() + 1, stackVelocity.end(), stackVelocity.begin());
        --stackSize;
    }

    stack[static_cast<std::size_t> (stackSize)] = static_cast<uint8_t> (note);
    stackVelocity[static_cast<std::size_t> (stackSize)] = velocity;
    ++stackSize;
}

bool VoiceAllocator::isCorrection (const NoteInput& noteOn, int voice, int64_t absoluteSample) const noexcept
{
    return noteOn.correction
           || absoluteSample - voiceStart[static_cast<std::size_t> (voice)] <= correctionWindowSamples;
}

int VoiceAllocator::findHeldVoice (int note) const noexcept
{
    for (int v = 0; v < maxVoices; ++v)
        if (voiceNote[static_cast<std::size_t> (v)] == note && voiceRelease[static_cast<std::size_t> (v)] < 0)
            return v;

    return -1;
}

void VoiceAllocator::assign (int voice, int note, int64_t absoluteSample, bool keepStart) noexcept
{
    const auto v = static_cast<std::size_t> (voice);
    voiceNote[v] = static_cast<int16_t> (note);
    voiceRelease[v] = -1;

    if (! keepStart)
        voiceStart[v] = absoluteSample;

    voiceOrder[v] = ++orderCounter;
    newestVoice = voice;
}

void VoiceAllocator::markReleased (int voice, int64_t absoluteSample) noexcept
{
    voiceRelease[static_cast<std::size_t> (voice)] = absoluteSample;
}

void VoiceAllocator::processGroup (std::span<const NoteInput> group, int64_t absoluteSample,
                                   const AllocatorSettings& settings, const std::array<bool, maxVoices>& voiceActive,
                                   VoiceCommandList& out) noexcept
{
    // A voice that finished its release (or was reset by the engine) no longer has a note
    for (std::size_t v = 0; v < static_cast<std::size_t> (maxVoices); ++v)
    {
        if (! voiceActive[v])
        {
            voiceNote[v] = -1;
            voiceRelease[v] = -1;
        }
    }

    if (group.empty())
        return;

    lastSample = absoluteSample;

    if (settings.playMode == PlayMode::poly)
        processPoly (group, absoluteSample, settings, voiceActive, out);
    else
        processMono (group, absoluteSample, settings, voiceActive, out);
}

void VoiceAllocator::processMono (std::span<const NoteInput> group, int64_t t, const AllocatorSettings& settings,
                                  const std::array<bool, maxVoices>&, VoiceCommandList& out) noexcept
{
    const bool legatoMode = settings.playMode == PlayMode::legato;
    bool deferredRelease = false;
    int remainingOffs = countNoteOffs (group, 0);

    for (const auto& e : group)
    {
        const int n = e.note & 0x7f;

        if (! e.isNoteOn)
        {
            --remainingOffs;

            if (! held[static_cast<std::size_t> (n)])
                continue; // unknown note-off

            held[static_cast<std::size_t> (n)] = false;
            stackRemove (n);

            if (voiceNote[0] != n || voiceRelease[0] >= 0)
                continue; // a held note that is not sounding

            if (stackSize > 0)
            {
                // Back to the newest held note: a connected change, never a correction
                const auto top = static_cast<std::size_t> (stackSize - 1);
                VoiceCommand c;
                c.type = legatoMode ? VoiceCommandType::legato : VoiceCommandType::start;
                c.voice = 0;
                c.note = stack[top];
                c.velocity = stackVelocity[top];
                c.glide = settings.glideEnabled;
                out.push (c);
                assign (0, stack[top], t, false);
            }
            else
            {
                deferredRelease = true; // a note-on at this sample makes it a connected change
            }

            continue;
        }

        // Room for this command, a release per remaining note-off and the deferred release
        if (! hasRoom (out, 2, remainingOffs))
            continue;

        const bool connected = voiceNote[0] >= 0 && voiceRelease[0] < 0; // incl. the deferred case
        held[static_cast<std::size_t> (n)] = true;
        stackPush (n, e.velocity);

        VoiceCommand c;
        c.voice = 0;
        c.note = static_cast<uint8_t> (n);
        c.velocity = e.velocity;

        if (connected)
        {
            deferredRelease = false;

            if (isCorrection (e, 0, t))
            {
                c.type = VoiceCommandType::correction;
                out.push (c);
                assign (0, n, t, true);
            }
            else
            {
                c.type = legatoMode ? VoiceCommandType::legato : VoiceCommandType::start;
                c.glide = settings.glideEnabled;
                out.push (c);
                assign (0, n, t, false);
            }
        }
        else
        {
            c.type = VoiceCommandType::start;
            c.glide = settings.glideEnabled && settings.glideMode == GlideMode::always && lastNote >= 0;
            out.push (c);
            assign (0, n, t, false);
        }

        lastNote = n;
    }

    if (deferredRelease)
    {
        VoiceCommand c;
        c.type = VoiceCommandType::release;
        c.voice = 0;
        c.note = static_cast<uint8_t> (voiceNote[0] & 0x7f);
        out.push (c);
        markReleased (0, t);
    }
}

void VoiceAllocator::processPoly (std::span<const NoteInput> group, int64_t t, const AllocatorSettings& settings,
                                  const std::array<bool, maxVoices>& voiceActive, VoiceCommandList& out) noexcept
{
    // Pairing (docs/SYNTH-DESIGN.md 10.2): each note-on, in event order, with the earliest unpaired note-off of
    // the group whose note held a voice at the group start. A pair that meets the correction rule moves the
    // new note onto the old note's voice; the note-off of the pair is then consumed.
    std::array<int8_t, maxPairedEvents> correctionVoice {}; // per note-on: voice of its correction, -1 none
    std::array<bool, maxPairedEvents> consumedOff {}; // note-off taken over by a correction that has not run yet
    std::array<int16_t, maxPairedEvents> pairedOff {};  // per correction note-on: index of its note-off
    std::array<int16_t, maxPairedEvents> pairedOn {};   // per consumed note-off: index of its note-on
    std::array<bool, maxVoices> voicePaired {};
    correctionVoice.fill (-1);
    const auto pairedCount = group.size() <= maxPairedEvents ? group.size() : std::size_t { 0 };
    std::array<bool, maxPairedEvents> offTaken {};

    bool anyHeldAtStart = false;

    for (int v = 0; v < maxVoices; ++v)
        anyHeldAtStart = anyHeldAtStart
                         || (voiceNote[static_cast<std::size_t> (v)] >= 0 && voiceRelease[static_cast<std::size_t> (v)] < 0);

    for (std::size_t j = 0; j < pairedCount; ++j)
    {
        if (! group[j].isNoteOn)
            continue;

        for (std::size_t o = 0; o < pairedCount; ++o)
        {
            if (group[o].isNoteOn || offTaken[o])
                continue;

            const auto voice = findHeldVoice (group[o].note & 0x7f);

            if (voice < 0 || voicePaired[static_cast<std::size_t> (voice)])
                continue;

            offTaken[o] = true;
            voicePaired[static_cast<std::size_t> (voice)] = true;

            // The rule, and the new note must not already hold another voice (it would then have two)
            const auto newHeldOn = findHeldVoice (group[j].note & 0x7f);

            if (isCorrection (group[j], voice, t) && (newHeldOn < 0 || newHeldOn == voice))
            {
                correctionVoice[j] = static_cast<int8_t> (voice);
                consumedOff[o] = true;
                pairedOff[j] = static_cast<int16_t> (o);
                pairedOn[o] = static_cast<int16_t> (j);
            }

            break;
        }
    }

    std::array<bool, maxVoices> busy = voiceActive;
    int remainingOffs = countNoteOffs (group, 0);

    for (std::size_t j = 0; j < group.size(); ++j)
    {
        const auto& e = group[j];
        const int n = e.note & 0x7f;
        const bool paired = j < pairedCount;

        if (! e.isNoteOn)
        {
            --remainingOffs;

            if (paired && consumedOff[j])
                continue; // taken over by its correction note-on

            if (! held[static_cast<std::size_t> (n)])
                continue; // unknown note-off

            held[static_cast<std::size_t> (n)] = false;
            const auto voice = findHeldVoice (n);

            if (voice >= 0)
            {
                VoiceCommand c;
                c.type = VoiceCommandType::release;
                c.voice = static_cast<uint8_t> (voice);
                c.note = static_cast<uint8_t> (n);
                out.push (c);
                markReleased (voice, t);
            }

            continue;
        }

        // A note pressed again after its own note-off was skipped for a correction still to come: that
        // note-off happens first (the correction is cancelled), so the new note-on retriggers normally
        for (std::size_t o = 0; o < j && paired; ++o)
        {
            if (! consumedOff[o] || (group[o].note & 0x7f) != n)
                continue;

            consumedOff[o] = false;
            correctionVoice[static_cast<std::size_t> (pairedOn[o])] = -1;

            if (held[static_cast<std::size_t> (n)])
            {
                held[static_cast<std::size_t> (n)] = false;
                const auto voice = findHeldVoice (n);

                if (voice >= 0)
                {
                    VoiceCommand c;
                    c.type = VoiceCommandType::release;
                    c.voice = static_cast<uint8_t> (voice);
                    c.note = static_cast<uint8_t> (n);
                    out.push (c);
                    markReleased (voice, t);
                }
            }
        }

        // Once the correction note-on is reached its note-off is an ordinary one again: if it comes later
        // in the group it is then unknown (the correction took its note), unless the note was pressed anew
        if (paired && correctionVoice[j] >= 0)
            consumedOff[static_cast<std::size_t> (pairedOff[j])] = false;

        if (! hasRoom (out, 2, remainingOffs))
            continue;

        if (paired && correctionVoice[j] >= 0)
        {
            const auto voice = static_cast<int> (correctionVoice[j]);
            const auto oldNote = voiceNote[static_cast<std::size_t> (voice)];

            const auto newHeldOn = findHeldVoice (n); // an earlier note-on of this group may have taken n

            if (oldNote >= 0 && voiceRelease[static_cast<std::size_t> (voice)] < 0 && (newHeldOn < 0 || newHeldOn == voice))
            {
                held[static_cast<std::size_t> (oldNote)] = false;
                held[static_cast<std::size_t> (n)] = true;
                VoiceCommand c;
                c.type = VoiceCommandType::correction;
                c.voice = static_cast<uint8_t> (voice);
                c.note = static_cast<uint8_t> (n);
                c.velocity = e.velocity;
                out.push (c);
                assign (voice, n, t, true);
                busy[static_cast<std::size_t> (voice)] = true;
                lastNote = n;
                continue;
            }
            // (the voice or the note changed in between: a normal start below, the old note is released there)
        }

        bool connected = anyHeldAtStart;

        for (int v = 0; v < maxVoices && ! connected; ++v)
            connected = voiceNote[static_cast<std::size_t> (v)] >= 0 && voiceRelease[static_cast<std::size_t> (v)] < 0;

        VoiceCommand c;
        c.note = static_cast<uint8_t> (n);
        c.velocity = e.velocity;
        c.glide = settings.glideEnabled && lastNote >= 0 && (settings.glideMode == GlideMode::always || connected);
        c.glideFromNote = c.glide ? static_cast<int8_t> (lastNote) : static_cast<int8_t> (-1);

        // 1. the note already has a voice (held first, then releasing)
        int voice = findHeldVoice (n);

        for (int v = 0; v < maxVoices && voice < 0; ++v)
            if (busy[static_cast<std::size_t> (v)] && voiceNote[static_cast<std::size_t> (v)] == n)
                voice = v;

        c.type = VoiceCommandType::start;

        // 2. the lowest inactive voice
        for (int v = 0; v < maxVoices && voice < 0; ++v)
            if (! busy[static_cast<std::size_t> (v)])
                voice = v;

        // 3. steal: the release that started first, else the oldest note-on (ties: the lower index)
        if (voice < 0)
        {
            c.type = VoiceCommandType::steal;

            for (int v = 0; v < maxVoices; ++v)
            {
                const auto vi = static_cast<std::size_t> (v);

                if (voiceRelease[vi] >= 0
                    && (voice < 0 || voiceRelease[vi] < voiceRelease[static_cast<std::size_t> (voice)]))
                    voice = v;
            }

            if (voice < 0)
            {
                voice = 0;

                for (int v = 1; v < maxVoices; ++v)
                    if (voiceStart[static_cast<std::size_t> (v)] < voiceStart[static_cast<std::size_t> (voice)])
                        voice = v;
            }
            // A held note that loses its voice stays "held" without a voice: its note-off is then ignored
        }

        // A correction that fell through after its note-off was already skipped: that note-off happens here
        if (paired && correctionVoice[j] >= 0 && static_cast<std::size_t> (pairedOff[j]) < j)
        {
            const auto offNote = group[static_cast<std::size_t> (pairedOff[j])].note & 0x7f;
            const auto oldVoice = static_cast<int> (correctionVoice[j]);
            const auto ov = static_cast<std::size_t> (oldVoice);

            if (voiceNote[ov] == offNote && voiceRelease[ov] < 0) // still the note the pair was made with
            {
                held[static_cast<std::size_t> (offNote)] = false;

                if (oldVoice != voice)
                {
                    VoiceCommand r;
                    r.type = VoiceCommandType::release;
                    r.voice = static_cast<uint8_t> (oldVoice);
                    r.note = static_cast<uint8_t> (offNote);
                    out.push (r);
                    markReleased (oldVoice, t);
                }
            }
        }

        held[static_cast<std::size_t> (n)] = true;
        c.voice = static_cast<uint8_t> (voice);
        out.push (c);
        assign (voice, n, t, false);
        busy[static_cast<std::size_t> (voice)] = true;
        lastNote = n;
    }
}

void VoiceAllocator::releaseAll (VoiceCommandList& out) noexcept
{
    for (int v = 0; v < maxVoices; ++v)
    {
        const auto vi = static_cast<std::size_t> (v);

        if (voiceNote[vi] < 0 || voiceRelease[vi] >= 0)
            continue;

        VoiceCommand c;
        c.type = VoiceCommandType::release;
        c.voice = static_cast<uint8_t> (v);
        c.note = static_cast<uint8_t> (voiceNote[vi] & 0x7f);
        out.push (c);
        markReleased (v, lastSample);
    }

    held.fill (false);
    stackSize = 0;
}

int VoiceAllocator::getNewestVoice (const std::array<bool, maxVoices>& voiceActive) const noexcept
{
    int best = -1;

    for (int v = 0; v < maxVoices; ++v)
    {
        const auto vi = static_cast<std::size_t> (v);

        if (voiceActive[vi] && voiceOrder[vi] > 0
            && (best < 0 || voiceOrder[vi] > voiceOrder[static_cast<std::size_t> (best)]))
            best = v;
    }

    return best;
}

int VoiceAllocator::getNumHeldNotes() const noexcept
{
    return static_cast<int> (std::count (held.begin(), held.end(), true));
}

} // namespace vocalsynth::synth
