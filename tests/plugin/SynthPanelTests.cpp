// SynthPanel: both variants, every synth parameter has exactly one control, layout inside bounds, preset bar.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent E (UI).

#include "Parameters.h"
#include "PluginProcessor.h"
#include "PresetManager.h"
#include "SynthDataTestUtils.h"
#include "SynthParameters.h"
#include "ui/SynthPanel.h"
#include "ui/VsLookAndFeel.h"
#include "vocalsynth/synth/FactoryWavetables.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <vector>

using namespace vocalsynth;
namespace testdata = vocalsynth::testdata;

namespace
{
/** A processor (APVTS) with the plugin-data objects next to it, wired as the integration does
    (docs/SYNTH-DESIGN.md 17), and the panel on top. User presets go to a temp folder. */
struct PanelFixture
{
    testdata::ScopedTempFolder temp;
    VocalSynthProcessor processor;
    WavetableStore store;
    WavetableWorker worker { store };
    PresetManager presets { processor.getParameters(), store, worker };
    ExpressionPublisher expression;
    ui::VsLookAndFeel lookAndFeel;
    int presetLoadedCalls = 0;
    std::unique_ptr<ui::SynthPanel> panel;

    explicit PanelFixture (Variant variant = Variant::fx)
        : processor (variant)
    {
        presets.setUserFolder (temp.folder.getChildFile ("Presets"));
        juce::String error;
        worker.loadFactoryNow (0, "basic-shapes", error);
        worker.loadFactoryNow (1, "basic-shapes", error);
        presets.setCurrent ("Init", false);

        ui::SynthPanelContext context { processor.getParameters(), presets, store, worker, expression };
        context.dspLoadText = [] { return juce::String ("Synth DSP 0.6 %"); };
        context.onPresetLoaded = [this] { ++presetLoadedCalls; };
        panel = std::make_unique<ui::SynthPanel> (context);
        panel->setLookAndFeel (&lookAndFeel);
        panel->setBounds (0, 0, ui::SynthPanel::designWidth, ui::SynthPanel::designHeight);
        panel->runTimerTickForTests();
    }

    ~PanelFixture()
    {
        panel->setLookAndFeel (nullptr);
        panel.reset();
        worker.stop();
    }

