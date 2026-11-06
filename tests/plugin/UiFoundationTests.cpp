// UI foundation: Theme, VsLookAndFeel, Knob (attachment, modulation ring), ChoiceChip, SectionPanel, WavetableView.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent E (UI).

#include "Parameters.h"
#include "PluginProcessor.h"
#include "SynthParameters.h"
#include "ui/ChoiceChip.h"
#include "ui/Knob.h"
#include "ui/SectionPanel.h"
#include "ui/Theme.h"
#include "ui/VsLookAndFeel.h"
#include "ui/WavetableView.h"
#include "vocalsynth/synth/FactoryWavetables.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>

using namespace vocalsynth;
using Catch::Approx;

namespace
{
/** A processor for its APVTS (all 130 parameters). */
struct ParamFixture
{
    VocalSynthProcessor processor { Variant::fx };
    ui::VsLookAndFeel lookAndFeel;

    juce::AudioProcessorValueTreeState& apvts() { return processor.getParameters(); }

    juce::RangedAudioParameter& param (const juce::String& id)
    {
        auto* p = apvts().getParameter (id);
        REQUIRE (p != nullptr);
        return *p;
    }

    void setPlain (const juce::String& id, float value)
    {
        auto& p = param (id);
        p.setValueNotifyingHost (p.convertTo0to1 (value));
    }

    float plain (const juce::String& id)
    {
        auto& p = param (id);
        return p.convertFrom0to1 (p.getValue());
    }
};

juce::Image render (juce::Component& c, juce::LookAndFeel& lnf)
{
    c.setLookAndFeel (&lnf);
    auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
    c.setLookAndFeel (nullptr);
    return image;
}

/** Number of pixels close to the colour (each channel within tolerance). */
int countPixels (const juce::Image& image, juce::Colour colour, int tolerance = 40)
{
    int n = 0;
    const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);

    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto p = data.getPixelColour (x, y);

            if (std::abs (p.getRed() - colour.getRed()) <= tolerance && std::abs (p.getGreen() - colour.getGreen()) <= tolerance
                && std::abs (p.getBlue() - colour.getBlue()) <= tolerance && p.getAlpha() > 200)
                ++n;
        }

    return n;
}

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos,
                             juce::ModifierKeys mods = {})
{
    // Distinct event times (the slider ignores a wheel event with the time of the previous one)
    static juce::int64 clock = juce::Time::currentTimeMillis();
    const auto time = juce::Time (++clock);
    return { juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
             time, downPos, time, 1, pos != downPos };
}
} // namespace

TEST_CASE ("UiFoundation: theme tokens", "[plugin][editor][ui]")
{
    CHECK (ui::theme::accent == juce::Colour (0xff4fc3f7));
    CHECK (ui::theme::modulation == juce::Colour (0xffffb74d));
    CHECK (ui::theme::background.getPerceivedBrightness() < 0.15f);
    CHECK (ui::theme::text.getPerceivedBrightness() > 0.8f);
    // Readable contrast of the dim text on the panel
    CHECK (ui::theme::textDim.getPerceivedBrightness() - ui::theme::panel.getPerceivedBrightness() > 0.35f);
    CHECK (ui::theme::fontSmall >= 11.0f);
    CHECK (ui::theme::font (ui::theme::fontNormal, true).isBold());
}

