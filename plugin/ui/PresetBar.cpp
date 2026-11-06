#include "ui/PresetBar.h"

#include "ui/Theme.h"

namespace vocalsynth::ui
{

namespace
{
juce::String categoryName (PresetCategory category)
{
    const auto index = static_cast<std::size_t> (juce::jlimit (0, numPresetCategories - 1, static_cast<int> (category)));
    const auto name = presetCategoryNames[index];
    return juce::String (name.data(), name.size());
}

/** U+2026 HORIZONTAL ELLIPSIS. */
juce::String ellipsis()
{
    return juce::String::fromUTF8 ("\xe2\x80\xa6");
}
} // namespace

PresetBar::PresetBar (PresetManager& p)
    : presets (p)
{
    previousButton.setComponentID ("presetPrev");
    nameButton.setComponentID ("presetName");
    nextButton.setComponentID ("presetNext");
    saveButton.setComponentID ("presetSave");
    previousButton.getProperties().set ("vsArrow", "left");
    nextButton.getProperties().set ("vsArrow", "right");
    nameButton.getProperties().set ("vsDropdown", true);

    previousButton.setTooltip ("Previous preset");
    nextButton.setTooltip ("Next preset");
    nameButton.setTooltip ("Presets: factory presets by category, your user presets, save / rename / delete");
    saveButton.setTooltip ("Save the current sound as a user preset (Documents/VocalSynth/Presets)");

    previousButton.onClick = [this] { pressPrevious(); };
    nextButton.onClick = [this] { pressNext(); };
    nameButton.onClick = [this] { showMenu(); };
    saveButton.onClick = [this] { pressSave(); };

    for (auto* b : { &previousButton, &nameButton, &nextButton, &saveButton })
        addAndMakeVisible (*b);

    installDefaultDialogs();
    refresh(); // the 10 Hz refresh starts with setRefreshing (true): the owner shows the bar
}

void PresetBar::setRefreshing (bool shouldRefresh)
{
    if (shouldRefresh == isTimerRunning())
        return;

    if (shouldRefresh)
    {
        refresh();
        startTimerHz (timerHz);
    }
    else
    {
        stopTimer();
    }
}

PresetBar::~PresetBar()
{
    stopTimer();

    if (dialog != nullptr)
        dialog->exitModalState (0);
}

void PresetBar::resized()
{
    auto bounds = getLocalBounds();
    const auto h = bounds.getHeight();
    saveButton.setBounds (bounds.removeFromRight (64));
    bounds.removeFromRight (theme::gap);
    previousButton.setBounds (bounds.removeFromLeft (h));
    bounds.removeFromLeft (2);
    nextButton.setBounds (bounds.removeFromRight (h));
    bounds.removeFromRight (2);
    nameButton.setBounds (bounds);
}

void PresetBar::timerCallback()
{
    refresh();
}

void PresetBar::refresh()
{
    const auto text = presets.getCurrentName() + (presets.isModified() ? " *" : "");

    if (text != nameButton.getButtonText())
        nameButton.setButtonText (text);
}

void PresetBar::loaded()
{
    refresh();

    if (onPresetLoaded != nullptr)
        onPresetLoaded();
}

void PresetBar::pressPrevious()
{
    presets.loadPrevious();
    loaded();
}

void PresetBar::pressNext()
{
    presets.loadNext();
    loaded();
}

std::optional<PresetInfo> PresetBar::getCurrentUserPreset() const
{
    // By the PresetManager's identity, not by name: a user preset may carry a factory preset's name (review 0.4
    // R2-6). Only a project (setCurrent: name only) falls back to the name.
    if (presets.isCurrentFactory())
        return std::nullopt;

    const auto file = presets.getCurrentUserFile();
    const auto name = presets.getCurrentName();

    for (const auto& info : presets.getPresets())
        if (! info.isFactory && (file != juce::File() ? info.file == file : info.name == name))
            return info;

    return std::nullopt;
}

//==============================================================================
juce::PopupMenu PresetBar::createMenu()
{
    menuPresets = presets.getPresets();
    const auto current = presets.getCurrentName();
    const auto currentUser = getCurrentUserPreset();
    juce::PopupMenu menu;

    std::array<juce::PopupMenu, numPresetCategories> factoryByCategory;
    juce::PopupMenu user;
    int numUser = 0;

    for (int i = 0; i < menuPresets.size(); ++i)
    {
        const auto& info = menuPresets.getReference (i);
        const bool ticked = currentUser.has_value() ? (! info.isFactory && info.file == currentUser->file)
                                                    : (info.isFactory && info.name == current);
        juce::PopupMenu::Item item (info.name);
        item.itemID = presetItemBase + i;
        item.isTicked = ticked;

        if (info.isFactory)
        {
            if (info.category == PresetCategory::init)
                menu.addItem (item);
            else
                factoryByCategory[static_cast<std::size_t> (info.category)].addItem (item);
        }
        else
        {
            item.shortcutKeyDescription = categoryName (info.category);
            user.addItem (item);
            ++numUser;
        }
    }

    menu.addSectionHeader ("Factory");

    for (int c = 1; c < numPresetCategories; ++c)
        if (factoryByCategory[static_cast<std::size_t> (c)].getNumItems() > 0)
            menu.addSubMenu (categoryName (static_cast<PresetCategory> (c)), factoryByCategory[static_cast<std::size_t> (c)]);

    menu.addSectionHeader ("User");

    if (numUser == 0)
        user.addItem (juce::PopupMenu::Item ("No user presets yet - use Save").setEnabled (false));

    menu.addSubMenu ("User Presets", user);
    menu.addSeparator();
    menu.addItem (saveAsItem, "Save As" + ellipsis());
    const auto userName = currentUser.has_value() ? " \"" + currentUser->name + "\"" : juce::String();
    menu.addItem (renameItem, "Rename" + userName + ellipsis(), currentUser.has_value());
    menu.addItem (deleteItem, "Delete" + userName + ellipsis(), currentUser.has_value());
    menu.addSeparator();
    menu.addItem (rescanItem, "Rescan User Presets");
    menu.addItem (showFolderItem, "Show Preset Folder");
    return menu;
}

void PresetBar::showMenu()
{
    juce::Component::SafePointer<PresetBar> safeThis (this);
    createMenu().showMenuAsync (juce::PopupMenu::Options()
                                    .withTargetComponent (&nameButton)
                                    .withMinimumWidth (nameButton.getWidth())
                                    .withStandardItemHeight (24),
                                [safeThis] (int result)
                                {
                                    if (safeThis != nullptr && result != 0)
                                        safeThis->handleMenuResult (result);
                                });
}

void PresetBar::handleMenuResult (int itemId)
{
    if (itemId >= presetItemBase && itemId < presetItemBase + menuPresets.size())
    {
        presets.load (menuPresets.getReference (itemId - presetItemBase));
        loaded();
        return;
    }

    juce::Component::SafePointer<PresetBar> safeThis (this);

    switch (itemId)
    {
        case saveAsItem:
            pressSave();
            break;

        case renameItem:
            if (const auto user = getCurrentUserPreset())
                askRename (user->name, [safeThis, info = *user] (const juce::String& newName)
                           {
                               if (safeThis == nullptr)
                                   return;

                               juce::String error;

                               if (! safeThis->presets.renameUserPreset (info, newName.trim(), error))
                                   safeThis->showMessage ("Rename failed", error);

                               safeThis->refresh();
                           });
            break;

        case deleteItem:
            if (const auto user = getCurrentUserPreset())
                askConfirm ("Delete preset?", "Delete the user preset \"" + user->name + "\"? The file is removed from "
                                                  + user->file.getParentDirectory().getFullPathName() + ".",
                            "Delete", [safeThis, info = *user] (bool yes)
                            {
                                if (safeThis == nullptr || ! yes)
                                    return;

                                juce::String error;

                                if (! safeThis->presets.deleteUserPreset (info, error))
                                    safeThis->showMessage ("Delete failed", error);

                                safeThis->refresh();
                            });
            break;

        case rescanItem:
            presets.refresh();
            break;

        case showFolderItem:
        {
            const auto folder = presets.getUserFolder();

            if (folder.isDirectory())
                folder.revealToUser();
            else
                showMessage ("No user presets yet",
                             "The folder " + folder.getFullPathName() + " is created when you save your first preset.");
            break;
        }

        default:
            break;
    }
}

//==============================================================================
void PresetBar::pressSave()
{
    const auto user = getCurrentUserPreset();
    auto category = PresetCategory::lead;
    auto name = presets.getCurrentName();

    if (user.has_value())
        category = user->category;
    else
        for (const auto& info : presets.getPresets())
            if (info.isFactory && info.name == name && info.category != PresetCategory::init)
                category = info.category;

    if (! user.has_value())
        name = (name == "Init" ? juce::String ("My Preset") : name + " (User)");

    juce::Component::SafePointer<PresetBar> safeThis (this);
    askSaveDetails (name, category, [safeThis] (const juce::String& chosenName, PresetCategory chosenCategory)
                    {
                        if (safeThis != nullptr)
                            safeThis->saveAs (chosenName, chosenCategory);
                    });
}

void PresetBar::saveAs (const juce::String& rawName, PresetCategory category)
{
    const auto name = rawName.trim();

    if (name.isEmpty())
    {
        showMessage ("Preset not saved", "Please enter a name for the preset.");
        return;
    }

    juce::Component::SafePointer<PresetBar> safeThis (this);
    auto save = [safeThis, name, category]
    {
        if (safeThis == nullptr)
            return;

        juce::String error;

        if (! safeThis->presets.saveUserPreset (name, category, error))
            safeThis->showMessage ("Preset not saved", error);

        safeThis->refresh();
    };

    if (presets.getUserPresetFile (name).existsAsFile())
        askConfirm ("Replace preset?", "A user preset named \"" + name + "\" already exists. Replace it?", "Replace",
                    [save] (bool yes)
                    {
                        if (yes)
                            save();
                    });
    else
        save();
}

//==============================================================================
void PresetBar::installDefaultDialogs()
{
    askConfirm = [this] (const juce::String& title, const juce::String& message, const juce::String& yesText,
                         std::function<void (bool)> onResult)
    {
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, title,
                                                                     message, yesText, "Cancel", this);
        juce::AlertWindow::showAsync (options, [done = std::move (onResult)] (int result) { done (result == 1); });
    };