    juce::RangedAudioParameter& param (const juce::String& id)
    {
        auto* p = processor.getParameters().getParameter (id);
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

void forEachDescendant (juce::Component& root, const std::function<void (juce::Component&)>& visit)
{
    for (auto* child : root.getChildren())
    {
        visit (*child);
        forEachDescendant (*child, visit);
    }
}

bool isParameterControl (juce::Component& c)
{
    return dynamic_cast<ui::Knob*> (&c) != nullptr || dynamic_cast<ui::ChoiceChip*> (&c) != nullptr;
}

/** Writes a PNG of the component (scale = rendering scale) when VOCALSYNTH_UI_SCREENSHOT_DIR is set. */
void writeScreenshot (juce::Component& component, const juce::String& fileName, float scale)
{
    const auto* dir = std::getenv ("VOCALSYNTH_UI_SCREENSHOT_DIR");

    if (dir == nullptr)
        return;

    const auto image = component.createComponentSnapshot (component.getLocalBounds(), true, scale);
    const auto file = juce::File (juce::String (dir)).getChildFile (fileName);
    file.deleteFile();
    juce::FileOutputStream out (file);
    REQUIRE (out.openedOk());
    juce::PNGImageFormat png;
    REQUIRE (png.writeImageToStream (image, out));
}
} // namespace

TEST_CASE ("SynthPanel: screenshots", "[plugin][editor][synthpanel][.screenshot]")
{
    PanelFixture f;
    // A few non-default values so the page shows arcs, rings and dimming
    f.setPlain ("filterCutoff", 2400.0f);
    f.setPlain ("filterResonance", 35.0f);
    f.setPlain ("mod1Source", 3.0f);  // LFO 1
    f.setPlain ("mod1Dest", 11.0f);   // Filter Cutoff
    f.setPlain ("mod1Amount", 40.0f);
    f.setPlain ("mod2Source", 8.0f);  // Voice Level
    f.setPlain ("mod2Dest", 1.0f);    // Osc A Position
    f.setPlain ("mod2Amount", 30.0f);
    f.setPlain ("oscAUnison", 4.0f);
    f.setPlain ("oscAPosition", 62.0f);
    f.setPlain ("lfo2Sync", 9.0f);
    f.setPlain ("chorusOn", 1.0f);
    f.setPlain ("synthOn", 1.0f);
    f.expression.publish ({ 0, 0.7f, 0.45f, 0.2f, -23.0f });
    f.panel->runTimerTickForTests();

    writeScreenshot (*f.panel, "ui-synth-100.png", 1.0f);
    writeScreenshot (*f.panel, "ui-synth-200.png", 2.0f);

    f.panel->setBounds (0, 0, 825, 503);
    writeScreenshot (*f.panel, "ui-synth-75.png", 1.0f);
}

namespace
{
/** The parameter controls (Knob / ChoiceChip) below the panel, by component ID. */
std::map<juce::String, std::vector<juce::Component*>> collectControls (juce::Component& root)
{
    std::map<juce::String, std::vector<juce::Component*>> controls;
    forEachDescendant (root, [&] (juce::Component& c)
    {
        if (isParameterControl (c))
            controls[c.getComponentID()].push_back (&c);
    });
    return controls;
}

/** Inside each container, every visible child lies within the parent and visible siblings do not overlap.
    Recurses into the content and the sections (not into the widgets themselves). */
void checkNoOverlaps (juce::Component& parent)
{
    std::vector<juce::Component*> visible;

    for (auto* child : parent.getChildren())
        if (child->isVisible())
        {
            CAPTURE (parent.getComponentID(), child->getComponentID(), child->getBounds().toString());
            CHECK (parent.getLocalBounds().contains (child->getBounds()));
            CHECK_FALSE (child->getBounds().isEmpty());
            visible.push_back (child);
        }

    for (std::size_t i = 0; i < visible.size(); ++i)
        for (std::size_t j = i + 1; j < visible.size(); ++j)
        {
            CAPTURE (parent.getComponentID(), visible[i]->getComponentID(), visible[j]->getComponentID(),
                     visible[i]->getBounds().toString(), visible[j]->getBounds().toString());
            CHECK_FALSE (visible[i]->getBounds().intersects (visible[j]->getBounds()));
        }

    for (auto* child : visible)
        if (dynamic_cast<ui::SectionPanel*> (child) != nullptr)
            checkNoOverlaps (*child);
}

juce::File writeFile (const juce::File& folder, const juce::String& name, const juce::MemoryBlock& bytes)
{
    const auto file = folder.getChildFile (name);
    REQUIRE (file.replaceWithData (bytes.getData(), bytes.getSize()));
    return file;
}

/** WAV bytes with a "clm " chunk (Serum style) inserted after "fmt ". */
juce::MemoryBlock withClmChunk (const juce::MemoryBlock& wav, const char* text)
{
    const auto* bytes = static_cast<const char*> (wav.getData());
    juce::MemoryOutputStream out;
    out.write (bytes, 36); // RIFF header + fmt chunk
    const auto length = static_cast<int> (std::strlen (text));
    out.write ("clm ", 4);
    out.writeInt (length);
    out.write (text, static_cast<size_t> (length));

    if ((length & 1) != 0)
        out.writeByte (0);

    out.write (bytes + 36, wav.getSize() - 36);
    auto block = out.getMemoryBlock();
    const auto riffSize = static_cast<int> (block.getSize() - 8);
    block.copyFrom (&riffSize, 4, 4);
    return block;
}

juce::PopupMenu::Item* findItem (juce::PopupMenu& menu, const std::function<bool (const juce::PopupMenu::Item&)>& match)
{
    for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
        if (match (it.getItem()))
            return &it.getItem();

    return nullptr;
}
} // namespace

TEST_CASE ("SynthPanel: every synth parameter has exactly one attached control", "[plugin][editor][synthpanel]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    CAPTURE (variant == Variant::fx ? "fx" : "instrument");
    PanelFixture f (variant);
    const auto controls = collectControls (*f.panel);
    const auto ids = params::getSynthParameterIds();
    REQUIRE (ids.size() == params::numSynthParameters);

    for (const auto& id : ids)
    {
        CAPTURE (id);
        const auto found = controls.find (id);
        REQUIRE (found != controls.end());
        CHECK (found->second.size() == 1);
        auto* control = found->second.front();
        CHECK (f.panel->findControlFor (id) == control);
        CHECK (control->isVisible());

        auto& p = f.param (id);
        const auto range = p.getNormalisableRange();

        if (auto* knob = dynamic_cast<ui::Knob*> (control))
        {
            CHECK (dynamic_cast<juce::AudioParameterChoice*> (&p) == nullptr);
            CHECK (dynamic_cast<juce::AudioParameterBool*> (&p) == nullptr);

            // parameter -> control
            const auto target = range.convertFrom0to1 (0.73f);
            p.setValueNotifyingHost (range.convertTo0to1 (target));
            CHECK (knob->getSlider().getValue() == Catch::Approx (f.plain (id)).margin (1e-3 * (range.end - range.start)));

            // control -> parameter
            knob->getSlider().setValue (range.convertFrom0to1 (0.25f), juce::sendNotificationSync);
            CHECK (f.plain (id) == Catch::Approx (range.snapToLegalValue (range.convertFrom0to1 (0.25f)))
                                       .margin (1e-3 * (range.end - range.start)));
            CHECK (knob->getSlider().getTooltip().isNotEmpty());
        }
        else if (auto* chip = dynamic_cast<ui::ChoiceChip*> (control))
        {
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&p))
            {
                REQUIRE (chip->getComboBox() != nullptr);
                CHECK (chip->getComboBox()->getNumItems() == choice->choices.size());
                const auto last = choice->choices.size() - 1;
                p.setValueNotifyingHost (p.convertTo0to1 (static_cast<float> (last)));
                CHECK (chip->getComboBox()->getSelectedItemIndex() == last);
                chip->getComboBox()->setSelectedItemIndex (0, juce::sendNotificationSync);
                CHECK (f.plain (id) == Catch::Approx (0.0f));
                CHECK (chip->getComboBox()->getTooltip().isNotEmpty());
            }
            else
            {
                REQUIRE (dynamic_cast<juce::AudioParameterBool*> (&p) != nullptr);
                REQUIRE (chip->getToggle() != nullptr);
                p.setValueNotifyingHost (1.0f);
                CHECK (chip->getToggle()->getToggleState());
                chip->getToggle()->setToggleState (false, juce::sendNotificationSync);
                CHECK (f.plain (id) == Catch::Approx (0.0f));
                CHECK (chip->getToggle()->getTooltip().isNotEmpty());
            }
        }
        else
        {
            FAIL ("unknown control type");
        }
    }