TEST_CASE ("UiFoundation: Knob attaches to its parameter both ways", "[plugin][editor][ui]")
{
    ParamFixture f;
    ui::Knob knob (f.apvts(), "filterCutoff", "Cutoff");
    knob.setBounds (0, 0, 60, 70);

    CHECK (knob.getComponentID() == "filterCutoff");
    CHECK (knob.getSlider().getComponentID() == "filterCutoff");
    CHECK (knob.getValueText() == "20000 Hz");

    // parameter -> knob
    f.setPlain ("filterCutoff", 1000.0f);
    CHECK (knob.getSlider().getValue() == Approx (1000.0).epsilon (1e-4));
    CHECK (knob.getValueText() == "1000 Hz");

    // knob -> parameter (through the attachment, as a gesture)
    knob.getSlider().setValue (440.0, juce::sendNotificationSync);
    CHECK (f.plain ("filterCutoff") == Approx (440.0f).epsilon (1e-4));

    // typed value (value text editing) and double-click default
    knob.getSlider().setValue (knob.getSlider().getValueFromText ("250 Hz"), juce::sendNotificationSync);
    CHECK (f.plain ("filterCutoff") == Approx (250.0f).epsilon (1e-4));
    CHECK (knob.getSlider().isDoubleClickReturnEnabled());
    CHECK (knob.getSlider().getDoubleClickReturnValue() == Approx (20000.0));

    // Tooltip defaults to the parameter name
    CHECK (knob.getSlider().getTooltip() == "Filter Cutoff");
}

TEST_CASE ("UiFoundation: Knob value texts carry units and signs", "[plugin][editor][ui]")
{
    ParamFixture f;
    ui::Knob volume (f.apvts(), "synthVolume", "Volume");
    ui::Knob octave (f.apvts(), "oscAOctave", "Octave");
    ui::Knob pan (f.apvts(), "oscAPan", "Pan");
    ui::Knob amount (f.apvts(), "mod1Amount", {}, ui::Knob::Style::bar);

    CHECK (volume.getValueText() == "-6.0 dB");
    CHECK_FALSE (static_cast<bool> (volume.getSlider().getProperties()["bipolar"]));
    CHECK (octave.getValueText() == "0 oct");
    f.setPlain ("oscAOctave", 2.0f);
    CHECK (octave.getValueText() == "+2 oct");
    f.setPlain ("oscAOctave", -1.0f);
    CHECK (octave.getValueText() == "-1 oct");
    CHECK (static_cast<bool> (pan.getSlider().getProperties()["bipolar"]));
    f.setPlain ("mod1Amount", 25.0f);
    CHECK (amount.getValueText() == "+25 %");
    CHECK (amount.getStyle() == ui::Knob::Style::bar);
    CHECK (amount.getSlider().getSliderStyle() == juce::Slider::LinearBar);

    // "+25 %" parses back
    amount.getSlider().setValue (amount.getSlider().getValueFromText ("+40 %"), juce::sendNotificationSync);
    CHECK (f.plain ("mod1Amount") == Approx (40.0f));
}

TEST_CASE ("UiFoundation: Knob fine drag with shift / ctrl moves a tenth as far", "[plugin][editor][ui]")
{
    ParamFixture f;
    juce::Component parent;
    parent.setSize (100, 100);
    ui::Knob knob (f.apvts(), "oscAPosition", "Position");
    parent.addAndMakeVisible (knob);
    knob.setBounds (0, 0, 60, 70);
    auto& slider = knob.getSlider();

    const auto drag = [&] (juce::ModifierKeys mods, float pixels)
    {
        slider.setValue (50.0, juce::sendNotificationSync);
        const juce::Point<float> down { 20.0f, 20.0f };
        slider.mouseDown (mouseEvent (slider, down, down, mods));

        // In two steps, so the virtual position accumulates
        slider.mouseDrag (mouseEvent (slider, down.translated (0.0f, -pixels * 0.5f), down, mods));
        slider.mouseDrag (mouseEvent (slider, down.translated (0.0f, -pixels), down, mods));
        slider.mouseUp (mouseEvent (slider, down.translated (0.0f, -pixels), down, mods));
        return f.plain ("oscAPosition") - 50.0f;
    };

    const auto normal = drag ({}, 40.0f);
    const auto fineShift = drag (juce::ModifierKeys (juce::ModifierKeys::shiftModifier), 40.0f);
    const auto fineCtrl = drag (juce::ModifierKeys (juce::ModifierKeys::commandModifier), 40.0f);

    CHECK (normal == Approx (100.0f * 40.0f / 200.0f).margin (0.6f)); // 200 px for the full range
    CHECK (fineShift == Approx (normal * 0.1f).margin (0.2f));
    CHECK (fineCtrl == Approx (normal * 0.1f).margin (0.2f));
    CHECK (fineShift > 0.0f);

    // Mouse wheel moves the value; fine wheel less
    const auto wheel = [&] (juce::ModifierKeys mods)
    {
        slider.setValue (50.0, juce::sendNotificationSync);
        juce::MouseWheelDetails w {};
        w.deltaY = 0.25f;
        slider.mouseWheelMove (mouseEvent (slider, { 20.0f, 20.0f }, { 20.0f, 20.0f }, mods), w);
        return f.plain ("oscAPosition") - 50.0f;
    };

    const auto wheelNormal = wheel ({});
    const auto wheelFine = wheel (juce::ModifierKeys (juce::ModifierKeys::shiftModifier));
    CHECK (wheelNormal > 0.0f);
    CHECK (wheelFine > 0.0f);
    CHECK (wheelFine < wheelNormal * 0.5f);
}

