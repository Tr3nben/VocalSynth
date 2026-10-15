#include "Parameters.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <functional>
#include <vector>

using namespace vocalsynth;
using vocalsynth::VoiceRange;

// Editor smoke tests: build, layout and paint headless (software renderer, no window)

namespace
{
juce::StringArray collectLabelTexts (juce::Component& root)
{
    juce::StringArray labelTexts;
    std::function<void (juce::Component&)> collect = [&] (juce::Component& c)
    {
        if (auto* label = dynamic_cast<juce::Label*> (&c))
            labelTexts.add (label->getText());

        for (auto* child : c.getChildren())
            collect (*child);
    };
    collect (root);
    return labelTexts;
}

/** Every visible child lies inside the editor, and visible siblings do not overlap. */
void checkLayout (juce::Component& editor)
{
    std::vector<juce::Component*> visible;

    for (auto* child : editor.getChildren())
        if (child->isVisible() && dynamic_cast<juce::TooltipWindow*> (child) == nullptr)
        {
            CHECK (editor.getLocalBounds().contains (child->getBounds()));
            CHECK_FALSE (child->getBounds().isEmpty());
            visible.push_back (child);
        }

    for (std::size_t i = 0; i < visible.size(); ++i)
        for (std::size_t j = i + 1; j < visible.size(); ++j)
        {
            CAPTURE (visible[i]->getComponentID(), visible[j]->getComponentID(), visible[i]->getBounds().toString(),
                     visible[j]->getBounds().toString());
            CHECK_FALSE (visible[i]->getBounds().intersects (visible[j]->getBounds()));
        }
}
} // namespace