    // Controls for nothing else but the synth parameters and the voice's bend range (no stray IDs)
    for (const auto& [id, list] : controls)
    {
        CAPTURE (id);
        CHECK ((ids.contains (id) || id == params::bendRange.getParamID()));
    }

    // Design-specific placements (docs/SYNTH-DESIGN.md 20.6, 23)
    auto* gain = dynamic_cast<ui::Knob*> (f.panel->findControlFor ("ampGain"));
    REQUIRE (gain != nullptr);
    CHECK (gain->getLabelText() == "Gain");
    auto* envSection = dynamic_cast<ui::SectionPanel*> (gain->getParentComponent());
    REQUIRE (envSection != nullptr);
    CHECK (envSection->getTitle() == "ENV 1");

    auto* dest = dynamic_cast<ui::ChoiceChip*> (f.panel->findControlFor ("mod1Dest"));
    REQUIRE (dest != nullptr);
    bool hasPitch = false;

    for (int i = 0; i < dest->getComboBox()->getNumItems(); ++i)
        hasPitch = hasPitch || dest->getComboBox()->getItemText (i) == "Pitch";

    CHECK (hasPitch);
    CHECK (dynamic_cast<ui::SectionPanel*> (f.panel->findControlFor ("synthOn")->getParentComponent())->getTitle() == "MASTER");
}

