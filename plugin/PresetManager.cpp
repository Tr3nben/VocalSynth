#include "PresetManager.h"

#include "SynthParameters.h"
#include "SynthState.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace vocalsynth
{

namespace
{

const juce::Identifier versionAttribute ("version");
const juce::Identifier nameAttribute ("name");
const juce::Identifier categoryAttribute ("category");
const juce::Identifier authorAttribute ("author");
const juce::Identifier tagsAttribute ("tags");
const juce::Identifier paramTag ("PARAM");
const juce::Identifier tableTag ("TABLE");
const juce::Identifier idAttribute ("id");
const juce::Identifier valueAttribute ("value");

juce::String fromView (std::string_view v)
{
    return juce::String::fromUTF8 (v.data(), static_cast<int> (v.size()));
}

juce::String categoryName (PresetCategory c)
{
    const auto i = juce::jlimit (0, numPresetCategories - 1, static_cast<int> (c));
    return fromView (presetCategoryNames[static_cast<std::size_t> (i)]);
}

/** Stored by name (14.2); unknown -> FX for user presets. */
PresetCategory categoryFromName (const juce::String& name)
{
    for (int i = 0; i < numPresetCategories; ++i)
        if (name.equalsIgnoreCase (fromView (presetCategoryNames[static_cast<std::size_t> (i)])))
            return static_cast<PresetCategory> (i);

    return PresetCategory::fx;
}

juce::StringArray splitTags (const juce::String& tags)
{
    auto result = juce::StringArray::fromTokens (tags, ",", "");
    result.trim();
    result.removeEmptyStrings();
    return result;
}

/** A plain number ("35", "-1.5", "2e3"); anything else is not a value. */
bool parseNumber (const juce::String& text, float& value)
{
    const auto t = text.trim();

    if (t.isEmpty() || ! t.containsOnly ("0123456789+-.eE"))
        return false;

    const auto d = t.getDoubleValue();

    if (! std::isfinite (d))
        return false;

    value = static_cast<float> (d);
    return true;
}

/** Windows reserved device names (with or without an extension). */
bool isReservedWindowsName (const juce::String& base)
{
    const auto stem = base.upToFirstOccurrenceOf (".", false, false).trimEnd().toUpperCase();

    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
        return true;

    if (stem.length() == 4 && (stem.startsWith ("COM") || stem.startsWith ("LPT")))
        return juce::CharacterFunctions::isDigit (stem[3]) && stem[3] != '0';

    return false;
}

PresetInfo readPresetInfo (const juce::File& file)
{
    PresetInfo info;
    info.isFactory = false;
    info.file = file;
    info.name = file.getFileNameWithoutExtension();
    info.category = PresetCategory::fx;

    juce::XmlDocument doc (file);

    if (auto root = doc.getDocumentElement (true); root != nullptr && root->hasTagName (PresetManager::presetTreeType.toString()))
    {
        const auto name = root->getStringAttribute (nameAttribute).trim();

        if (name.isNotEmpty())
            info.name = name;

        info.category = categoryFromName (root->getStringAttribute (categoryAttribute));
        info.author = root->getStringAttribute (authorAttribute);
        info.tags = splitTags (root->getStringAttribute (tagsAttribute));
    }

    return info;
}

} // namespace

//==============================================================================
struct PresetManager::PresetData
{
    juce::String name;
    std::map<juce::String, float> values;   ///< plain values by parameter ID (unknown IDs are never looked up)
    std::array<SynthState::Table, WavetableStore::numSlots> tables;
    bool isFactory = true;
    int factoryIndex = 0;
    juce::File file;
};

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state, WavetableStore& s, WavetableWorker& w)
    : apvts (state),
      store (s),
      worker (w),
      userFolder (getDefaultUserFolder())
{
    for (const auto& id : params::getSynthParameterIds())
    {
        if (! params::isPresetParameter (id))
            continue;

        auto* param = apvts.getParameter (id);
        auto* raw = apvts.getRawParameterValue (id);
        jassert (param != nullptr && raw != nullptr);

        if (param != nullptr && raw != nullptr)
        {
            presetParameters.push_back (param);
            presetRaw.push_back (raw);
        }
    }

    const std::scoped_lock lock (mutex);
    takeSnapshotLocked();
}

PresetManager::~PresetManager() = default;

