#include "SynthState.h"

#include "vocalsynth/synth/FactoryWavetables.h"

#include <cmath>
#include <map>

namespace vocalsynth
{

namespace
{

const juce::Identifier tableType ("TABLE");
const juce::Identifier versionProperty ("version");
const juce::Identifier presetNameProperty ("presetName");
const juce::Identifier presetModifiedProperty ("presetModified");
const juce::Identifier editorScaleProperty ("editorScale");
const juce::Identifier oscProperty ("osc");
const juce::Identifier refProperty ("ref");
const juce::Identifier nameProperty ("name");
const juce::Identifier framesProperty ("frames");
const juce::Identifier dataProperty ("data");

constexpr int minEditorScale = 75;
constexpr int maxEditorScale = 200;

constexpr std::size_t valuesPerFrame = static_cast<std::size_t> (synth::Wavetable::frameSize);
constexpr std::size_t maxPcmValues = static_cast<std::size_t> (synth::Wavetable::maxFrames) * valuesPerFrame;

/** Base64 length of the largest table (256 frames of PCM16 = 1 MiB): anything longer is rejected before
    it is decoded (a corrupt or hostile state must not make us allocate without bound). */
constexpr int maxBase64Length = static_cast<int> ((maxPcmValues * 2 + 2) / 3 * 4);

/** 16 lower-case hex digits (WavetableReference::id of a user table). */
bool isHash (const juce::String& s)
{
    return s.length() == 16 && s.containsOnly ("0123456789abcdef");
}

/** int property, def if missing or not a number. */
int intProperty (const juce::ValueTree& tree, const juce::Identifier& id, int def)
{
    const auto& v = tree.getProperty (id);

    if (v.isVoid())
        return def;

    if (v.isString())
    {
        const auto text = v.toString().trim();

        if (text.isEmpty() || ! text.containsOnly ("+-0123456789"))
            return def;

        return text.getIntValue();
    }

    const auto d = static_cast<double> (v);
    return std::isfinite (d) && std::abs (d) < 1.0e9 ? static_cast<int> (std::lround (d)) : def;
}

/** Reference of a TABLE, sanitised: bad syntax / an unknown factory id / a user id that is no hash -> the
    default factory table. */
WavetableReference parseReference (const juce::ValueTree& table)
{
    const auto name = table.getProperty (nameProperty).toString();
    auto ref = WavetableReference::fromRefString (table.getProperty (refProperty).toString(), name);

    if (ref.kind == WavetableReference::Kind::factory)
    {
        const auto id = ref.id.toStdString();
        const auto factory = synth::FactoryWavetables::fromId (id);

        if (! factory.has_value())
            return WavetableReference::defaultFactory();

        if (ref.name.isEmpty())
        {
            const auto display = synth::FactoryWavetables::getInfo (*factory).displayName;
            ref.name = juce::String (display.data(), display.size());
        }

        return ref;
    }

    ref.id = ref.id.toLowerCase();

    if (! isHash (ref.id))
        return WavetableReference::defaultFactory();

    return ref;
}

/** Embedded data of a user TABLE, null if absent or broken (frames out of range, wrong value count, bad
    base64, too long). The hash is NOT checked here: the worker recomputes it. */
std::shared_ptr<const UserTableData> parseData (const juce::ValueTree& table, const WavetableReference& ref)
{
    const auto& dataValue = table.getProperty (dataProperty);

    if (dataValue.isVoid())
        return nullptr;

    const auto frames = intProperty (table, framesProperty, 0);

    if (frames < 1 || frames > synth::Wavetable::maxFrames)
        return nullptr;

    const auto text = dataValue.toString();

    if (text.length() > maxBase64Length)
        return nullptr;

    auto pcm = SynthState::decodePcm16 (text);

    if (pcm.size() != static_cast<std::size_t> (frames) * valuesPerFrame)
        return nullptr;

    auto data = std::make_shared<UserTableData>();
    data->name = ref.name;
    data->hash = ref.id;
    data->numFrames = frames;
    data->pcm = std::move (pcm);
    return data;
}

} // namespace

juce::ValueTree SynthState::toValueTree() const
{
    juce::ValueTree tree (treeType);
    tree.setProperty (versionProperty, currentVersion, nullptr);
    tree.setProperty (presetNameProperty, presetName, nullptr);
    tree.setProperty (presetModifiedProperty, presetModified, nullptr);
    tree.setProperty (editorScaleProperty, juce::jlimit (minEditorScale, maxEditorScale, editorScalePercent), nullptr);

    juce::StringArray embeddedRefs; // user table data once per hash (13.1)

    for (std::size_t s = 0; s < tables.size(); ++s)
    {
        const auto& t = tables[s];
        juce::ValueTree child (tableType);
        child.setProperty (oscProperty, static_cast<int> (s), nullptr);
        child.setProperty (refProperty, t.reference.toRefString(), nullptr);
        child.setProperty (nameProperty, t.reference.name, nullptr);

        const auto ref = t.reference.toRefString();
        const auto& data = t.embedded;
        const bool hasData = t.reference.kind == WavetableReference::Kind::user && data != nullptr
                             && data->numFrames >= 1 && data->numFrames <= synth::Wavetable::maxFrames
                             && data->pcm.size() == static_cast<std::size_t> (data->numFrames) * valuesPerFrame;

        if (hasData && ! embeddedRefs.contains (ref))
        {
            child.setProperty (framesProperty, data->numFrames, nullptr);
            child.setProperty (dataProperty, encodePcm16 (data->pcm), nullptr);
            embeddedRefs.add (ref);
        }

        tree.appendChild (child, nullptr);
    }

    return tree;
}

SynthState SynthState::fromValueTree (const juce::ValueTree& tree)
{
    SynthState state;

    if (! tree.isValid() || ! tree.hasType (treeType))
        return state;

    // Any version: later versions only add properties / children, which are ignored here
    if (tree.hasProperty (presetNameProperty))
        state.presetName = tree.getProperty (presetNameProperty).toString().substring (0, 256);

    state.presetModified = intProperty (tree, presetModifiedProperty, 0) != 0
                           || tree.getProperty (presetModifiedProperty).toString().equalsIgnoreCase ("true");

    const auto scale = intProperty (tree, editorScaleProperty, 100);
    state.editorScalePercent = scale >= minEditorScale && scale <= maxEditorScale ? scale : 100;

    std::array<bool, WavetableStore::numSlots> seen {};
    std::map<juce::String, std::shared_ptr<const UserTableData>> dataByRef;

    // First pass: every valid embedded data block by reference (a TABLE without data may come first)
    for (const auto& child : tree)
    {
        if (! child.hasType (tableType))
            continue;

        const auto ref = parseReference (child);

        if (ref.kind != WavetableReference::Kind::user || dataByRef.count (ref.toRefString()) > 0)
            continue;

        if (auto data = parseData (child, ref))
            dataByRef[ref.toRefString()] = std::move (data);
    }

    for (const auto& child : tree)
    {
        if (! child.hasType (tableType))
            continue;

        const auto osc = intProperty (child, oscProperty, -1);

        if (osc < 0 || osc >= WavetableStore::numSlots || seen[static_cast<std::size_t> (osc)])
            continue; // invalid or duplicate oscillator: the first TABLE wins

        seen[static_cast<std::size_t> (osc)] = true;
        auto& table = state.tables[static_cast<std::size_t> (osc)];
        table.reference = parseReference (child);

        if (table.reference.kind != WavetableReference::Kind::user)
            continue;

        if (const auto it = dataByRef.find (table.reference.toRefString()); it != dataByRef.end())
        {
            if (it->second->name == table.reference.name)
            {
                table.embedded = it->second;
            }
            else
            {
                auto renamed = std::make_shared<UserTableData> (*it->second);
                renamed->name = table.reference.name;
                table.embedded = std::move (renamed);
            }
        }
        else
        {
            // No (valid) data in this state: reference only; the worker looks the hash up among the tables
            // of this process, else falls back to the default table (13.1)
            auto refOnly = std::make_shared<UserTableData>();
            refOnly->name = table.reference.name;
            refOnly->hash = table.reference.id;
            table.embedded = std::move (refOnly);
        }
    }

    return state;
}

void SynthState::removeFromState (juce::ValueTree& root)
{
    for (auto child = root.getChildWithName (treeType); child.isValid(); child = root.getChildWithName (treeType))
        root.removeChild (child, nullptr);
}

int SynthState::getStateVersion (const juce::ValueTree& root)
{
    return juce::jmax (0, intProperty (root, stateVersionProperty, 0));
}

juce::String SynthState::encodePcm16 (std::span<const int16_t> pcm)
{
    juce::MemoryBlock bytes (pcm.size() * 2);
    auto* out = static_cast<uint8_t*> (bytes.getData());

    for (std::size_t i = 0; i < pcm.size(); ++i)
    {
        const auto u = static_cast<uint16_t> (pcm[i]);
        out[2 * i] = static_cast<uint8_t> (u & 0xffu);
        out[2 * i + 1] = static_cast<uint8_t> (u >> 8);
    }

    return juce::Base64::toBase64 (bytes.getData(), bytes.getSize());
}

std::vector<int16_t> SynthState::decodePcm16 (const juce::String& base64)
{
    if (base64.isEmpty() || base64.length() % 4 != 0 || base64.length() > maxBase64Length)
        return {};

    juce::MemoryOutputStream bytes;

    if (! juce::Base64::convertFromBase64 (bytes, base64) || bytes.getDataSize() % 2 != 0)
        return {};

    const auto* in = static_cast<const uint8_t*> (bytes.getData());
    std::vector<int16_t> pcm (bytes.getDataSize() / 2);

    for (std::size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t> (static_cast<uint16_t> (in[2 * i] | (in[2 * i + 1] << 8)));

    return pcm;
}

} // namespace vocalsynth
