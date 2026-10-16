#include "CaptureExporter.h"

#include "ExportFiles.h"

namespace vocalsynth::CaptureExporter
{

// TODO(C): implement (docs/M3-DESIGN.md 12). Stubs: folders only, nothing is written or deleted.

juce::File getCaptureRoot()
{
    auto dir = ExportFiles::getTempRoot().getChildFile ("Capture");
    dir.createDirectory();
    return dir;
}

juce::File getInstanceFolder (const juce::String& instanceId)
{
    return getCaptureRoot().getChildFile (instanceId);
}

juce::File getDefaultTakesFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("VocalSynth")
        .getChildFile ("Takes");
}

juce::String makeInstanceId()
{
    return juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()).paddedLeft ('0', 16);
}

juce::String makeBaseName (const Take&, const RenderedTake&)
{
    return "VocalSynth";
}

juce::String makeMidiFileName (const Take& take, const RenderedTake& rendered, const RenderOptions&)
{
    return makeBaseName (take, rendered) + ".mid";
}

juce::String makeWavFileName (const Take& take, const RenderedTake& rendered)
{
    return makeBaseName (take, rendered) + "_Voice.wav";
}

juce::File writeMidi (const Take&, const RenderOptions&, SmfFormat, const juce::File&)
{
    return {};
}

juce::File writeWav (const juce::File&, double, WavSampleFormat, uint64_t,
                     const std::function<void (uint64_t, std::span<float>)>&)
{
    return {};
}

juce::File persistWav (const juce::File&, const juce::File&)
{
    return {};
}

void touchHeartbeat (const juce::File&)
{
}

int deleteStaleInstanceFolders (const juce::File&, const juce::String&, int)
{
    return 0;
}

void deleteInstanceFolder (const juce::File&)
{
}

} // namespace vocalsynth::CaptureExporter