//==============================================================================
std::vector<float> PresetManager::readPresetValues() const
{
    std::vector<float> values (presetRaw.size());

    for (std::size_t i = 0; i < presetRaw.size(); ++i)
        values[i] = presetRaw[i]->load (std::memory_order_relaxed);

    return values;
}

void PresetManager::takeSnapshotLocked()
{
    snapshot = readPresetValues();
}

juce::String PresetManager::getCurrentName() const
{
    const std::scoped_lock lock (mutex);
    return currentName;
}

bool PresetManager::isModified() const
{
    const std::scoped_lock lock (mutex);

    if (modifiedFlag)
        return true;

    if (snapshot.size() != presetRaw.size())
        return false;

    for (std::size_t i = 0; i < presetRaw.size(); ++i)
    {
        // A host may hand a value back through its own normalised format: compare with a tolerance far below
        // any visible step (1e-6 of the range)
        const auto& range = presetParameters[i]->getNormalisableRange();
        const auto tolerance = 1.0e-6f * (range.end - range.start);

        if (std::abs (presetRaw[i]->load (std::memory_order_relaxed) - snapshot[i]) > tolerance)
            return true;
    }

    return false;
}

void PresetManager::markModified()
{
    const std::scoped_lock lock (mutex);
    modifiedFlag = true;
}

void PresetManager::setCurrent (const juce::String& name, bool isModifiedFlag)
{
    const std::scoped_lock lock (mutex);
    currentName = name;
    modifiedFlag = isModifiedFlag;

    // Identity unknown (a project): prev / next find the preset by its name
    currentIsFactory = false;
    currentFactoryIndex = -1;
    currentFile = juce::File();
    takeSnapshotLocked();
}

//==============================================================================
juce::Array<PresetInfo> PresetManager::getPresets() const
{
    scanIfNeeded();
    juce::Array<PresetInfo> result;

    for (int i = 0; i < FactoryPresets::getNumPresets(); ++i)
    {
        const auto& p = FactoryPresets::getPreset (i);
        PresetInfo info;
        info.name = fromView (p.name);
        info.category = p.category;
        info.author = "VocalSynth";
        info.tags = splitTags (fromView (p.tags));
        info.isFactory = true;
        info.factoryIndex = i;
        result.add (info);
    }

    result.addArray (userPresets);
    return result;
}

void PresetManager::refresh()
{
    scanned = false;
    scanIfNeeded();
}

void PresetManager::scanIfNeeded() const
{
    if (scanned)
        return;

    scanned = true;
    userPresets.clear();

    if (! userFolder.isDirectory())
        return;

    for (const auto& file : userFolder.findChildFiles (juce::File::findFiles, false, "*" + fileExtension))
        if (file.getFileExtension().equalsIgnoreCase (fileExtension))
            userPresets.add (readPresetInfo (file));

    std::sort (userPresets.begin(), userPresets.end(), [] (const PresetInfo& a, const PresetInfo& b)
    {
        const auto c = a.name.compareNatural (b.name);
        return c != 0 ? c < 0 : a.file.getFullPathName() < b.file.getFullPathName();
    });
}

juce::File PresetManager::getDefaultUserFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("VocalSynth")
        .getChildFile ("Presets");
}

juce::File PresetManager::getUserFolder() const
{
    return userFolder;
}

void PresetManager::setUserFolder (const juce::File& folder)
{
    userFolder = folder;
    scanned = false;
}

juce::File PresetManager::getUserPresetFile (const juce::String& name) const
{
    // createLegalFileName removes  " # @ , ; : < > * ^ | ? \ /  and limits the length; Windows also rejects
    // control characters, trailing dots / spaces and the device names
    juce::String base;

    for (const auto c : juce::File::createLegalFileName (name.trim()))
        if (c >= 32 && c != 127)
            base += juce::String::charToString (c);

    base = base.trim();

    while (base.endsWithChar ('.') || base.endsWithChar (' '))
        base = base.dropLastCharacters (1);

    if (base.isEmpty())
        base = "Preset";

    if (isReservedWindowsName (base))
        base = "_" + base;

    return userFolder.getChildFile (base + fileExtension);
}