TEST_CASE ("Editor builds for both variants and shows both pages", "[editor]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    CAPTURE (variant == Variant::fx ? "fx" : "instrument");

    VocalSynthProcessor processor (variant);
    processor.prepareToPlay (48000.0, 512);

    // Some blocks first, so the constructor shows a filled histogram, counters and a test note
    if (auto* source = processor.getParameters().getParameter (params::source.getParamID()))
        source->setValueNotifyingHost (source->convertTo0to1 ((float) params::Source::testPattern));

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
    CHECK (editor->getWidth() == VocalSynthEditor::defaultWidth);
    CHECK (editor->getHeight() == VocalSynthEditor::defaultHeight);

    auto* vsEditor = dynamic_cast<VocalSynthEditor*> (editor.get());
    REQUIRE (vsEditor != nullptr);
    CHECK (vsEditor->getPage() == VocalSynthEditor::Page::voice); // the main panel comes first

    for (const auto page : { VocalSynthEditor::Page::voice, VocalSynthEditor::Page::testAndRouting })
    {
        const auto isVoice = page == VocalSynthEditor::Page::voice;
        CAPTURE (isVoice);
        vsEditor->showPage (page);
        CHECK (vsEditor->getPage() == page);
        checkLayout (*editor);

        // Painting must not throw or assert (software renderer, no window needed)
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
        CHECK (image.getWidth() == VocalSynthEditor::defaultWidth);

        // For a visual check of the layout: VOCALSYNTH_EDITOR_SNAPSHOT_DIR=<dir> writes a PNG per variant and page
        if (const auto dir = juce::SystemStats::getEnvironmentVariable ("VOCALSYNTH_EDITOR_SNAPSHOT_DIR", {});
            dir.isNotEmpty())
        {
            const auto name = juce::String (variant == Variant::fx ? "editor-fx" : "editor-instrument")
                              + (isVoice ? "-voice.png" : "-test.png");
            const auto file = juce::File (dir).getChildFile (name);
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat().writeImageToStream (image, out);
        }
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

    const auto labelTexts = collectLabelTexts (*editor);

    // The docs (FL-ROUTING.md, M2-TESTING.md, LIESMICH.txt) refer to these controls by name
    for (const auto* expected : { "Test pattern", "MIDI channel", "Test tempo (BPM)", "CC number", "Source",
                                  "Voice Range", "Algorithm", "Key", "Scale", "Bend Mode", "Bend Range", "Stickiness",
                                  "Gate", "Min Note", "Velocity Sens.", "Octave" })
    {
        CAPTURE (expected);
        CHECK (labelTexts.contains (expected));
    }

    // Toggle buttons and page buttons
    juce::StringArray buttonTexts;
    for (auto* child : editor->getChildren())
        if (auto* button = dynamic_cast<juce::Button*> (child))
            buttonTexts.add (button->getButtonText());

    for (const auto* expected : { "Legato", "Monitor Tone", "Virtual MIDI Out", "Mute Dry", "Restart pattern", "Voice",
                                  "Test & Routing" })
    {
        CAPTURE (expected);
        CHECK (buttonTexts.contains (expected));
    }
}

TEST_CASE ("Every M2 parameter has a control on the Voice page", "[editor]")
{
    VocalSynthProcessor processor (Variant::fx);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* vsEditor = dynamic_cast<VocalSynthEditor*> (editor.get());
    REQUIRE (vsEditor != nullptr);
    vsEditor->showPage (VocalSynthEditor::Page::voice);

    int comboBoxes = 0, sliders = 0, toggles = 0;
    for (auto* child : editor->getChildren())
    {
        if (! child->isVisible())
            continue;

        comboBoxes += dynamic_cast<juce::ComboBox*> (child) != nullptr ? 1 : 0;
        sliders += dynamic_cast<juce::Slider*> (child) != nullptr ? 1 : 0;
        toggles += dynamic_cast<juce::ToggleButton*> (child) != nullptr ? 1 : 0;
    }

    CHECK (comboBoxes == 7); // Source, Voice Range, Algorithm, Key, Scale, Bend Mode, Bend Range
    CHECK (sliders == 5);    // Stickiness, Gate, Min Note, Velocity Sens., Octave
    CHECK (toggles == 1);    // Legato

    // A control changes its parameter (the attachments are connected)
    juce::ComboBox* rangeBox = nullptr;
    for (auto* child : editor->getChildren())
        if (auto* box = dynamic_cast<juce::ComboBox*> (child); box != nullptr && box->getNumItems() == 7)
            rangeBox = box;

    REQUIRE (rangeBox != nullptr);
    CHECK (rangeBox->getSelectedItemIndex() == 2); // Tenor
    rangeBox->setSelectedItemIndex (0, juce::sendNotificationSync);
    CHECK (processor.getVoiceSettings().range == VoiceRange::bass);
}

TEST_CASE ("Editor shows the latency and follows the analysis", "[editor]")
{
    VocalSynthProcessor processor (Variant::fx);
    processor.prepareToPlay (44100.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    REQUIRE (editor != nullptr);

    const auto latency = VocalSynthEditor::formatLatency (processor.getVoiceLatencyMs());
    bool found = false;
    for (const auto& text : collectLabelTexts (*editor))
        found = found || (text.contains ("Latency") && text.contains (latency) && text.contains ("Tenor"));

    CHECK (found);
    CHECK (VocalSynthEditor::formatLatency (24.21) == "24.2 ms");

    // Process a little silence, then repaint the voice page with the new snapshot
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    for (int i = 0; i < 20; ++i)
    {
        buffer.clear();
        processor.processBlock (buffer, midi);
    }

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
    CHECK (image.getWidth() == VocalSynthEditor::defaultWidth);
}

TEST_CASE ("Note names follow FL Studio (C5 = 60)", "[editor]")
{
    CHECK (VocalSynthEditor::getNoteName (60) == "C5");
    CHECK (VocalSynthEditor::getNoteName (69) == "A5"); // 440 Hz
    CHECK (VocalSynthEditor::getNoteName (61) == "C#5");
    CHECK (VocalSynthEditor::getNoteName (0) == "C0");
    CHECK (VocalSynthEditor::getNoteName (127) == "G10");
    CHECK (VocalSynthEditor::getNoteName (-1) == "-");
    CHECK (VocalSynthEditor::getNoteName (128) == "-");
}
