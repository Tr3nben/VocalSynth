#include "ExportFiles.h"

namespace vocalsynth::ExportFiles
{

// Factored out of the M1 TestClipExporter (docs/M3-DESIGN.md 12); the M1 test clips and the M3 capture
// exports share them.

juce::File getTempRoot()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("VocalSynth");
    dir.createDirectory();
    return dir;
}

juce::File writeAtomically (const juce::File& target, const std::function<bool (const juce::File&)>& writeContent)
{
    if (target == juce::File() || writeContent == nullptr)
        return {};

    juce::TemporaryFile temp (target);

    if (! writeContent (temp.getFile()) || ! temp.getFile().existsAsFile())
        return {};

    if (temp.overwriteTargetFileWithTemporary())
        return target;

    const auto sibling = target.getNonexistentSibling (true);

    if (temp.getFile().moveFileTo (sibling))
        return sibling;

    return {};
}

juce::File writeBytesAtomically (const juce::File& target, const void* data, size_t numBytes)
{
    return writeAtomically (target,
                            [data, numBytes] (const juce::File& file)
                            {
                                juce::FileOutputStream out (file);

                                if (! out.openedOk() || ! out.write (data, numBytes))
                                    return false;

                                out.flush();
                                return out.getStatus().wasOk();
                            });
}

juce::File moveOrCopy (const juce::File& source, const juce::File& target)
{
    if (! source.existsAsFile() || target == juce::File())
        return {};

    const auto destination = target.exists() ? target.getNonexistentSibling (true) : target;
    destination.getParentDirectory().createDirectory();

    if (source.moveFileTo (destination))
        return destination;

    if (source.copyFileTo (destination))
    {
        source.deleteFile();
        return destination;
    }

    return {};
}

} // namespace vocalsynth::ExportFiles