//==============================================================================
bool PresetManager::apply (const PresetData& data)
{
    // 14.4: every preset parameter (missing -> default, values clamped) with a gesture each; never the mix
    // (synthOn, synthVolume, dryLevel), voice, test or capture parameters (presetParameters holds only
    // params::isPresetParameter IDs)
    for (auto* param : presetParameters)
    {
        const auto& range = param->getNormalisableRange();
        auto plain = param->convertFrom0to1 (param->getDefaultValue());

        if (const auto it = data.values.find (param->paramID); it != data.values.end() && std::isfinite (it->second))
            plain = juce::jlimit (range.start, range.end, it->second);

        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (plain));
        param->endChangeGesture();
    }

    // Tables synchronously: the preset is complete (and audible) when load() returns
    for (int s = 0; s < WavetableStore::numSlots; ++s)
    {
        const auto& table = data.tables[static_cast<std::size_t> (s)];
        juce::String error;

        if (table.reference.kind == WavetableReference::Kind::user)
            worker.loadEmbeddedNow (s, table.embedded, error);
        else
            worker.loadFactoryNow (s, table.reference.id, error);
    }

    const std::scoped_lock lock (mutex);
    currentName = data.name;
    modifiedFlag = false;
    currentIsFactory = data.isFactory;
    currentFactoryIndex = data.isFactory ? data.factoryIndex : -1;
    currentFile = data.isFactory ? juce::File() : data.file;
    takeSnapshotLocked();
    return true;
}

bool PresetManager::loadFactory (int factoryIndex)
{
    if (factoryIndex < 0 || factoryIndex >= FactoryPresets::getNumPresets())
        return false;

    const auto& preset = FactoryPresets::getPreset (factoryIndex);
    PresetData data;
    data.name = fromView (preset.name);
    data.isFactory = true;
    data.factoryIndex = factoryIndex;

    for (const auto& v : preset.values)
        data.values[fromView (v.id)] = v.value;

    const std::array<std::string_view, WavetableStore::numSlots> tableIds { preset.tableA, preset.tableB };

    for (std::size_t s = 0; s < tableIds.size(); ++s)
        if (! tableIds[s].empty())
            data.tables[s].reference = WavetableReference::fromRefString ("factory:" + fromView (tableIds[s]));

    return apply (data);
}

bool PresetManager::load (const PresetInfo& preset)
{
    if (preset.isFactory)
        return loadFactory (preset.factoryIndex);

    if (! preset.file.existsAsFile())
        return false;

    const auto xml = juce::XmlDocument::parse (preset.file);

    if (xml == nullptr || ! xml->hasTagName (presetTreeType.toString()))
        return false;

    PresetData data;
    data.isFactory = false;
    data.file = preset.file;
    data.name = xml->getStringAttribute (nameAttribute).trim();

    if (data.name.isEmpty())
        data.name = preset.file.getFileNameWithoutExtension();

    juce::ValueTree synthTree (SynthState::treeType);

    for (const auto* child : xml->getChildIterator())
    {
        if (child->hasTagName (paramTag.toString()))
        {
            float value = 0.0f;

            if (const auto id = child->getStringAttribute (idAttribute); id.isNotEmpty()
                && parseNumber (child->getStringAttribute (valueAttribute), value))
                data.values[id] = value;
        }
        else if (child->hasTagName (tableTag.toString()))
        {
            synthTree.appendChild (juce::ValueTree::fromXml (*child), nullptr);
        }
    }

    // The same TABLE rules as a project (13.1): data once per hash, missing / broken -> default table
    data.tables = SynthState::fromValueTree (synthTree).tables;
    return apply (data);
}

int PresetManager::findCurrentIndex (const juce::Array<PresetInfo>& presets) const
{
    bool isFactory = false;
    int factoryIndex = -1;
    juce::File file;
    juce::String name;

    {
        const std::scoped_lock lock (mutex);
        isFactory = currentIsFactory;
        factoryIndex = currentFactoryIndex;
        file = currentFile;
        name = currentName;
    }

    for (int i = 0; i < presets.size(); ++i)
    {
        const auto& p = presets.getReference (i);

        if (isFactory ? (p.isFactory && p.factoryIndex == factoryIndex)
                      : (! p.isFactory && file != juce::File() && p.file == file))
            return i;
    }

    for (int i = 0; i < presets.size(); ++i)
        if (presets.getReference (i).name == name)
            return i;

    return -1;
}

void PresetManager::loadNext()
{
    const auto presets = getPresets();

    if (presets.isEmpty())
        return;

    const auto current = findCurrentIndex (presets);
    load (presets.getReference ((current + 1) % presets.size()));
}