TEST_CASE ("SynthPanel: layout at 75 / 100 / 150 / 200 % keeps every control inside and apart", "[plugin][editor][synthpanel]")
{
    PanelFixture f;
    const auto scale = GENERATE (0.75f, 1.0f, 1.25f, 1.5f, 2.0f);
    CAPTURE (scale);

    // Mode 1: the panel itself gets scaled bounds (it scales its content with a transform)
    const auto w = juce::roundToInt (ui::SynthPanel::designWidth * scale);
    const auto h = juce::roundToInt (ui::SynthPanel::designHeight * scale);
    f.panel->setBounds (0, 0, w, h);
    f.panel->runTimerTickForTests();

    auto& content = f.panel->getContent();
    CHECK (content.getWidth() == ui::SynthPanel::designWidth);
    CHECK (content.getHeight() == ui::SynthPanel::designHeight);
    CHECK (f.panel->getLocalBounds().expanded (1).contains (content.getBoundsInParent()));
    CHECK (content.getBoundsInParent().getWidth() >= w - 2);

    checkNoOverlaps (content);

    // Every control is inside the panel in panel coordinates and big enough to use
    for (const auto& [id, list] : collectControls (*f.panel))
    {
        CAPTURE (id);
        auto* c = list.front();
        const auto inPanel = f.panel->getLocalArea (c, c->getLocalBounds());
        CHECK (f.panel->getLocalBounds().expanded (1).contains (inPanel));

        if (auto* knob = dynamic_cast<ui::Knob*> (c); knob != nullptr && knob->getStyle() == ui::Knob::Style::rotary)
        {
            CHECK (c->getWidth() >= 48);
            CHECK (knob->getSlider().getWidth() >= 30); // rotary diameter in design pixels
        }
        else
        {
            CHECK (c->getHeight() >= 18);
        }
    }

    // Renders at that size
    const auto image = f.panel->createComponentSnapshot (f.panel->getLocalBounds());
    CHECK (image.getWidth() == w);

    // Mode 2: the editor scales a parent with a transform and the panel stays at the design size
    juce::Component editorContent;
    editorContent.setBounds (0, 0, ui::SynthPanel::designWidth, ui::SynthPanel::designHeight);
    editorContent.setTransform (juce::AffineTransform::scale (scale));
    f.panel->setBounds (0, 0, ui::SynthPanel::designWidth, ui::SynthPanel::designHeight);
    editorContent.addAndMakeVisible (*f.panel);
    CHECK (content.getTransform().isIdentity());
    CHECK (content.getBoundsInParent() == f.panel->getLocalBounds());
    editorContent.removeChildComponent (f.panel.get());
}

