#pragma once

#include "vocalsynth/CaptureOptions.h"
#include "vocalsynth/SmfWriter.h"
#include "vocalsynth/Take.h"
#include "vocalsynth/TakeRenderer.h"
#include "vocalsynth/WavWriter.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <span>

namespace vocalsynth
{

/** Files of the M3 capture (docs/M3-DESIGN.md 12): names, folders, MIDI and WAV writing, persisting dragged
    WAVs, cleanup. Message thread (MIDI on drag start, Save as, cleanup at destruction) and capture worker
    thread (WAV cache, heartbeat, stale-folder cleanup at start). File I/O and allocation: never on the
    audio thread. */
namespace CaptureExporter
{
inline constexpr int heartbeatMaxAgeHours = 24;
inline constexpr const char* heartbeatFileName = "heartbeat";

/** <temp>/VocalSynth/Capture (created if missing). */
juce::File getCaptureRoot();

/** <temp>/VocalSynth/Capture/<instanceId>. Not created. */
juce::File getInstanceFolder (const juce::String& instanceId);

/** <Documents>/VocalSynth/Takes (not created): dragged / saved WAVs live here and are never deleted. */
juce::File getDefaultTakesFolder();

/** 16 lowercase hex characters, unique per processor instance. */
juce::String makeInstanceId();

/** "VocalSynth_<yyyy-MM-dd_HHmmss>_Take<NN>[_Bar<BBB>]" (local time of take.wallClockStartMs; _Bar only
    for synced takes, BBB = rendered.originBar with at least 3 digits, NN at least 2). ASCII, no spaces. */
juce::String makeBaseName (const Take& take, const RenderedTake& rendered);

/** makeBaseName + "_Raw" | "_Clean" + quantize tag ("_Q16", strength < 100: "_Q16-50") + ".mid". */
juce::String makeMidiFileName (const Take& take, const RenderedTake& rendered, const RenderOptions& options);

/** makeBaseName + "_Voice.wav". */
juce::String makeWavFileName (const Take& take, const RenderedTake& rendered);

/** Renders the take, writes it as .mid into folder (ExportFiles::writeAtomically) and returns the file, or
    juce::File() on failure. */
juce::File writeMidi (const Take& take, const RenderOptions& options, SmfFormat format, const juce::File& folder);

/** Streams numFrames mono samples into a WAV file (temp + move). readSamples (frameIndex, destination)
    fills destination with the samples starting at frameIndex (worker: from the CaptureHistory). */
juce::File writeWav (const juce::File& target, double sampleRate, WavSampleFormat format, uint64_t numFrames,
                     const std::function<void (uint64_t, std::span<float>)>& readSamples);

/** Moves a cached WAV into takesFolder (created if missing) under the same name (numbered sibling if it
    exists). Returns the persisted file, or juce::File() on failure. */
juce::File persistWav (const juce::File& cachedWav, const juce::File& takesFolder);

/** Touches <instanceFolder>/heartbeat (creates the folder if needed). */
void touchHeartbeat (const juce::File& instanceFolder);

/** Deletes every folder in root other than keepInstanceId whose heartbeat (or, without one, the folder
    itself) was last modified more than maxAgeHours ago. Returns the number of folders deleted. */
int deleteStaleInstanceFolders (const juce::File& root, const juce::String& keepInstanceId,
                                int maxAgeHours = heartbeatMaxAgeHours);

/** Deletes an instance folder and everything in it (processor destructor). */
void deleteInstanceFolder (const juce::File& instanceFolder);
} // namespace CaptureExporter

} // namespace vocalsynth