TEST_CASE ("UiFoundation: modulation ring properties and drawing", "[plugin][editor][ui]")
{
    ParamFixture f;
    ui::Knob knob (f.apvts(), "filterResonance", "Reso");
    knob.setBounds (0, 0, 70, 84);
    f.setPlain ("filterResonance", 50.0f);

    const auto before = render (knob, f.lookAndFeel);
    CHECK (countPixels (before, ui::theme::modulation) == 0);
    CHECK (countPixels (before, ui::theme::accent) > 20); // value arc

    knob.setModulationRange ({ 0.5, 0.9 });
    CHECK (static_cast<double> (knob.getSlider().getProperties()["modStart"]) == Approx (0.5));
    CHECK (static_cast<double> (knob.getSlider().getProperties()["modEnd"]) == Approx (0.9));
    CHECK (knob.getModulationRange() == juce::Range<double> (0.5, 0.9));

    const auto after = render (knob, f.lookAndFeel);
    CHECK (countPixels (after, ui::theme::modulation) > 10);

    knob.setModulationRange ({});
    CHECK (countPixels (render (knob, f.lookAndFeel), ui::theme::modulation) == 0);
}

TEST_CASE ("UiFoundation: ChoiceChip for choices and bools", "[plugin][editor][ui]")
{
    ParamFixture f;
    ui::ChoiceChip mode (f.apvts(), "playMode", "Mode");
    ui::ChoiceChip on (f.apvts(), "oscBOn", {}, ui::ChoiceChip::ToggleStyle::power);
    ui::ChoiceChip retrig (f.apvts(), "lfo1Retrig", "Retrig");
    mode.setBounds (0, 0, 100, ui::theme::chipCaptionHeight + ui::theme::chipHeight);
    on.setBounds (0, 0, 18, 18);
    retrig.setBounds (0, 0, 90, 22);

    REQUIRE (mode.getComboBox() != nullptr);
    CHECK (mode.getToggle() == nullptr);
    CHECK (mode.getComponentID() == "playMode");
    CHECK (mode.getComboBox()->getNumItems() == 3);
    CHECK (mode.getDisplayText() == "Legato");
    CHECK (mode.getComboBox()->getHeight() == ui::theme::chipHeight); // below the caption
    CHECK (mode.getCaption() == "Mode");

    // parameter -> chip, chip -> parameter
    f.setPlain ("playMode", 2.0f);
    CHECK (mode.getDisplayText() == "Poly");
    mode.getComboBox()->setSelectedItemIndex (0, juce::sendNotificationSync);
    CHECK (f.plain ("playMode") == Approx (0.0f));

    REQUIRE (on.getToggle() != nullptr);
    CHECK (on.getComboBox() == nullptr);
    CHECK (on.getToggle()->getProperties()["vsStyle"].toString() == "power");
    CHECK_FALSE (on.getToggle()->getToggleState());
    on.getToggle()->setToggleState (true, juce::sendNotificationSync);
    CHECK (f.plain ("oscBOn") == Approx (1.0f));
    f.setPlain ("oscBOn", 0.0f);
    CHECK_FALSE (on.getToggle()->getToggleState());

    CHECK (retrig.getDisplayText() == "Retrig");
    CHECK (retrig.getToggle()->getProperties()["vsStyle"].toString() == "chip");

    // Every style renders
    for (auto* c : std::initializer_list<juce::Component*> { &mode, &on, &retrig })
        CHECK (render (*c, f.lookAndFeel).isValid());

    f.setPlain ("oscBOn", 1.0f);
    CHECK (countPixels (render (on, f.lookAndFeel), ui::theme::accent) > 5);
}