TEST_CASE ("SynthPanel: preset bar loads, marks modified, saves, renames and deletes", "[plugin][editor][synthpanel]")
{
    PanelFixture f;
    auto& bar = f.panel->getPresetBar();
    CHECK (bar.getNameText() == "Init");

    for (const auto* id : { "presetPrev", "presetName", "presetNext", "presetSave" })
    {
        CAPTURE (id);
        bool found = false;
        forEachDescendant (bar, [&] (juce::Component& c) { found = found || c.getComponentID() == id; });
        CHECK (found);
    }

    // Dialog hooks for the test
    std::vector<juce::String> questions, messages;
    bool answer = true;
    juce::String saveName = "My Lead";
    bar.askConfirm = [&] (const juce::String& title, const juce::String&, const juce::String&, std::function<void (bool)> done)
    {
        questions.push_back (title);
        done (answer);
    };
    bar.showMessage = [&] (const juce::String& title, const juce::String& message) { messages.push_back (title + ": " + message); };
    bar.askSaveDetails = [&] (const juce::String&, PresetCategory, std::function<void (const juce::String&, PresetCategory)> ok)
    {
        ok (saveName, PresetCategory::lead);
    };
    bar.askRename = [&] (const juce::String&, std::function<void (const juce::String&)> ok) { ok ("Renamed Lead"); };

    // Next / previous walk the list and report the load
    bar.pressNext();
    const auto presetsList = f.presets.getPresets();
    REQUIRE (presetsList.size() > 1);
    CHECK (bar.getNameText() == presetsList[1].name);
    CHECK (f.presetLoadedCalls == 1);
    bar.pressPrevious();
    CHECK (bar.getNameText() == "Init");
    CHECK (f.presetLoadedCalls == 2);

    // A parameter change shows the modified mark on the next tick
    f.setPlain ("filterCutoff", 1234.0f);
    bar.refresh();
    CHECK (bar.getNameText() == "Init *");

    // Menu: Init on top, categories as sub-menus, user section; picking an item loads it
    auto menu = bar.createMenu();
    auto* init = findItem (menu, [] (const juce::PopupMenu::Item& i) { return i.text == "Init"; });
    REQUIRE (init != nullptr);
    CHECK (init->isTicked);
    juce::StringArray topLevel;

    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        topLevel.add (it.getItem().text);

    CHECK (topLevel.contains ("Lead"));
    CHECK (topLevel.contains ("User Presets"));
    const auto lastFactory = presetsList[presetsList.size() - 1];
    auto* item = findItem (menu, [&] (const juce::PopupMenu::Item& i) { return i.text == lastFactory.name && i.itemID > 0; });
    REQUIRE (item != nullptr);
    bar.handleMenuResult (item->itemID);
    CHECK (bar.getNameText() == lastFactory.name);
    CHECK (f.presetLoadedCalls == 3);

    // Save (new file), then save again under the same name: asks before replacing
    bar.pressSave();
    CHECK (f.presets.getUserPresetFile ("My Lead").existsAsFile());
    CHECK (bar.getNameText() == "My Lead");
    CHECK (questions.empty());
    bar.pressSave();
    CHECK (questions.size() == 1);
    answer = false;
    bar.pressSave();
    CHECK (questions.size() == 2);
    CHECK (bar.getCurrentUserPreset().has_value());

    // Empty name -> message, nothing saved
    saveName = "   ";
    bar.pressSave();
    CHECK (messages.size() == 1);

    // The user preset appears in the menu (ticked), rename and delete are enabled for it
    menu = bar.createMenu();
    auto* user = findItem (menu, [] (const juce::PopupMenu::Item& i) { return i.text == "My Lead"; });
    REQUIRE (user != nullptr);
    CHECK (user->isTicked);
    CHECK (user->shortcutKeyDescription == "Lead");
    auto* rename = findItem (menu, [] (const juce::PopupMenu::Item& i) { return i.itemID == ui::PresetBar::renameItem; });
    REQUIRE (rename != nullptr);
    CHECK (rename->isEnabled);

    bar.handleMenuResult (ui::PresetBar::renameItem);
    CHECK_FALSE (f.presets.getUserPresetFile ("My Lead").existsAsFile());
    CHECK (f.presets.getUserPresetFile ("Renamed Lead").existsAsFile());

    answer = true;
    f.presets.load (*bar.getCurrentUserPreset()); // current = the renamed preset
    menu = bar.createMenu();
    bar.handleMenuResult (ui::PresetBar::deleteItem);
    CHECK_FALSE (f.presets.getUserPresetFile ("Renamed Lead").existsAsFile());
    CHECK (messages.size() == 1);

    // Factory preset current: rename / delete disabled
    f.presets.loadInit();
    menu = bar.createMenu();
    rename = findItem (menu, [] (const juce::PopupMenu::Item& i) { return i.itemID == ui::PresetBar::renameItem; });
    REQUIRE (rename != nullptr);
    CHECK_FALSE (rename->isEnabled);
}

