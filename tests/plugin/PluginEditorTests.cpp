#include "Parameters.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <functional>

using namespace vocalsynth;

// Editor smoke tests: build, layout and paint headless (software renderer, no window)

TEST_CASE ("Editor builds for both variants and shows the M1 header", "[editor]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    CAPTURE (variant == Variant::fx ? "fx" : "instrument");

    VocalSynthProcessor processor (variant);
    processor.prepareToPlay (48000.0, 512);

    // Some blocks first, so the constructor shows a filled histogram, counters and a test note
    if (auto* pattern = processor.getParameters().getParameter (params::testPattern.getParamID()))
        pattern->setValueNotifyingHost (pattern->convertTo0to1 ((float) TestPattern::notes));

    juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                 processor.getTotalNumOutputChannels()),
                                     512);
    juce::MidiBuffer midi;

    for (const auto blockSize : { 512, 512, 512, 64, 7, 1 })
    {
        buffer.clear();
        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), blockSize);
        processor.processBlock (view, midi);
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    REQUIRE (editor != nullptr);
    CHECK (dynamic_cast<juce::DragAndDropContainer*> (editor.get()) != nullptr);
    CHECK (editor->getWidth() == 640);
    CHECK (editor->getHeight() == 440);

    // Every visible child lies inside the editor
    for (auto* child : editor->getChildren())
        if (child->isVisible() && dynamic_cast<juce::TooltipWindow*> (child) == nullptr)
            CHECK (editor->getLocalBounds().contains (child->getBounds()));

    // Painting must not throw or assert (software renderer, no window needed)
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
    CHECK (image.getWidth() == 640);

    // For a visual check of the layout: VOCALSYNTH_EDITOR_SNAPSHOT_DIR=<dir> writes a PNG per variant
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("VOCALSYNTH_EDITOR_SNAPSHOT_DIR", {});
        dir.isNotEmpty())
    {
        const auto file = juce::File (dir).getChildFile (variant == Variant::fx ? "editor-fx.png" : "editor-instrument.png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (image, out);
    }

    editor.reset();
    processor.releaseResources();
}

TEST_CASE ("Editor shows the plugin format", "[editor]")
{
    CHECK (VocalSynthEditor::getFormatName (juce::AudioProcessor::wrapperType_VST3) == "VST3");
    CHECK (VocalSynthEditor::getFormatName (juce::AudioProcessor::wrapperType_Undefined) == "CLAP");
    CHECK (VocalSynthEditor::getFormatName (juce::AudioProcessor::wrapperType_Standalone) == "Standalone");
}

TEST_CASE ("Editor control labels match the names used in FL-ROUTING.md and LIESMICH.txt", "[editor]")
{
    VocalSynthProcessor processor (Variant::fx);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    REQUIRE (editor != nullptr);

    juce::StringArray labelTexts;
    std::function<void (juce::Component&)> collectLabels = [&] (juce::Component& c)
    {
        if (auto* label = dynamic_cast<juce::Label*> (&c))
            labelTexts.add (label->getText());

        for (auto* child : c.getChildren())
            collectLabels (*child);
    };
    collectLabels (*editor);

    // The docs refer to these controls by name (case aside)
    for (const auto* expected : { "Test pattern", "MIDI channel", "Test tempo (BPM)", "CC number" })
    {
        CAPTURE (expected);
        CHECK (labelTexts.contains (expected));
    }
}