TEST_CASE ("UiFoundation: SectionPanel content area, header and power slots", "[plugin][editor][ui]")
{
    ParamFixture f;
    ui::SectionPanel section ("OSC A");
    section.setSubtitle ("MAIN");
    juce::TextButton header ("table");
    juce::ToggleButton power;
    section.setHeaderComponent (&header);
    section.setHeaderWidth (120);
    section.setPowerComponent (&power);
    section.setBounds (0, 0, 300, 200);

    const auto content = section.getContentBounds();
    CHECK (section.getLocalBounds().contains (content));
    CHECK (content.getY() >= ui::SectionPanel::titleBandHeight);
    CHECK (content.getX() == ui::theme::sectionPadding);
    CHECK (header.getParentComponent() == &section);
    CHECK (header.getWidth() == 120);
    CHECK (header.getRight() == 300 - ui::theme::sectionPadding);
    CHECK (header.getBottom() <= ui::SectionPanel::titleBandHeight);
    CHECK (power.getX() == ui::theme::sectionPadding);
    CHECK (power.getWidth() == power.getHeight());
    CHECK_FALSE (power.getBounds().intersects (header.getBounds()));

    CHECK (section.isActive());
    section.setActive (false);
    CHECK_FALSE (section.isActive());
    CHECK (render (section, f.lookAndFeel).isValid());
}

TEST_CASE ("UiFoundation: WavetableView draws the frame at the position and caches its path", "[plugin][editor][ui]")
{
    ParamFixture f;
    ui::WavetableView view;
    view.setBounds (0, 0, 320, 60);

    // No table: "Loading..." and a flat line
    CHECK (view.getShownFrame() == -1);
    CHECK (render (view, f.lookAndFeel).isValid());

    auto built = synth::FactoryWavetables::build (synth::FactoryTable::basicShapes);
    REQUIRE (built.table != nullptr);
    std::shared_ptr<const synth::Wavetable> table = std::move (built.table);
    view.setTable (table);
    CHECK (view.getTable() == table.get());

    view.setPosition (0.5f);
    const auto image = render (view, f.lookAndFeel);
    // The waveform itself (above the position bar): at least one accent pixel per column on average
    CHECK (countPixels (image.getClippedImage ({ 0, 14, 320, 34 }), ui::theme::accent, 60) > 300);
    const auto rebuilds = view.getPathRebuildCount();
    CHECK (rebuilds >= 1);
    CHECK (view.getShownFrame() == juce::roundToInt (0.5f * static_cast<float> (table->getNumFrames() - 1)));

    // Within the tolerance: no rebuild; beyond: rebuild
    view.setPosition (0.503f);
    render (view, f.lookAndFeel);
    CHECK (view.getPathRebuildCount() == rebuilds);
    view.setPosition (0.6f);
    render (view, f.lookAndFeel);
    CHECK (view.getPathRebuildCount() == rebuilds + 1);

    // Clamped
    view.setPosition (2.0f);
    CHECK (view.getPosition() == 1.0f);
    CHECK (view.getShownFrame() == table->getNumFrames() - 1);

    // Error messages in the warning colour
    view.setMessage ("Not a WAV file", true);
    CHECK (view.getMessage() == "Not a WAV file");
    CHECK (countPixels (render (view, f.lookAndFeel), ui::theme::warning, 30) > 5);
}