TEST_CASE ("SynthPanel: table menu requests a factory table and marks the preset modified", "[plugin][editor][synthpanel]")
{
    PanelFixture f;
    CHECK (f.panel->getTableButtonText (0) == "Basic Shapes");
    CHECK (f.panel->getWavetableView (0).getTable() != nullptr);
    CHECK_FALSE (f.presets.isModified());

    auto menu = f.panel->createTableMenu (1);
    int factoryItems = 0;
    int pwmId = 0;

    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        const auto& item = it.getItem();

        if (item.itemID >= ui::SynthPanel::factoryTableItemBase && item.itemID < ui::SynthPanel::importItem)
        {
            ++factoryItems;

            if (item.text == "PWM")
                pwmId = item.itemID;

            if (item.text == "Basic Shapes")
                CHECK (item.isTicked);
        }
    }

    CHECK (factoryItems == synth::numFactoryTables);
    CHECK (findItem (menu, [] (const juce::PopupMenu::Item& i) { return i.itemID == ui::SynthPanel::importItem; }) != nullptr);
    REQUIRE (pwmId > 0);

    f.panel->handleTableMenuResult (1, pwmId);
    CHECK (f.presets.isModified());
    CHECK (f.worker.getRequestedReference (1).reference.toRefString() == "factory:pwm");
    REQUIRE (f.worker.waitUntilIdle (10000));
    f.panel->runTimerTickForTests();
    CHECK (f.store.getSlot (1).reference.id == "pwm");
    CHECK (f.panel->getTableButtonText (1) == "PWM");
    REQUIRE (f.panel->getWavetableView (1).getTable() != nullptr);
    CHECK (juce::String (f.panel->getWavetableView (1).getTable()->getName()) == "PWM");
    CHECK (f.panel->getWavetableView (1).getMessage().isEmpty());
    CHECK (f.panel->getPresetBar().getNameText() == "Init");
    f.panel->getPresetBar().refresh();
    CHECK (f.panel->getPresetBar().getNameText() == "Init *");

    // Choosing the table that is already requested changes nothing
    f.presets.setCurrent ("Init", false);
    f.panel->chooseFactoryTable (1, "pwm");
    CHECK_FALSE (f.presets.isModified());

    // Slot 0 untouched; the wavetable view follows the position parameter
    CHECK (f.store.getSlot (0).reference.id == "basic-shapes");
    f.setPlain ("oscAPosition", 25.0f);
    f.panel->runTimerTickForTests();
    CHECK (f.panel->getWavetableView (0).getPosition() == Catch::Approx (0.25f));
}

