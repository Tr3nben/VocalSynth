#pragma once

#include "PresetManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>

namespace vocalsynth::ui
{

/** Preset bar (docs/SYNTH-DESIGN.md 20.5): [<] [name (* when modified) - click: menu by category] [>]
    [Save]. The menu lists the factory presets grouped by category, then "User" with the user presets, then
    "Init". Save asks for a name and category (AlertWindow, async) and calls
    PresetManager::saveUserPreset. Refreshes the name with a 10 Hz timer (modified flag). Component IDs:
    "presetPrev", "presetName", "presetNext", "presetSave". Message thread. Owner: agent E.

    Details (agent E): menu = "Init" on top, a "Factory" header with one sub-menu per category, a "User"
    sub-menu (user presets sorted by name, category on the right), then "Save As...", "Rename ...",
    "Delete ..." (current preset, user presets only), "Rescan User Presets", "Show Preset Folder". Saving over
    an existing file asks first. Every load (menu, < >) calls onPresetLoaded afterwards (the integration
    calls updateHostDisplay (programChanged) there, CLAP). Dialogs go through the std::function hooks below
    (default: async AlertWindows; the tests replace them). */
class PresetBar : public juce::Component, private juce::Timer
{
public:
    explicit PresetBar (PresetManager& presets);
    ~PresetBar() override;

    void resized() override;

    /** Called on the message thread after a preset was loaded through the bar. */
    std::function<void()> onPresetLoaded;

    //==============================================================================
    // Dialog hooks (replaceable for tests). Callbacks may be called later (async) or never (cancel).

    /** Asks for name + category; onOk (name, category). */
    std::function<void (const juce::String& suggestedName, PresetCategory suggestedCategory,
                        std::function<void (const juce::String&, PresetCategory)> onOk)> askSaveDetails;
    /** Asks for a new name; onOk (newName). */
    std::function<void (const juce::String& currentName, std::function<void (const juce::String&)> onOk)> askRename;
    /** Yes / no question; onResult (true = yes). */
    std::function<void (const juce::String& title, const juce::String& message, const juce::String& yesText,
                        std::function<void (bool)> onResult)> askConfirm;
    /** Shows an error / info message. */
    std::function<void (const juce::String& title, const juce::String& message)> showMessage;

    //==============================================================================
    // Actions (used by the buttons and the menu; public for the tests)

    /** Menu item IDs: presets are presetItemBase + index into getPresets() at menu creation. */
    enum MenuIds
    {
        presetItemBase = 1,
        saveAsItem = 100001,
        renameItem,
        deleteItem,
        rescanItem,
        showFolderItem
    };

    juce::PopupMenu createMenu();
    void handleMenuResult (int itemId);

    void pressPrevious();
    void pressNext();
    void pressSave();

    /** Save flow after the name dialog: empty name -> message; existing file -> askConfirm; then save. */
    void saveAs (const juce::String& name, PresetCategory category);

    /** The user preset that is current (same name as the current preset), if any. */
    std::optional<PresetInfo> getCurrentUserPreset() const;

    /** The name button's text ("Voice Lead *"). */
    juce::String getNameText() const { return nameButton.getButtonText(); }

    /** Timer tick now (tests). */
    void refresh();

    static constexpr int timerHz = 10;

private:
    void timerCallback() override;
    void showMenu();
    void loaded();
    void installDefaultDialogs();

    PresetManager& presets;
    juce::TextButton previousButton { "<" };
    juce::TextButton nameButton;
    juce::TextButton nextButton { ">" };
    juce::TextButton saveButton { "Save" };

    juce::Array<PresetInfo> menuPresets; ///< snapshot of getPresets() for the open menu
    std::unique_ptr<juce::AlertWindow> dialog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBar)
};

} // namespace vocalsynth::ui