void PresetManager::loadPrevious()
{
    const auto presets = getPresets();

    if (presets.isEmpty())
        return;

    const auto current = findCurrentIndex (presets);
    load (presets.getReference (current <= 0 ? presets.size() - 1 : current - 1));
}

//==============================================================================
bool PresetManager::saveUserPreset (const juce::String& name, PresetCategory category, juce::String& errorMessage)
{
    const auto trimmed = name.trim();

    if (trimmed.isEmpty())
    {
        errorMessage = "Please enter a preset name";
        return false;
    }

    if (const auto result = userFolder.createDirectory(); result.failed())
    {
        errorMessage = "Could not create " + userFolder.getFullPathName() + ": " + result.getErrorMessage();
        return false;
    }

    const auto file = getUserPresetFile (trimmed);

    juce::XmlElement root (presetTreeType);
    root.setAttribute (versionAttribute, formatVersion);
    root.setAttribute (nameAttribute, trimmed);
    root.setAttribute (categoryAttribute, categoryName (category));
    root.setAttribute (authorAttribute, juce::String());
    root.setAttribute (tagsAttribute, juce::String());

    const auto values = readPresetValues();

    for (std::size_t i = 0; i < presetParameters.size(); ++i)
    {
        auto* child = root.createNewChildElement (paramTag);
        child->setAttribute (idAttribute, presetParameters[i]->paramID);
        child->setAttribute (valueAttribute, static_cast<double> (values[i]));
    }

    // Tables as the state saves them: the newest requested reference per oscillator, user data once per hash
    SynthState tables;

    for (int s = 0; s < WavetableStore::numSlots; ++s)
    {
        const auto info = worker.getRequestedReference (s);
        auto userData = info.userData;

        // A user reference always comes with its data from the worker; if not, take the published slot's
        if (info.reference.kind == WavetableReference::Kind::user && userData == nullptr)
            if (const auto published = store.getSlot (s); published.reference.id == info.reference.id)
                userData = published.userData;

        tables.tables[static_cast<std::size_t> (s)] = { info.reference, std::move (userData) };
    }

    for (const auto& child : tables.toValueTree())
        if (auto xml = child.createXml())
            root.addChildElement (xml.release());

    if (! root.writeTo (file))
    {
        errorMessage = "Could not write " + file.getFullPathName();
        return false;
    }

    refresh();

    const std::scoped_lock lock (mutex);
    currentName = trimmed;
    modifiedFlag = false;
    currentIsFactory = false;
    currentFactoryIndex = -1;
    currentFile = file;
    snapshot = values;
    return true;
}

bool PresetManager::renameUserPreset (const PresetInfo& preset, const juce::String& newName, juce::String& errorMessage)
{
    const auto trimmed = newName.trim();

    if (preset.isFactory)
    {
        errorMessage = "Factory presets cannot be renamed";
        return false;
    }

    if (trimmed.isEmpty())
    {
        errorMessage = "Please enter a preset name";
        return false;
    }

    auto xml = preset.file.existsAsFile() ? juce::XmlDocument::parse (preset.file) : nullptr;

    if (xml == nullptr || ! xml->hasTagName (presetTreeType.toString()))
    {
        errorMessage = "Preset file not found: " + preset.file.getFileName();
        return false;
    }

    const auto target = getUserPresetFile (trimmed);

    if (target != preset.file && target.exists())
    {
        errorMessage = "A preset named \"" + target.getFileNameWithoutExtension() + "\" already exists";
        return false;
    }

    xml->setAttribute (nameAttribute, trimmed);

    if (! xml->writeTo (target))
    {
        errorMessage = "Could not write " + target.getFullPathName();
        return false;
    }

    if (target != preset.file)
        preset.file.deleteFile();

    refresh();

    const std::scoped_lock lock (mutex);

    if (! currentIsFactory && currentFile == preset.file)
    {
        currentName = trimmed;
        currentFile = target;
    }

    return true;
}

bool PresetManager::deleteUserPreset (const PresetInfo& preset, juce::String& errorMessage)
{
    if (preset.isFactory)
    {
        errorMessage = "Factory presets cannot be deleted";
        return false;
    }

    if (! preset.file.existsAsFile() || ! preset.file.deleteFile())
    {
        errorMessage = "Could not delete " + preset.file.getFullPathName();
        return false;
    }

    refresh();

    const std::scoped_lock lock (mutex);

    if (! currentIsFactory && currentFile == preset.file)
        currentFile = juce::File();

    return true;
}

} // namespace vocalsynth