TEST_CASE ("SynthPanel: WAV import path, errors and the frame-size question", "[plugin][editor][synthpanel]")
{
    PanelFixture f;
    const auto folder = f.temp.folder.getChildFile (juce::String::fromUTF8 ("Tab\xc3\xa9llen mit Leerzeichen"));
    folder.createDirectory();

    // Good import: requested at once, published by the worker, shown with the file name
    const auto good = writeFile (folder, "My Table.wav", testdata::makeWav16 (testdata::morphFrames (8, 2)));
    CHECK_FALSE (ui::SynthPanel::needsFrameSizeChoice (good)); // 8 x 2048
    f.panel->importTable (0, good);
    CHECK (f.presets.isModified());
    REQUIRE (f.worker.waitUntilIdle (10000));
    f.panel->runTimerTickForTests();
    CHECK (f.store.getSlot (0).reference.kind == WavetableReference::Kind::user);
    CHECK (f.panel->getTableButtonText (0) == "My Table.wav");
    CHECK (f.panel->getWavetableView (0).getMessage().isEmpty());

    auto menu = f.panel->createTableMenu (0);
    auto* current = findItem (menu, [] (const juce::PopupMenu::Item& i) { return i.itemID == ui::SynthPanel::currentUserTableItem; });
    REQUIRE (current != nullptr);
    CHECK (current->isTicked);
    CHECK (current->text == "My Table.wav");

    // Broken file: the slot keeps its table, the view shows the worker's error in the warning style
    const auto broken = writeFile (folder, "broken.wav", juce::MemoryBlock ("not a wav at all", 16));
    f.panel->importTable (0, broken);
    REQUIRE (f.worker.waitUntilIdle (10000));
    f.panel->runTimerTickForTests();
    CHECK (f.store.getSlot (0).reference.kind == WavetableReference::Kind::user);
    CHECK (f.panel->getWavetableView (0).getMessage().isNotEmpty());
    CHECK (f.panel->getWavetableView (0).getMessage() == f.worker.getStatus().lastError);

    // Frame-size question only for ambiguous files
    std::vector<float> samples (20000, 0.0f);

    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.5f * std::sin (static_cast<float> (i) * 0.01f);

    const auto ambiguous = writeFile (folder, "long.wav", testdata::makeWav16 (samples));
    const auto single = writeFile (folder, "single.wav",
                                   testdata::makeWav16 (std::vector<float> (samples.begin(), samples.begin() + 600)));
    const auto stereo2048 = writeFile (folder, "stereo.wav", testdata::makeWav16 (testdata::morphFrames (3, 1), 2));
    const auto serum = writeFile (folder, "serum.wav", withClmChunk (testdata::makeWav16 (samples), "<!>1000 00000000 wavetable"));
    const auto serumOk = writeFile (folder, "serum ok.wav", withClmChunk (testdata::makeWav16 (samples), "<!>256 00000000 wavetable"));

    CHECK (ui::SynthPanel::needsFrameSizeChoice (ambiguous));
    CHECK_FALSE (ui::SynthPanel::needsFrameSizeChoice (single));
    CHECK_FALSE (ui::SynthPanel::needsFrameSizeChoice (stereo2048));
    CHECK (ui::SynthPanel::needsFrameSizeChoice (serum)); // 1000 is not a valid clm frame size
    CHECK_FALSE (ui::SynthPanel::needsFrameSizeChoice (serumOk));
    CHECK_FALSE (ui::SynthPanel::needsFrameSizeChoice (broken));
    CHECK_FALSE (ui::SynthPanel::needsFrameSizeChoice (folder.getChildFile ("missing.wav")));

    // With an override from the frame-size menu the ambiguous file imports
    f.panel->importTable (1, ambiguous, 1024);
    REQUIRE (f.worker.waitUntilIdle (10000));
    f.panel->runTimerTickForTests();
    CHECK (f.store.getSlot (1).reference.kind == WavetableReference::Kind::user);
    CHECK (f.panel->getTableButtonText (1) == "long.wav");
}

TEST_CASE ("SynthPanel: destroying the panel while the worker builds is safe", "[plugin][editor][synthpanel]")
{
    for (int round = 0; round < 3; ++round)
    {
        PanelFixture f;
        const auto big = writeFile (f.temp.folder, "big table.wav", testdata::makeWav16 (testdata::morphFrames (256, round)));
        f.panel->importTable (0, big);
        f.panel->chooseFactoryTable (1, round == 0 ? "choir" : (round == 1 ? "fold" : "organ"));
        f.panel->runTimerTickForTests();
        f.panel->setLookAndFeel (nullptr);
        f.panel.reset(); // while the worker still reads / builds

        REQUIRE (f.worker.waitUntilIdle (20000));
        CHECK (f.store.getSlot (0).reference.kind == WavetableReference::Kind::user);

        // A new panel on the same objects picks the tables up
        ui::SynthPanelContext context { f.processor.getParameters(), f.presets, f.store, f.worker, f.expression };
        f.panel = std::make_unique<ui::SynthPanel> (context);
        f.panel->setLookAndFeel (&f.lookAndFeel);
        f.panel->runTimerTickForTests();
        CHECK (f.panel->getTableButtonText (0) == "big table.wav");
    }
}

