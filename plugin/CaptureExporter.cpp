#include "CaptureExporter.h"

#include "ExportFiles.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth::CaptureExporter
{

namespace
{
juce::String exportVersionName (ExportVersion version)
{
    return version == ExportVersion::raw ? "Raw" : "Clean";
}
} // namespace

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
    // toHexString of an int64 prints its two's complement: always 16 digits for negative values
    return juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()).paddedLeft ('0', 16).toLowerCase();
}

juce::String makeBaseName (const Take& take, const RenderedTake& rendered)
{
    const auto start = take.wallClockStartMs > 0 ? juce::Time (take.wallClockStartMs) : juce::Time::getCurrentTime();

    auto name = "VocalSynth_" + start.formatted ("%Y-%m-%d_%H%M%S") + "_Take"
                + juce::String (juce::jmax (0, take.number)).paddedLeft ('0', 2);

    if (take.mode == TakeMode::synced)
        name << "_Bar" << juce::String ((juce::int64) juce::jmax ((int64_t) 0, rendered.originBar)).paddedLeft ('0', 3);

    // ASCII only, no spaces (the timestamp and numbers already are; this guards the format string)
    return name.replaceCharacter (' ', '_');
}

juce::String makeMidiFileName (const Take& take, const RenderedTake& rendered, const RenderOptions& options)
{
    auto name = makeBaseName (take, rendered) + "_" + exportVersionName (options.version);

    // Free takes are never quantized (docs/M3-SPEC.md 7), so their names carry no tag
    if (take.mode == TakeMode::synced && options.quantize != QuantizeGrid::off)
    {
        name << "_" << quantizeGridTag (options.quantize);

        const auto strength = juce::roundToInt (juce::jlimit (0.0, 100.0, options.quantizeStrengthPercent));

        if (strength < 100)
            name << "-" << strength;
    }

    return name + ".mid";
}

juce::String makeWavFileName (const Take& take, const RenderedTake& rendered)
{
    return makeBaseName (take, rendered) + "_Voice.wav";
}

juce::File writeMidi (const Take& take, const RenderOptions& options, SmfFormat format, const juce::File& folder)
{
    if (folder == juce::File() || ! folder.createDirectory())
        return {};

    const auto rendered = TakeRenderer::render (take, options);

    SmfOptions smf;
    smf.format = format;
    smf.trackName = ("VocalSynth Take " + juce::String (take.number) + " " + exportVersionName (options.version))
                        .toStdString();

    const auto bytes = SmfWriter::write (rendered, smf);
    return ExportFiles::writeBytesAtomically (folder.getChildFile (makeMidiFileName (take, rendered, options)),
                                              bytes.data(), bytes.size());
}

juce::File writeWav (const juce::File& target, double sampleRate, WavSampleFormat format, uint64_t numFrames,
                     const std::function<void (uint64_t, std::span<float>)>& readSamples)
{
    if (readSamples == nullptr)
        return {};

    WavFileWriter writer;

    if (! writer.open (target, sampleRate, format, numFrames))
        return {};

    std::vector<float> block (static_cast<std::size_t> (juce::jmin<uint64_t> (writer.getNumFrames(), 1u << 16)));

    while (writer.getFramesWritten() < writer.getNumFrames())
    {
        const auto n = static_cast<std::size_t> (
            juce::jmin<uint64_t> (block.size(), writer.getNumFrames() - writer.getFramesWritten()));
        const std::span<float> slice { block.data(), n };
        readSamples (writer.getFramesWritten(), slice);

        if (! writer.write (slice))
            return {};
    }

    return writer.finish();
}

//==============================================================================
WavFileWriter::~WavFileWriter()
{
    abort();
}

bool WavFileWriter::open (const juce::File& target, double sampleRate, WavSampleFormat format, uint64_t numFrames)
{
    abort();

    if (target == juce::File() || ! (sampleRate > 0.0) || ! target.getParentDirectory().createDirectory())
        return false;

    targetFile = target;
    sampleFormat = format;
    numFramesTotal = juce::jmin (numFrames, WavWriter::maxFrames (format));
    framesWritten = 0;

    temp = std::make_unique<juce::TemporaryFile> (target);
    stream = std::make_unique<juce::FileOutputStream> (temp->getFile());

    if (! stream->openedOk())
    {
        abort();
        return false;
    }

    const auto header = WavWriter::makeHeader (format, sampleRate, numFramesTotal);

    if (! stream->write (header.data(), header.size()))
    {
        abort();
        return false;
    }

    return true;
}

