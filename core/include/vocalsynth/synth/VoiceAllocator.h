#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vocalsynth::synth
{

/** One note event of a group (all note events of one sample position, in event order). */
struct NoteInput
{
    bool isNoteOn = true;
    uint8_t note = 60;
    uint8_t velocity = 100;      ///< 1..127 (note-on)
    bool correction = false;     ///< VsEventTiming flag EventTimingFlags::correction (provisional note fixed)
};

enum class VoiceCommandType : uint8_t
{
    start,      ///< note start; on an active voice a retrigger (envelopes restart from the current value)
    steal,      ///< voice is taken: fade out over stealFadeSeconds, then start (like start) with this note
    legato,     ///< connected change on the same voice: new pitch (glide if set), envelopes keep running
    correction, ///< provisional note corrected: new pitch at once, no glide, no envelope restart,
                ///< velocity / random value of the original note kept
    release     ///< note-off: envelopes release
};

struct VoiceCommand
{
    VoiceCommandType type = VoiceCommandType::start;
    uint8_t voice = 0;
    uint8_t note = 60;
    uint8_t velocity = 100;
    bool glide = false;          ///< glide to the note (start / steal / legato)
    int8_t glideFromNote = -1;   ///< poly: glide starts at this note's pitch; -1 = from the voice's own
                                 ///< current pitch (mono / legato, or no previous note: no glide)
};

/** Fixed-capacity command list (never allocates; push() returns false when full). */
class VoiceCommandList
{
public:
    static constexpr std::size_t capacity = 64;

    bool push (const VoiceCommand& c) noexcept
    {
        if (count >= capacity)
            return false;

        commands[count++] = c;
        return true;
    }

    void clear() noexcept { count = 0; }
    std::size_t size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }
    const VoiceCommand& operator[] (std::size_t i) const noexcept { return commands[i]; }
    std::span<const VoiceCommand> view() const noexcept { return { commands.data(), count }; }

private:
    std::array<VoiceCommand, capacity> commands {};
    std::size_t count = 0;
};

struct AllocatorSettings
{
    PlayMode playMode = PlayMode::legato;
    GlideMode glideMode = GlideMode::legatoOnly;
    bool glideEnabled = false; ///< glideTimeMs > 0
};

/** Note -> voice logic, pure (no DSP), so every rule is testable on its own (docs/SYNTH-DESIGN.md 10).

    The engine calls processGroup() once per sample position that has note events, after rendering up to
    that position, with the activity of every voice (a released voice whose envelope finished is inactive).
    Rules (binding):
    - Mono / Legato: voice 0 only; a note stack of the held notes (maxHeldNotes, last-note priority).
      A note-off that empties the stack is DEFERRED to the end of the group: a note-on at the same sample
      cancels it and counts as a CONNECTED change (the VoicePipeline sends a retrigger change as note-off +
      note-on at one sample, a legato change as note-on + note-off; both are connected changes).
      Connected change (note-on while a note sounds, or the deferred case): Legato -> legato command,
      Mono -> start (retrigger); returning to a still-held note after a note-off: the same.
      Not connected (voice silent or releasing): start.
    - Correction rule: a connected change caused by a NOTE-ON within correctionWindowSeconds of the
      replaced note's own note-on (absolute samples), or flagged NoteInput::correction, is a correction
      command (both modes, also Poly: the new note takes over the old note's voice).
    - Glide: glideEnabled and (glideMode == always and a previous note exists since reset, or
      glideMode == legatoOnly and the change is connected); never for corrections.
    - Poly: a note-on for a note that already has a voice (held or releasing) restarts that voice; else
      the lowest-index inactive voice; else steal: the voice in release whose release started first, else
      the voice whose note-on is oldest. glideFromNote = the most recent note-on's note.
    - Note-off for a note without a voice: ignored. At most maxVoices voices hold notes; nothing hangs:
      every voice that got a start / steal / legato / correction for a note gets a release when that
      note's last note-off arrives (invariant test with random event sequences).
    - releaseAll(): release for every voice holding a note, stack cleared (flush, mode change).
    No allocation. Audio thread only. Owner: agent B. */
class VoiceAllocator
{
public:
    static constexpr int maxVoices = 16;
    static constexpr int maxHeldNotes = 16;
    static constexpr double correctionWindowSeconds = 0.040;
    static constexpr double stealFadeSeconds = 0.002;

    void prepare (double sampleRate) noexcept;

    /** Forgets every note and voice (no commands: the engine resets the voices itself). */
    void reset() noexcept;

    /** group: the note events at absoluteSample (non-empty, event order). voiceActive[i]: voice i sounds.
        Appends the commands (all at absoluteSample) to out. */
    void processGroup (std::span<const NoteInput> group, int64_t absoluteSample, const AllocatorSettings& settings,
                       const std::array<bool, maxVoices>& voiceActive, VoiceCommandList& out) noexcept;

    void releaseAll (VoiceCommandList& out) noexcept;

    /** Voice of the most recent start / steal / legato / correction that is still active, -1 if none. */
    int getNewestVoice (const std::array<bool, maxVoices>& voiceActive) const noexcept;

    /** Notes held (note-on without note-off yet), for tests. */
    int getNumHeldNotes() const noexcept;

private:
    double sampleRate = 48000.0;
    std::array<int16_t, maxVoices> voiceNote {};     ///< note held by each voice, -1 none
    std::array<int64_t, maxVoices> voiceStart {};    ///< absolute sample of its note-on (steal order, correction)
    std::array<int64_t, maxVoices> voiceRelease {};  ///< absolute sample of its release, -1 none
    std::array<uint8_t, maxHeldNotes> stack {};
    int stackSize = 0;
    int lastNote = -1;
    int newestVoice = -1;
};

} // namespace vocalsynth::synth