TEST_CASE ("SynthPanel: meters, modulation rings, dimming and the DSP text", "[plugin][editor][synthpanel]")
{
    PanelFixture f;
    CHECK (f.panel->getDspText() == "Synth DSP 0.6 %");

    // Expression meters at the timer
    f.expression.publish ({ 0, 0.8f, 0.3f, 0.6f, 50.0f });
    f.panel->runTimerTickForTests();
    juce::Component* meters = nullptr;
    forEachDescendant (*f.panel, [&] (juce::Component& c) { if (c.getComponentID() == "expressionMeters") meters = &c; });
    REQUIRE (meters != nullptr);
    CHECK (meters->isVisible());

    // Modulation ring: LFO 1 -> Filter Resonance 50 % around the knob value (bipolar source)
    auto* reso = dynamic_cast<ui::Knob*> (f.panel->findControlFor ("filterResonance"));
    REQUIRE (reso != nullptr);
    CHECK (reso->getModulationRange().isEmpty());
    f.setPlain ("filterResonance", 40.0f);
    f.setPlain ("mod3Source", static_cast<float> (synth::ModSource::lfo1));
    f.setPlain ("mod3Dest", static_cast<float> (synth::ModDest::filterResonance));
    f.setPlain ("mod3Amount", 50.0f);
    f.panel->runTimerTickForTests();
    CHECK (reso->getModulationRange().getStart() == Catch::Approx (0.0).margin (1e-6));
    CHECK (reso->getModulationRange().getEnd() == Catch::Approx (0.9).margin (1e-4));

    // Unipolar source (Env 2), negative amount: range below the value
    f.setPlain ("mod3Source", static_cast<float> (synth::ModSource::env2));
    f.setPlain ("mod3Amount", -20.0f);
    f.panel->runTimerTickForTests();
    CHECK (reso->getModulationRange().getStart() == Catch::Approx (0.2).margin (1e-4));
    CHECK (reso->getModulationRange().getEnd() == Catch::Approx (0.4).margin (1e-4));

    // Cutoff: +60 semitones at 100 % (log knob)
    auto* cutoff = dynamic_cast<ui::Knob*> (f.panel->findControlFor ("filterCutoff"));
    f.setPlain ("filterCutoff", 200.0f);
    f.setPlain ("mod3Dest", static_cast<float> (synth::ModDest::filterCutoff));
    f.setPlain ("mod3Amount", 100.0f);
    f.panel->runTimerTickForTests();
    const auto& range = f.param ("filterCutoff").getNormalisableRange();
    CHECK (cutoff->getModulationRange().getEnd() == Catch::Approx (range.convertTo0to1 (6400.0f)).margin (1e-3));
    CHECK (reso->getModulationRange().isEmpty());

    // Off amount: no ring
    f.setPlain ("mod3Amount", 0.0f);
    f.panel->runTimerTickForTests();
    CHECK (cutoff->getModulationRange().isEmpty());

    // Dimming: Osc B (off by default) and a synced LFO rate
    auto* oscBLevel = f.panel->findControlFor ("oscBLevel");
    CHECK (oscBLevel->getAlpha() < 0.5f);
    f.setPlain ("oscBOn", 1.0f);
    f.panel->runTimerTickForTests();
    CHECK (oscBLevel->getAlpha() == 1.0f);
    CHECK (f.panel->findControlFor ("lfo1Rate")->getAlpha() == 1.0f);
    f.setPlain ("lfo1Sync", 3.0f);
    f.panel->runTimerTickForTests();
    CHECK (f.panel->findControlFor ("lfo1Rate")->getAlpha() < 0.5f);
    // The on switch itself is never dimmed
    f.setPlain ("oscBOn", 0.0f);
    f.panel->runTimerTickForTests();
    CHECK (f.panel->findControlFor ("oscBOn")->getAlpha() == 1.0f);
}

TEST_CASE ("SynthPanel: without a DSP text function the display is hidden", "[plugin][editor][synthpanel]")
{
    testdata::ScopedTempFolder temp;
    VocalSynthProcessor processor (Variant::instrument);
    WavetableStore store;
    WavetableWorker worker { store };
    PresetManager presets { processor.getParameters(), store, worker };
    presets.setUserFolder (temp.folder);
    ExpressionPublisher expression;
    ui::SynthPanel panel ({ processor.getParameters(), presets, store, worker, expression });
    panel.setSize (ui::SynthPanel::designWidth, ui::SynthPanel::designHeight);
    panel.runTimerTickForTests();
    CHECK (panel.getDspText().isEmpty());

    // No table published yet: the views say so
    CHECK (panel.getWavetableView (0).getTable() == nullptr);
    CHECK (panel.createComponentSnapshot (panel.getLocalBounds()).isValid());
    worker.stop();
}