    showMessage = [this] (const juce::String& title, const juce::String& message)
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOk (juce::MessageBoxIconType::WarningIcon,
                                                                              title, message, "OK", this),
                                      nullptr);
    };

    askSaveDetails = [this] (const juce::String& suggestedName, PresetCategory suggestedCategory,
                             std::function<void (const juce::String&, PresetCategory)> onOk)
    {
        if (dialog != nullptr)
            return;

        dialog = std::make_unique<juce::AlertWindow> ("Save preset", "Name and category of the user preset:",
                                                      juce::MessageBoxIconType::NoIcon, this);
        dialog->setLookAndFeel (&getLookAndFeel());
        dialog->addTextEditor ("name", suggestedName, "Name");
        juce::StringArray categories;

        for (int c = 1; c < numPresetCategories; ++c)
            categories.add (categoryName (static_cast<PresetCategory> (c)));

        dialog->addComboBox ("category", categories, "Category");
        dialog->getComboBoxComponent ("category")
            ->setSelectedItemIndex (juce::jmax (0, static_cast<int> (suggestedCategory) - 1), juce::dontSendNotification);
        dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<PresetBar> safeThis (this);
        dialog->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, done = std::move (onOk)] (int result)
                                 {
                                     if (safeThis == nullptr || safeThis->dialog == nullptr)
                                         return;

                                     const auto name = safeThis->dialog->getTextEditorContents ("name");
                                     const auto index = safeThis->dialog->getComboBoxComponent ("category")->getSelectedItemIndex();
                                     safeThis->dialog.reset();

                                     if (result == 1)
                                         done (name, static_cast<PresetCategory> (juce::jlimit (1, numPresetCategories - 1, index + 1)));
                                 }),
                                 false);
    };

    askRename = [this] (const juce::String& currentName, std::function<void (const juce::String&)> onOk)
    {
        if (dialog != nullptr)
            return;

        dialog = std::make_unique<juce::AlertWindow> ("Rename preset", "New name of \"" + currentName + "\":",
                                                      juce::MessageBoxIconType::NoIcon, this);
        dialog->setLookAndFeel (&getLookAndFeel());
        dialog->addTextEditor ("name", currentName, "Name");
        dialog->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<PresetBar> safeThis (this);
        dialog->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, done = std::move (onOk)] (int result)
                                 {
                                     if (safeThis == nullptr || safeThis->dialog == nullptr)
                                         return;

                                     const auto name = safeThis->dialog->getTextEditorContents ("name");
                                     safeThis->dialog.reset();

                                     if (result == 1)
                                         done (name);
                                 }),
                                 false);
    };
}

} // namespace vocalsynth::ui