TEST_CASE ("UiFoundation: WavetableView redraws a new table that reuses a freed table's address (review 0.4 R2-5)",
           "[plugin][editor][ui]")
{
    ParamFixture f;
    ui::WavetableView view;
    view.setBounds (0, 0, 320, 60);

    auto first = synth::FactoryWavetables::build (synth::FactoryTable::basicShapes);
    auto second = synth::FactoryWavetables::build (synth::FactoryTable::pwm);
    auto third = synth::FactoryWavetables::build (synth::FactoryTable::harmonicSweep);
    REQUIRE ((first.table != nullptr && second.table != nullptr && third.table != nullptr));

    // One storage place for table 1 and table 3: the second table object gets the first one's address, as an
    // allocator may hand out a freed block again (the view never owned these, the deleters do nothing)
    std::optional<synth::Wavetable> place;
    const auto noDelete = [] (const synth::Wavetable*) {};
    place.emplace (*first.table);
    view.setTable (std::shared_ptr<const synth::Wavetable> (&*place, noDelete));
    render (view, f.lookAndFeel);
    const auto rebuilds = view.getPathRebuildCount();

    view.setTable (std::shared_ptr<const synth::Wavetable> (std::move (second.table))); // not painted (hidden page)
    place.reset();
    place.emplace (*third.table);
    view.setTable (std::shared_ptr<const synth::Wavetable> (&*place, noDelete));
    render (view, f.lookAndFeel);
    CHECK (view.getPathRebuildCount() == rebuilds + 1);
    view.setTable (nullptr);
}

TEST_CASE ("UiFoundation: VsLookAndFeel draws the old pages' widgets", "[plugin][editor][ui]")
{
    ParamFixture f;
    juce::Component page;
    page.setSize (400, 300);
    juce::Slider linear (juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft);
    juce::Slider incDec (juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft);
    juce::ComboBox combo;
    combo.addItemList ({ "One", "Two" }, 1);
    combo.setSelectedId (1);
    juce::ToggleButton toggle ("Legato");
    toggle.setToggleState (true, juce::dontSendNotification);
    juce::TextButton button ("Voice");
    button.setClickingTogglesState (true);
    button.setToggleState (true, juce::dontSendNotification);
    juce::Label label ({}, "Caption");

    int y = 0;

    for (auto* c : std::initializer_list<juce::Component*> { &linear, &incDec, &combo, &toggle, &button, &label })
    {
        page.addAndMakeVisible (c);
        c->setBounds (10, y + 5, 300, 28);
        y += 40;
    }

    const auto image = render (page, f.lookAndFeel);
    CHECK (image.isValid());
    CHECK (countPixels (image, ui::theme::accent) > 50); // toggle switch, slider fill, the "on" page button

    // Popup menu and tooltip drawing (no window needed)
    // Software images and scoped Graphics: on Windows a default image may be a Direct2D image whose pixels only
    // arrive when its Graphics context is destroyed
    juce::Image menuImage (juce::Image::ARGB, 200, 100, true, juce::SoftwareImageType());
    {
        juce::Graphics g (menuImage);
        f.lookAndFeel.drawPopupMenuBackground (g, 200, 100);
        f.lookAndFeel.drawPopupMenuItem (g, { 0, 0, 200, 24 }, false, true, true, true, true, "Item", "Ctrl+X", nullptr, nullptr);
        f.lookAndFeel.drawPopupMenuItem (g, { 0, 24, 200, 8 }, true, true, false, false, false, {}, {}, nullptr, nullptr);
        f.lookAndFeel.drawPopupMenuSectionHeader (g, { 0, 32, 200, 24 }, "Factory");
    }
    juce::Image tipImage (juce::Image::ARGB, 200, 40, true, juce::SoftwareImageType());
    {
        juce::Graphics tipGraphics (tipImage);
        f.lookAndFeel.drawTooltip (tipGraphics, "A tooltip text that is long enough to wrap onto a second line maybe", 200, 40);
    }
    CHECK (countPixels (tipImage, ui::theme::text, 60) > 10);
    CHECK (f.lookAndFeel.getTooltipBounds ("Tip", { 100, 100 }, { 0, 0, 800, 600 }).getWidth() > 10);
    CHECK (countPixels (menuImage, ui::theme::accent) > 3); // the tick
}