bool WavFileWriter::write (std::span<const float> samples)
{
    if (stream == nullptr)
        return false;

    if (samples.size() > numFramesTotal - framesWritten)
        return false;

    const auto needed = samples.size() * static_cast<std::size_t> (WavWriter::bytesPerSample (sampleFormat));

    if (bytes.size() < needed)
        bytes.resize (needed); // reused: grows to one slice once

    const auto encoded = WavWriter::encode (samples, sampleFormat, std::span<uint8_t> { bytes.data(), needed });

    if (encoded != needed || ! stream->write (bytes.data(), encoded))
        return false;

    framesWritten += samples.size();
    return true;
}

juce::File WavFileWriter::finish()
{
    if (stream == nullptr || framesWritten != numFramesTotal)
    {
        abort();
        return {};
    }

    if (WavWriter::dataPaddingBytes (sampleFormat, numFramesTotal) > 0)
    {
        const uint8_t pad = 0;
        stream->write (&pad, 1);
    }

    stream->flush();
    const auto ok = stream->getStatus().wasOk();
    stream.reset(); // closes the file before it is moved

    juce::File result;

    if (ok && temp->getFile().existsAsFile())
    {
        if (temp->overwriteTargetFileWithTemporary())
        {
            result = targetFile;
        }
        else
        {
            // FL may still hold the previous file open: numbered sibling (as ExportFiles::writeAtomically)
            const auto sibling = targetFile.getNonexistentSibling (true);

            if (temp->getFile().moveFileTo (sibling))
                result = sibling;
        }
    }

    abort(); // deletes the temporary file if it is still there
    return result;
}

void WavFileWriter::abort()
{
    stream.reset();
    temp.reset(); // ~TemporaryFile deletes the temporary file
    targetFile = juce::File();
    numFramesTotal = 0;
    framesWritten = 0;
}

//==============================================================================
juce::File persistWav (const juce::File& cachedWav, const juce::File& takesFolder)
{
    if (! cachedWav.existsAsFile() || takesFolder == juce::File() || ! takesFolder.createDirectory())
        return {};

    return ExportFiles::moveOrCopy (cachedWav, takesFolder.getChildFile (cachedWav.getFileName()));
}

void touchHeartbeat (const juce::File& instanceFolder)
{
    if (! instanceFolder.isDirectory())
        return;

    const auto heartbeat = instanceFolder.getChildFile (heartbeatFileName);

    if (heartbeat.existsAsFile())
        heartbeat.setLastModificationTime (juce::Time::getCurrentTime());
    else
        heartbeat.replaceWithText ("VocalSynth capture instance folder (deleted when the plugin closes)\n");
}

int deleteStaleInstanceFolders (const juce::File& root, const juce::String& keepInstanceId, int maxAgeHours)
{
    if (! root.isDirectory())
        return 0;

    const auto now = juce::Time::getCurrentTime();
    const auto maxAge = juce::RelativeTime::hours (static_cast<double> (juce::jmax (0, maxAgeHours)));
    int deleted = 0;

    for (const auto& folder : root.findChildFiles (juce::File::findDirectories, false))
    {
        if (folder.getFileName() == keepInstanceId)
            continue;

        const auto heartbeat = folder.getChildFile (heartbeatFileName);
        auto lastAlive = folder.getLastModificationTime();

        if (heartbeat.existsAsFile())
        {
            lastAlive = heartbeat.getLastModificationTime();
        }
        else
        {
            // No heartbeat (crashed before writing one): the newest file decides. Its own time is more
            // reliable than the folder's, which Windows only updates when entries are added or removed.
            const auto files = folder.findChildFiles (juce::File::findFiles, true);

            if (! files.isEmpty())
            {
                lastAlive = files.getFirst().getLastModificationTime();

                for (const auto& f : files)
                    lastAlive = juce::jmax (lastAlive, f.getLastModificationTime());
            }
        }

        if (now - lastAlive > maxAge && folder.deleteRecursively())
            ++deleted;
    }

    return deleted;
}

void deleteInstanceFolder (const juce::File& instanceFolder)
{
    // Only ever below the capture root (never a folder the user chose)
    if (instanceFolder.isDirectory() && instanceFolder.getParentDirectory().getFileName() == "Capture"
        && instanceFolder.getParentDirectory().getParentDirectory() == ExportFiles::getTempRoot())
        instanceFolder.deleteRecursively();
}

} // namespace vocalsynth::CaptureExporter
