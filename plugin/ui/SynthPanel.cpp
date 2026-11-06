#include "ui/SynthPanel.h"

#include "Parameters.h"
#include "SynthParameters.h"
#include "ui/Theme.h"
#include "vocalsynth/synth/FactoryWavetables.h"
#include "vocalsynth/synth/WavReader.h"
#include "vocalsynth/synth/WavetableBuilder.h"

#include <array>
#include <cmath>
#include <cstring>

namespace vocalsynth::ui
{

namespace
{
using Style = Knob::Style;
using ToggleStyle = ChoiceChip::ToggleStyle;

constexpr int outer = 8;
constexpr int gap = theme::gap;
constexpr int topBarHeight = theme::presetBarHeight;
constexpr int oscRowHeight = 238;
constexpr int filterRowHeight = 112;
constexpr int lfoRowHeight = 136;
constexpr int fxRowHeight = 114;
static_assert (outer + topBarHeight + gap + oscRowHeight + gap + filterRowHeight + gap + lfoRowHeight + gap
                   + fxRowHeight + outer
               == SynthPanel::designHeight);

constexpr int dspTextEveryTicks = 15; // 2 Hz
constexpr float dimmedAlpha = 0.45f;

juce::String factoryDisplayName (const juce::String& id)
{
    if (const auto table = synth::FactoryWavetables::fromId (id.toStdString()))
    {
        const auto name = synth::FactoryWavetables::getInfo (*table).displayName;
        return juce::String (name.data(), name.size());
    }

    return id;
}

juce::String displayNameOf (const WavetableReference& ref)
{
    if (ref.name.isNotEmpty())
        return ref.name;

    return ref.kind == WavetableReference::Kind::factory ? factoryDisplayName (ref.id) : juce::String ("User table");
}

/** Lays the components out side by side in equal cells (nullptr = an empty cell). */
void placeRow (juce::Rectangle<int> area, std::initializer_list<juce::Component*> components, int spacing = 0)
{
    const auto n = static_cast<int> (components.size());

    if (n == 0)
        return;

    const auto cell = static_cast<float> (area.getWidth() - spacing * (n - 1)) / static_cast<float> (n);
    int i = 0;

    for (auto* c : components)
    {
        const auto x0 = area.getX() + juce::roundToInt (static_cast<float> (i) * (cell + static_cast<float> (spacing)));
        const auto x1 = area.getX() + juce::roundToInt (static_cast<float> (i) * (cell + static_cast<float> (spacing)) + cell);

        if (c != nullptr)
            c->setBounds (x0, area.getY(), x1 - x0, area.getHeight());

        ++i;
    }
}

/** Splits a width into parts proportional to weights (rounded, the sum stays exact). */
std::vector<int> splitWidths (int total, std::initializer_list<float> weights)
{
    float sum = 0.0f;

    for (auto w : weights)
        sum += w;

    std::vector<int> result;
    float acc = 0.0f;
    int done = 0;

    for (auto w : weights)
    {
        acc += w;
        const auto edge = juce::roundToInt (static_cast<float> (total) * acc / sum);
        result.push_back (edge - done);
        done = edge;
    }

    return result;
}

//==============================================================================
/** The four voice-expression meters of the VOICE section (16.5): level, brightness, noisiness (0..1) and
    pitch (-100..+100 cents, from the centre). Repaints only when a value moved visibly. */
class ExpressionMeters final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ExpressionMeters()
    {
        setComponentID ("expressionMeters");
        setTooltip ("Voice expression (Voice mode): level, brightness, noisiness and pitch of your voice - the mod "
                    "sources Voice Level / Brightness / Noisiness / Pitch");
        setInterceptsMouseClicks (false, false);
    }

    void setFrame (const ExpressionFrame& f)
    {
        const std::array<float, 4> next { juce::jlimit (0.0f, 1.0f, f.level), juce::jlimit (0.0f, 1.0f, f.brightness),
                                          juce::jlimit (0.0f, 1.0f, f.noisiness),
                                          juce::jlimit (-1.0f, 1.0f, f.pitchCents / 100.0f) };
        bool changed = false;

        for (std::size_t i = 0; i < next.size(); ++i)
            changed = changed || std::abs (next[i] - values[i]) > 0.004f;

        if (changed)
        {
            values = next;
            repaint();
        }
    }

    const std::array<float, 4>& getValues() const noexcept { return values; }

    void paint (juce::Graphics& g) override
    {
        static const char* const names[] = { "Level", "Bright", "Noise", "Pitch" };
        const auto rowHeight = static_cast<float> (getHeight()) / 4.0f;
        g.setFont (theme::font (theme::fontSmall));

        for (int i = 0; i < 4; ++i)
        {
            auto row = juce::Rectangle<float> (0.0f, rowHeight * static_cast<float> (i), static_cast<float> (getWidth()), rowHeight);
            g.setColour (theme::textDim);
            g.drawText (names[i], row.removeFromLeft (46.0f), juce::Justification::centredLeft, false);

            const auto bar = row.withSizeKeepingCentre (row.getWidth(), juce::jmin (6.0f, row.getHeight() - 4.0f));
            g.setColour (theme::meterBackground);
            g.fillRoundedRectangle (bar, 2.0f);
            const auto v = values[static_cast<std::size_t> (i)];

            if (i < 3)
            {
                g.setColour (theme::accent);
                g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * v), 2.0f);
            }
            else
            {
                const auto cx = bar.getCentreX();
                const auto x = cx + bar.getWidth() * 0.5f * v;
                g.setColour (theme::accent);
                g.fillRoundedRectangle (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (cx, x), bar.getY(),
                                                                                     juce::jmax (cx, x), bar.getBottom()),
                                        2.0f);
                g.setColour (theme::textDim);
                g.fillRect (juce::Rectangle<float> (cx - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f));
            }
        }
    }

private:
    std::array<float, 4> values {};
};

/** MOD MATRIX section: draws the slot numbers' arrows between source and destination. */
class ModMatrixPanel final : public SectionPanel
{
public:
    ModMatrixPanel() : SectionPanel ("MOD MATRIX") {}

    std::vector<juce::Rectangle<float>> arrows;

    void paint (juce::Graphics& g) override
    {
        SectionPanel::paint (g);
        g.setColour (theme::textDim);

        for (const auto& a : arrows)
        {
            juce::Path p;
            const auto y = a.getCentreY();
            p.startNewSubPath (a.getX() + 1.0f, y);
            p.lineTo (a.getRight() - 1.5f, y);
            p.startNewSubPath (a.getRight() - 4.5f, y - 3.0f);
            p.lineTo (a.getRight() - 1.5f, y);
            p.lineTo (a.getRight() - 4.5f, y + 3.0f);
            g.strokePath (p, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
};

} // namespace

//==============================================================================
struct SynthPanel::Parts
{
    juce::Component content;
    std::vector<std::unique_ptr<SectionPanel>> sections;

    std::array<WavetableView, 2> views;
    std::array<juce::TextButton, 2> tableButtons;
    ExpressionMeters meters;
    juce::Label dspLabel;
    std::array<juce::Label, synth::numModSlots> modNumbers;
    juce::Label voiceMetersCaption;

    // Sections
    std::array<SectionPanel*, 2> oscSection {};
    SectionPanel* subSection = nullptr;
    SectionPanel* noiseSection = nullptr;
    SectionPanel* voiceSection = nullptr;
    SectionPanel* filterSection = nullptr;
    std::array<SectionPanel*, 3> envSection {};
    std::array<SectionPanel*, 2> lfoSection {};
    ModMatrixPanel* modSection = nullptr;
    SectionPanel* chorusSection = nullptr;
    SectionPanel* delaySection = nullptr;
    SectionPanel* reverbSection = nullptr;
    SectionPanel* masterSection = nullptr;

    // Controls (owned by SynthPanel::controls)
    struct Osc
    {
        ChoiceChip* on = nullptr;
        Knob *position = nullptr, *octave = nullptr, *semi = nullptr, *fine = nullptr, *level = nullptr, *pan = nullptr,
             *unison = nullptr, *detune = nullptr, *spread = nullptr, *phase = nullptr;
    };
    std::array<Osc, 2> osc;

    Knob *subLevel = nullptr, *noiseLevel = nullptr, *noiseColor = nullptr;
    ChoiceChip *subOctave = nullptr, *subShape = nullptr;
    ChoiceChip *playMode = nullptr, *glideMode = nullptr, *bendRange = nullptr;
    Knob *glideTime = nullptr, *ampVelocity = nullptr;
    ChoiceChip *filterOn = nullptr, *filterMode = nullptr;
    std::array<Knob*, 5> filterKnobs {};
    std::array<std::array<Knob*, 4>, 3> envKnobs {};
    Knob* ampGain = nullptr;

    struct Lfo
    {
        ChoiceChip *shape = nullptr, *sync = nullptr, *retrig = nullptr;
        Knob *rate = nullptr, *fade = nullptr;
    };
    std::array<Lfo, 2> lfo;

    struct Mod
    {
        ChoiceChip *source = nullptr, *dest = nullptr;
        Knob* amount = nullptr;
    };
    std::array<Mod, synth::numModSlots> mod;

    ChoiceChip *chorusOn = nullptr, *delayOn = nullptr, *delaySync = nullptr, *delayPingPong = nullptr,
               *reverbOn = nullptr, *synthOn = nullptr;
    std::array<Knob*, 3> chorusKnobs {}, reverbKnobs {};
    Knob *delayTime = nullptr, *delayFeedback = nullptr, *delayMix = nullptr, *synthVolume = nullptr, *dryLevel = nullptr;

    // Raw parameter values (message-thread reads of the APVTS atomics)
    std::array<std::atomic<float>*, 2> rawPosition {}, rawOscOn {};
    std::array<std::atomic<float>*, 2> rawLfoSync {};
    std::atomic<float>* rawDelaySync = nullptr;
    std::atomic<float>* rawFilterOn = nullptr;
    std::atomic<float>*rawChorusOn = nullptr, *rawDelayOn = nullptr, *rawReverbOn = nullptr, *rawSynthOn = nullptr;
    std::array<std::atomic<float>*, synth::numModSlots> rawModSource {}, rawModDest {}, rawModAmount {};

    // Cached state
    uint32_t lastGeneration = 0;
    bool haveGeneration = false;
    std::array<WavetableSlotInfo, 2> published;
    int tick = 0;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastImportFolder;

    SectionPanel& addSection (const juce::String& title, const juce::String& subtitle = {})
    {
        sections.push_back (std::make_unique<SectionPanel> (title));
        auto& s = *sections.back();
        s.setSubtitle (subtitle);
        content.addAndMakeVisible (s);
        return s;
    }

    ModMatrixPanel& addModSection()
    {
        auto panel = std::make_unique<ModMatrixPanel>();
        auto* raw = panel.get();
        sections.push_back (std::move (panel));
        content.addAndMakeVisible (*raw);
        return *raw;
    }
};

//==============================================================================
SynthPanel::SynthPanel (const SynthPanelContext& c)
    : context (c),
      presetBar (c.presets),
      parts (std::make_unique<Parts>())
{
    setOpaque (true);
    parts->content.setComponentID ("synthContent");
    addAndMakeVisible (parts->content);
    parts->content.addAndMakeVisible (presetBar);
    presetBar.onPresetLoaded = [this]
    {
        if (context.onPresetLoaded != nullptr)
            context.onPresetLoaded();
    };

    buildControls();
    layoutContent();
    timerCallback();
    startTimerHz (timerHz);
}

SynthPanel::~SynthPanel()
{
    stopTimer();
}

juce::Component& SynthPanel::getContent() noexcept
{
    return parts->content;
}

WavetableView& SynthPanel::getWavetableView (int slot)
{
    return parts->views[static_cast<std::size_t> (juce::jlimit (0, 1, slot))];
}

juce::String SynthPanel::getTableButtonText (int slot)
{
    return parts->tableButtons[static_cast<std::size_t> (juce::jlimit (0, 1, slot))].getButtonText();
}

juce::String SynthPanel::getDspText() const
{
    return parts->dspLabel.isVisible() ? parts->dspLabel.getText() : juce::String();
}

juce::Component* SynthPanel::findControlFor (const juce::String& parameterId)
{
    for (auto& control : controls)
        if (control->getComponentID() == parameterId)
            return control.get();

    return nullptr;
}

//==============================================================================
void SynthPanel::buildControls()
{
    auto& p = *parts;
    auto& apvts = context.apvts;

    const auto knob = [&] (SectionPanel& section, const juce::ParameterID& id, const juce::String& label,
                           const juce::String& tip, Style style = Style::rotary) -> Knob*
    {
        auto k = std::make_unique<Knob> (apvts, id.getParamID(), label, style);
        auto* raw = k.get();

        if (tip.isNotEmpty())
            raw->setTooltip (tip);

        section.addAndMakeVisible (*raw);
        controls.push_back (std::move (k));
        return raw;
    };

    const auto chip = [&] (SectionPanel& section, const juce::ParameterID& id, const juce::String& text,
                           const juce::String& tip, ToggleStyle style = ToggleStyle::chip) -> ChoiceChip*
    {
        auto ch = std::make_unique<ChoiceChip> (apvts, id.getParamID(), text, style);
        auto* raw = ch.get();

        if (tip.isNotEmpty())
            raw->setTooltip (tip);

        section.addAndMakeVisible (*raw);
        controls.push_back (std::move (ch));
        return raw;
    };

    // --- Oscillators
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto& ids = params::oscParameters[i];
        const juce::String name = i == 0 ? "A" : "B";
        auto& s = p.addSection ("OSC " + name);
        p.oscSection[i] = &s;
        auto& o = p.osc[i];

        o.on = chip (s, ids.on, {}, "Oscillator " + name + " on / off", ToggleStyle::power);
        s.setPowerComponent (o.on);

        auto& button = p.tableButtons[i];
        button.setComponentID (i == 0 ? "oscATable" : "oscBTable");
        button.getProperties().set ("vsDropdown", true);
        button.setTooltip ("Wavetable of oscillator " + name + ": 14 factory tables or Import WAV... (the table is saved "
                           "with the project)");
        button.onClick = [this, slot = static_cast<int> (i)] { showTableMenu (slot); };
        s.setHeaderComponent (&button);
        s.setHeaderWidth (156);

        p.views[i].setComponentID (i == 0 ? "oscAView" : "oscBView");
        p.views[i].setShowsName (false);
        s.addAndMakeVisible (p.views[i]);

        o.position = knob (s, ids.position, "Position", "Position in the wavetable (morphs through its frames)");
        o.octave = knob (s, ids.octave, "Octave", "Octave offset");
        o.semi = knob (s, ids.semi, "Semi", "Semitone offset");
        o.fine = knob (s, ids.fine, "Fine", "Fine tuning in cents");
        o.level = knob (s, ids.level, "Level", "Oscillator level");
        o.pan = knob (s, ids.pan, "Pan", "Stereo position");
        o.unison = knob (s, ids.unison, "Unison", "Number of unison voices (1-8)");
        o.detune = knob (s, ids.detune, "Detune", "Total detune spread of the unison voices in cents");
        o.spread = knob (s, ids.spread, "Spread", "Stereo spread of the unison voices");
        o.phase = knob (s, ids.phaseRand, "Phase", "Random start phase per note (0 = all voices start in phase)");

        p.rawPosition[i] = apvts.getRawParameterValue (ids.position.getParamID());
        p.rawOscOn[i] = apvts.getRawParameterValue (ids.on.getParamID());
    }

    // --- Sub / noise
    {
        auto& s = p.addSection ("SUB");
        p.subSection = &s;
        p.subLevel = knob (s, params::subLevel, "Level", "Sub oscillator level (follows the note, 1 or 2 octaves down)");
        p.subOctave = chip (s, params::subOctave, {}, "Sub octave");
        p.subShape = chip (s, params::subShape, {}, "Sub waveform");

        auto& n = p.addSection ("NOISE");
        p.noiseSection = &n;
        p.noiseLevel = knob (n, params::noiseLevel, "Level", "Noise level");
        p.noiseColor = knob (n, params::noiseColor, "Color", "Noise colour: white (0 %) to pink (100 %)");
    }

    // --- Voice
    {
        auto& s = p.addSection ("VOICE");
        p.voiceSection = &s;
        p.playMode = chip (s, params::playMode, "Mode",
                           "Mono: retrigger every note. Legato: notes glide / continue without retrigger. Poly: 16 voices");
        p.glideMode = chip (s, params::glideMode, "Glide Mode", "Glide on every note or only between connected notes");
        p.bendRange = chip (s, params::bendRange, "Bend",
                            "Pitch bend range of the voice tracking (same setting as on the Voice page)");
        p.glideTime = knob (s, params::glideTime, "Glide", "Glide (portamento) time");
        p.ampVelocity = knob (s, params::ampVelocity, "Velocity", "How much the note velocity changes the level");
        p.voiceMetersCaption.setText ("EXPRESSION", juce::dontSendNotification);
        p.voiceMetersCaption.setFont (theme::font (theme::fontSmall - 1.0f, true).withExtraKerningFactor (0.08f));
        p.voiceMetersCaption.setColour (juce::Label::textColourId, theme::textDim);
        p.voiceMetersCaption.setBorderSize ({});
        s.addAndMakeVisible (p.voiceMetersCaption);
        s.addAndMakeVisible (p.meters);
    }

    // --- Filter
    {
        auto& s = p.addSection ("FILTER");
        p.filterSection = &s;
        p.filterOn = chip (s, params::filterOn, {}, "Filter on / off", ToggleStyle::power);
        s.setPowerComponent (p.filterOn);
        p.filterMode = chip (s, params::filterMode, "Mode", "Filter type: low pass 12 / 24 dB, band pass, high pass, notch");
        p.filterKnobs = { knob (s, params::filterCutoff, "Cutoff", "Cutoff frequency"),
                          knob (s, params::filterResonance, "Reso", "Resonance"),
                          knob (s, params::filterDrive, "Drive", "Drive (saturation) before the filter"),
                          knob (s, params::filterKeytrack, "Key", "Keyboard tracking: the cutoff follows the note"),
                          knob (s, params::filterEnvAmount, "Env", "Env 2 amount on the cutoff (+-5 octaves)") };
        p.rawFilterOn = apvts.getRawParameterValue (params::filterOn.getParamID());
    }

    // --- Envelopes
    {
        static const char* const subtitles[] = { "AMP", "FILTER", "MOD" };
        static const char* const tips[] = { "Amplitude envelope", "Filter envelope (Filter Env amount) and mod source Env 2",
                                            "Mod source Env 3" };

        for (std::size_t e = 0; e < 3; ++e)
        {
            const auto& ids = params::envelopeParameters[e];
            auto& s = p.addSection ("ENV " + juce::String (static_cast<int> (e) + 1), subtitles[e]);
            p.envSection[e] = &s;
            const juce::String tip = tips[e];
            p.envKnobs[e] = { knob (s, ids.attack, "Attack", tip + ": attack time"),
                              knob (s, ids.decay, "Decay", tip + ": decay time"),
                              knob (s, ids.sustain, "Sustain", tip + ": sustain level"),
                              knob (s, ids.release, "Release", tip + ": release time") };

            if (e == 0)
                p.ampGain = knob (s, params::ampGain, "Gain", "Patch level (part of the preset; Volume in MASTER is yours)");
        }
    }

    // --- LFOs
    for (std::size_t l = 0; l < 2; ++l)
    {
        const auto& ids = params::lfoParameters[l];
        auto& s = p.addSection ("LFO " + juce::String (static_cast<int> (l) + 1));
        p.lfoSection[l] = &s;
        auto& lfo = p.lfo[l];
        lfo.shape = chip (s, ids.shape, "Shape", "LFO waveform");
        lfo.sync = chip (s, ids.sync, "Sync", "Tempo sync to the host (Off = Rate in Hz)");
        lfo.retrig = chip (s, ids.retrig, "Retrig", "Restart the LFO at every new note");
        lfo.rate = knob (s, ids.rate, "Rate", "LFO rate (when Sync is Off)");
        lfo.fade = knob (s, ids.fade, "Fade", "Fade-in time after a note start");
        p.rawLfoSync[l] = apvts.getRawParameterValue (ids.sync.getParamID());
    }

    // --- Mod matrix
    {
        auto& s = p.addModSection();
        p.modSection = &s;

        for (std::size_t m = 0; m < synth::numModSlots; ++m)
        {
            const auto& ids = params::modParameters[m];
            auto& slot = p.mod[m];
            const auto n = juce::String (static_cast<int> (m) + 1);
            slot.source = chip (s, ids.source, {}, "Mod " + n + " source");
            slot.dest = chip (s, ids.dest, {}, "Mod " + n + " destination");
            slot.amount = knob (s, ids.amount, {}, "Mod " + n + " amount (+-100 %)", Style::bar);
            slot.amount->getSlider().setColour (juce::Slider::trackColourId, theme::modulation);
            slot.source->getComboBox()->getProperties().set ("vsCompact", true);
            slot.dest->getComboBox()->getProperties().set ("vsCompact", true);

            auto& label = p.modNumbers[m];
            label.setText (n, juce::dontSendNotification);
            label.setFont (theme::font (theme::fontSmall, true));
            label.setColour (juce::Label::textColourId, theme::textDim);
            label.setJustificationType (juce::Justification::centred);
            label.setBorderSize ({});
            s.addAndMakeVisible (label);

            p.rawModSource[m] = apvts.getRawParameterValue (ids.source.getParamID());
            p.rawModDest[m] = apvts.getRawParameterValue (ids.dest.getParamID());
            p.rawModAmount[m] = apvts.getRawParameterValue (ids.amount.getParamID());
        }
    }

    // --- FX
    {
        auto& c = p.addSection ("CHORUS");
        p.chorusSection = &c;
        p.chorusOn = chip (c, params::chorusOn, {}, "Chorus on / off", ToggleStyle::power);
        c.setPowerComponent (p.chorusOn);
        p.chorusKnobs = { knob (c, params::chorusRate, "Rate", "Chorus rate"),
                          knob (c, params::chorusDepth, "Depth", "Chorus depth"),
                          knob (c, params::chorusMix, "Mix", "Chorus mix") };

        auto& d = p.addSection ("DELAY");
        p.delaySection = &d;
        p.delayOn = chip (d, params::delayOn, {}, "Delay on / off", ToggleStyle::power);
        d.setPowerComponent (p.delayOn);
        p.delayTime = knob (d, params::delayTime, "Time", "Delay time (when Sync is Off)");
        p.delaySync = chip (d, params::delaySync, "Sync", "Tempo sync to the host (Off = Time in ms)");
        p.delayPingPong = chip (d, params::delayPingPong, "Ping-Pong", "Echoes alternate left / right");
        p.delayFeedback = knob (d, params::delayFeedback, "Feedback", "Delay feedback");
        p.delayMix = knob (d, params::delayMix, "Mix", "Delay mix");

        auto& r = p.addSection ("REVERB");
        p.reverbSection = &r;
        p.reverbOn = chip (r, params::reverbOn, {}, "Reverb on / off", ToggleStyle::power);
        r.setPowerComponent (p.reverbOn);
        p.reverbKnobs = { knob (r, params::reverbSize, "Size", "Room size / decay time"),
                          knob (r, params::reverbDamping, "Damping", "High-frequency damping"),
                          knob (r, params::reverbMix, "Mix", "Reverb mix") };

        auto& m = p.addSection ("MASTER");
        p.masterSection = &m;
        p.synthOn = chip (m, params::synthOn, "Synth On",
                          "VocalSynth's own synth plays the notes of your voice (the MIDI output works either way)");
        p.synthVolume = knob (m, params::synthVolume, "Volume", "Synth output level (not part of presets)");
        p.dryLevel = knob (m, params::dryLevel, "Dry",
                           "Level of your dry voice in the output (FX variant only; Mute Dry still mutes it)");

        p.rawDelaySync = apvts.getRawParameterValue (params::delaySync.getParamID());
        p.rawChorusOn = apvts.getRawParameterValue (params::chorusOn.getParamID());
        p.rawDelayOn = apvts.getRawParameterValue (params::delayOn.getParamID());
        p.rawReverbOn = apvts.getRawParameterValue (params::reverbOn.getParamID());
        p.rawSynthOn = apvts.getRawParameterValue (params::synthOn.getParamID());
    }

    // --- DSP display (top bar)
    p.dspLabel.setComponentID ("synthDsp");
    p.dspLabel.setFont (theme::font (theme::fontSmall));
    p.dspLabel.setColour (juce::Label::textColourId, theme::textDim);
    p.dspLabel.setJustificationType (juce::Justification::centredRight);
    p.dspLabel.setMinimumHorizontalScale (0.8f);
    p.dspLabel.setTooltip ("CPU load of VocalSynth's synth (share of the audio block time)");
    p.content.addChildComponent (p.dspLabel);
}

//==============================================================================
void SynthPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void SynthPanel::resized()
{
    auto& content = parts->content;
    content.setBounds (0, 0, designWidth, designHeight);

    const auto scale = juce::jmin (static_cast<float> (getWidth()) / static_cast<float> (designWidth),
                                   static_cast<float> (getHeight()) / static_cast<float> (designHeight));

    if (scale <= 0.0f || (getWidth() == designWidth && getHeight() == designHeight))
    {
        content.setTransform ({});
        return;
    }

    const auto dx = (static_cast<float> (getWidth()) - static_cast<float> (designWidth) * scale) * 0.5f;
    const auto dy = (static_cast<float> (getHeight()) - static_cast<float> (designHeight) * scale) * 0.5f;
    content.setTransform (juce::AffineTransform::scale (scale).translated (dx, dy));
}

void SynthPanel::visibilityChanged()
{
    if (isVisible())
    {
        timerCallback();
        startTimerHz (timerHz);
    }
    else
    {
        stopTimer();
    }
}

void SynthPanel::layoutContent()
{
    auto& p = *parts;
    const auto inner = juce::Rectangle<int> (designWidth, designHeight).reduced (outer);
    auto area = inner;

    // --- Top bar: preset bar centred, DSP text right
    {
        auto bar = area.removeFromTop (topBarHeight);
        presetBar.setBounds (bar.withSizeKeepingCentre (480, topBarHeight));
        p.dspLabel.setBounds (bar.removeFromRight (296));
        area.removeFromTop (gap);
    }

    const auto knobRow = [] (juce::Rectangle<int> r, std::initializer_list<juce::Component*> cs) { placeRow (r, cs, 2); };

    // --- Row 1: OSC A, OSC B, SUB + NOISE, VOICE
    {
        auto row = area.removeFromTop (oscRowHeight);
        area.removeFromTop (gap);
        const auto w = splitWidths (row.getWidth() - 3 * gap, { 340.0f, 340.0f, 168.0f, 218.0f });

        for (std::size_t i = 0; i < 2; ++i)
        {
            auto& s = *p.oscSection[i];
            s.setBounds (row.removeFromLeft (w[i]));
            row.removeFromLeft (gap);
            auto c = s.getContentBounds();
            p.views[i].setBounds (c.removeFromTop (60));
            c.removeFromTop (6);
            auto& o = p.osc[i];
            const auto rowH = (c.getHeight() - 4) / 2;
            knobRow (c.removeFromTop (rowH), { o.position, o.octave, o.semi, o.fine, o.level });
            c.removeFromTop (4);
            knobRow (c.removeFromTop (rowH), { o.pan, o.unison, o.detune, o.spread, o.phase });
        }

        {
            auto column = row.removeFromLeft (w[2]);
            row.removeFromLeft (gap);
            const auto subH = (column.getHeight() - gap) / 2;
            p.subSection->setBounds (column.removeFromTop (subH));
            column.removeFromTop (gap);
            p.noiseSection->setBounds (column);

            auto c = p.subSection->getContentBounds();
            p.subLevel->setBounds (c.removeFromLeft (62));
            c.removeFromLeft (gap);
            auto chips = c.withSizeKeepingCentre (c.getWidth(), 2 * theme::chipHeight + gap);
            p.subOctave->setBounds (chips.removeFromTop (theme::chipHeight));
            chips.removeFromTop (gap);
            p.subShape->setBounds (chips.removeFromTop (theme::chipHeight));

            auto n = p.noiseSection->getContentBounds();
            knobRow (n, { p.noiseLevel, p.noiseColor });
        }

        {
            auto& s = *p.voiceSection;
            s.setBounds (row);
            auto c = s.getContentBounds();
            const auto chipH = theme::chipCaptionHeight + theme::chipHeight;
            placeRow (c.removeFromTop (chipH), { p.playMode, p.glideMode }, gap);
            c.removeFromTop (gap);
            auto knobs = c.removeFromTop (66);
            const auto bendW = 62;
            p.bendRange->setBounds (knobs.removeFromRight (bendW).withSizeKeepingCentre (bendW, chipH));
            knobs.removeFromRight (gap);
            knobRow (knobs, { p.glideTime, p.ampVelocity });
            c.removeFromTop (6);
            p.voiceMetersCaption.setBounds (c.removeFromTop (13));
            c.removeFromTop (2);
            p.meters.setBounds (c);
        }
    }

    // --- Row 2: FILTER, ENV 1 (+ Gain), ENV 2, ENV 3
    {
        auto row = area.removeFromTop (filterRowHeight);
        area.removeFromTop (gap);
        const auto pad = 2 * theme::sectionPadding;
        const auto w = splitWidths (row.getWidth() - 3 * gap, { 5.0f * 51.0f + 82.0f + pad, 5.0f * 51.0f + pad,
                                                                4.0f * 51.0f + pad, 4.0f * 51.0f + pad });

        auto& f = *p.filterSection;
        f.setBounds (row.removeFromLeft (w[0]));
        row.removeFromLeft (gap);
        auto c = f.getContentBounds();
        p.filterMode->setBounds (c.removeFromLeft (76).withSizeKeepingCentre (76, theme::chipCaptionHeight + theme::chipHeight));
        c.removeFromLeft (6);
        knobRow (c, { p.filterKnobs[0], p.filterKnobs[1], p.filterKnobs[2], p.filterKnobs[3], p.filterKnobs[4] });

        for (std::size_t e = 0; e < 3; ++e)
        {
            auto& s = *p.envSection[e];
            s.setBounds (e == 2 ? row : row.removeFromLeft (w[e + 1]));
            row.removeFromLeft (gap);
            const auto& k = p.envKnobs[e];

            if (e == 0)
                knobRow (s.getContentBounds(), { k[0], k[1], k[2], k[3], p.ampGain });
            else
                knobRow (s.getContentBounds(), { k[0], k[1], k[2], k[3] });
        }
    }

    // --- Row 3: LFO 1, LFO 2, MOD MATRIX
    {
        auto row = area.removeFromTop (lfoRowHeight);
        area.removeFromTop (gap);

        for (std::size_t l = 0; l < 2; ++l)
        {
            auto& s = *p.lfoSection[l];
            s.setBounds (row.removeFromLeft (232));
            row.removeFromLeft (gap);
            auto c = s.getContentBounds();
            auto chips = c.removeFromLeft (98);
            c.removeFromLeft (gap);
            auto& lfo = p.lfo[l];
            const auto chipH = theme::chipCaptionHeight + theme::chipHeight;
            const auto spare = (chips.getHeight() - 2 * chipH - theme::chipHeight) / 2;
            lfo.shape->setBounds (chips.removeFromTop (chipH));
            chips.removeFromTop (spare);
            lfo.sync->setBounds (chips.removeFromTop (chipH));
            chips.removeFromTop (spare);
            lfo.retrig->setBounds (chips.removeFromTop (theme::chipHeight));
            knobRow (c.withSizeKeepingCentre (c.getWidth(), juce::jmin (c.getHeight(), 78)), { lfo.rate, lfo.fade });
        }

        auto& s = *p.modSection;
        s.setBounds (row);
        auto c = s.getContentBounds();
        constexpr int rows = synth::numModSlots / 2;
        const auto columnGap = 16;
        const auto columnW = (c.getWidth() - columnGap) / 2;
        const auto rowH = theme::chipHeight;
        const auto rowGap = (c.getHeight() - rows * rowH) / (rows - 1);
        s.arrows.clear();

        for (int m = 0; m < synth::numModSlots; ++m)
        {
            const auto column = m / rows;
            const auto r = m % rows;
            auto line = juce::Rectangle<int> (c.getX() + column * (columnW + columnGap), c.getY() + r * (rowH + rowGap),
                                              columnW, rowH);
            auto& slot = p.mod[static_cast<std::size_t> (m)];
            p.modNumbers[static_cast<std::size_t> (m)].setBounds (line.removeFromLeft (14));
            line.removeFromLeft (4);
            const auto w = splitWidths (line.getWidth() - 14 - 4, { 100.0f, 116.0f, 62.0f });
            slot.source->setBounds (line.removeFromLeft (w[0]));
            s.arrows.push_back (line.removeFromLeft (14).toFloat());
            slot.dest->setBounds (line.removeFromLeft (w[1]));
            line.removeFromLeft (4);
            slot.amount->setBounds (line);
        }
    }

    // --- Row 4: CHORUS, DELAY, REVERB, MASTER
    {
        auto row = area.removeFromTop (fxRowHeight);
        const auto pad = 2 * theme::sectionPadding;
        const auto u = 70.0f;
        const auto w = splitWidths (row.getWidth() - 3 * gap, { 3.0f * u + pad, 3.0f * u + 100.0f + pad, 3.0f * u + pad,
                                                                2.0f * u + 100.0f + pad });

        p.chorusSection->setBounds (row.removeFromLeft (w[0]));
        row.removeFromLeft (gap);
        knobRow (p.chorusSection->getContentBounds(), { p.chorusKnobs[0], p.chorusKnobs[1], p.chorusKnobs[2] });

        p.delaySection->setBounds (row.removeFromLeft (w[1]));
        row.removeFromLeft (gap);
        {
            auto c = p.delaySection->getContentBounds();
            const auto chipH = theme::chipCaptionHeight + theme::chipHeight;
            auto chips = c.removeFromLeft (96).withSizeKeepingCentre (96, chipH + gap + theme::chipHeight);
            c.removeFromLeft (gap);
            p.delaySync->setBounds (chips.removeFromTop (chipH));
            chips.removeFromTop (gap);
            p.delayPingPong->setBounds (chips.removeFromTop (theme::chipHeight));
            knobRow (c, { p.delayTime, p.delayFeedback, p.delayMix });
        }

        p.reverbSection->setBounds (row.removeFromLeft (w[2]));
        row.removeFromLeft (gap);
        knobRow (p.reverbSection->getContentBounds(), { p.reverbKnobs[0], p.reverbKnobs[1], p.reverbKnobs[2] });

        p.masterSection->setBounds (row);
        {
            auto c = p.masterSection->getContentBounds();
            p.synthOn->setBounds (c.removeFromLeft (96).withSizeKeepingCentre (96, theme::chipHeight + 4));
            c.removeFromLeft (gap);
            knobRow (c, { p.synthVolume, p.dryLevel });
        }
    }

    for (auto& s : p.sections)
        s->repaint();
}

//==============================================================================
void SynthPanel::timerCallback()
{
    auto& p = *parts;
    updateTables();

    for (std::size_t i = 0; i < 2; ++i)
        p.views[i].setPosition (p.rawPosition[i]->load (std::memory_order_relaxed) / 100.0f);

    p.meters.setFrame (context.expression.read());
    updateModulationRings();
    updateDimming();

    if (p.tick++ % dspTextEveryTicks == 0)
    {
        const auto text = context.dspLoadText != nullptr ? context.dspLoadText() : juce::String();

        if (text != p.dspLabel.getText())
            p.dspLabel.setText (text, juce::dontSendNotification);

        p.dspLabel.setVisible (text.isNotEmpty());
    }
}

void SynthPanel::updateTables()
{
    auto& p = *parts;
    const auto generation = context.store.getGeneration();

    if (! p.haveGeneration || generation != p.lastGeneration)
    {
        p.haveGeneration = true;
        p.lastGeneration = generation;

        for (int s = 0; s < WavetableStore::numSlots; ++s)
        {
            p.published[static_cast<std::size_t> (s)] = context.store.getSlot (s);
            p.views[static_cast<std::size_t> (s)].setTable (p.published[static_cast<std::size_t> (s)].table);
        }
    }

    const auto status = context.worker.getStatus();

    for (int s = 0; s < WavetableStore::numSlots; ++s)
    {
        const auto i = static_cast<std::size_t> (s);
        const auto requested = context.worker.getRequestedReference (s).reference;
        const bool loading = requested.toRefString() != p.published[i].reference.toRefString()
                             || p.published[i].table == nullptr;
        const auto name = displayNameOf (requested);

        if (p.tableButtons[i].getButtonText() != name)
            p.tableButtons[i].setButtonText (name);

        if (status.errorSlot == s && status.lastError.isNotEmpty())
            p.views[i].setMessage (status.lastError, true);
        else if (loading && status.busy)
            p.views[i].setMessage ("Loading " + name + "...", false);
        else
            p.views[i].setMessage ({}, false);
    }
}

void SynthPanel::updateModulationRings()
{
    auto& p = *parts;

    // Destination -> knob and how +100 % of a source value 1 moves its plain value (docs/SYNTH-DESIGN.md 9.2)
    enum class Law
    {
        none,
        fullRange, ///< +1 = the whole (linear) knob range
        octaves5,  ///< cutoff: +60 semitones
        octaves4   ///< LFO rate: +4 octaves
    };

    struct Target
    {
        Knob* knob = nullptr;
        Law law = Law::none;
        float lo = 0.0f, hi = 0.0f;
    };

    std::array<Target, synth::numModDests> targets {};
    const auto set = [&] (synth::ModDest d, Knob* k, Law law) { targets[static_cast<std::size_t> (d)] = { k, law, 0.0f, 0.0f }; };
    using D = synth::ModDest;
    set (D::oscAPosition, p.osc[0].position, Law::fullRange);
    set (D::oscALevel, p.osc[0].level, Law::fullRange);
    set (D::oscADetune, p.osc[0].detune, Law::fullRange);
    set (D::oscBPosition, p.osc[1].position, Law::fullRange);
    set (D::oscBLevel, p.osc[1].level, Law::fullRange);
    set (D::oscBDetune, p.osc[1].detune, Law::fullRange);
    set (D::subLevel, p.subLevel, Law::fullRange);
    set (D::noiseLevel, p.noiseLevel, Law::fullRange);
    set (D::filterCutoff, p.filterKnobs[0], Law::octaves5);
    set (D::filterResonance, p.filterKnobs[1], Law::fullRange);
    set (D::filterDrive, p.filterKnobs[2], Law::fullRange);
    set (D::lfo1Rate, p.lfo[0].rate, Law::octaves4);
    set (D::lfo2Rate, p.lfo[1].rate, Law::octaves4);
    set (D::chorusMix, p.chorusKnobs[2], Law::fullRange);
    set (D::delayMix, p.delayMix, Law::fullRange);
    set (D::reverbMix, p.reverbKnobs[2], Law::fullRange);

    for (std::size_t m = 0; m < synth::numModSlots; ++m)
    {
        const auto source = juce::roundToInt (p.rawModSource[m]->load (std::memory_order_relaxed));
        const auto dest = juce::roundToInt (p.rawModDest[m]->load (std::memory_order_relaxed));
        const auto amount = p.rawModAmount[m]->load (std::memory_order_relaxed) / 100.0f;

        if (source <= 0 || source >= synth::numModSources || dest <= 0 || dest >= synth::numModDests || amount == 0.0f)
            continue;

        const auto s = static_cast<synth::ModSource> (source);
        const bool bipolarSource = s == synth::ModSource::lfo1 || s == synth::ModSource::lfo2
                                   || s == synth::ModSource::note || s == synth::ModSource::randomPerNote
                                   || s == synth::ModSource::voicePitch;
        auto& t = targets[static_cast<std::size_t> (dest)];
        const auto a = amount;
        const auto b = bipolarSource ? -amount : 0.0f;
        t.lo += juce::jmin (a, b);
        t.hi += juce::jmax (a, b);
    }

    for (auto& t : targets)
    {
        if (t.knob == nullptr)
            continue;

        auto& slider = t.knob->getSlider();

        if (t.lo == 0.0f && t.hi == 0.0f)
        {
            t.knob->setModulationRange ({});
            continue;
        }

        const auto v = slider.getValue();
        const auto norm = [&] (float m) -> double
        {
            switch (t.law)
            {
                case Law::fullRange: return juce::jlimit (0.0, 1.0, slider.valueToProportionOfLength (v) + m);
                case Law::octaves5: return juce::jlimit (0.0, 1.0, slider.valueToProportionOfLength (
                                                                       juce::jlimit (slider.getMinimum(), slider.getMaximum(),
                                                                                     v * std::exp2 (5.0 * m))));
                case Law::octaves4: return juce::jlimit (0.0, 1.0, slider.valueToProportionOfLength (
                                                                       juce::jlimit (slider.getMinimum(), slider.getMaximum(),
                                                                                     v * std::exp2 (4.0 * m))));
                case Law::none: break;
            }

            return slider.valueToProportionOfLength (v);
        };

        t.knob->setModulationRange (juce::Range<double>::between (norm (t.lo), norm (t.hi)));
    }
}

void SynthPanel::updateDimming()
{
    auto& p = *parts;
    const auto on = [] (const std::atomic<float>* raw) { return raw->load (std::memory_order_relaxed) >= 0.5f; };

    const auto dimSection = [] (SectionPanel& section, bool active, std::initializer_list<juce::Component*> keepLit)
    {
        section.setActive (active);

        for (auto* child : section.getChildren())
        {
            bool keep = false;

            for (auto* k : keepLit)
                keep = keep || k == child;

            const auto alpha = (active || keep) ? 1.0f : dimmedAlpha;

            if (! juce::approximatelyEqual (child->getAlpha(), alpha))
                child->setAlpha (alpha);
        }
    };

    for (std::size_t i = 0; i < 2; ++i)
        dimSection (*p.oscSection[i], on (p.rawOscOn[i]), { p.osc[i].on, &p.tableButtons[i] });

    dimSection (*p.filterSection, on (p.rawFilterOn), { p.filterOn });
    dimSection (*p.chorusSection, on (p.rawChorusOn), { p.chorusOn });
    dimSection (*p.delaySection, on (p.rawDelayOn), { p.delayOn });
    dimSection (*p.reverbSection, on (p.rawReverbOn), { p.reverbOn });
    p.masterSection->setActive (on (p.rawSynthOn));

    // A synced LFO / delay ignores its Hz / ms knob
    const auto dimKnob = [] (Knob& k, bool dim)
    {
        const auto alpha = dim ? dimmedAlpha : 1.0f;

        if (! juce::approximatelyEqual (k.getAlpha(), alpha))
            k.setAlpha (alpha);
    };

    for (std::size_t l = 0; l < 2; ++l)
        dimKnob (*p.lfo[l].rate, juce::roundToInt (p.rawLfoSync[l]->load (std::memory_order_relaxed)) != 0);

    if (on (p.rawDelayOn))
        dimKnob (*p.delayTime, juce::roundToInt (p.rawDelaySync->load (std::memory_order_relaxed)) != 0);
}

//==============================================================================
juce::PopupMenu SynthPanel::createTableMenu (int slot)
{
    const auto requested = context.worker.getRequestedReference (juce::jlimit (0, 1, slot)).reference;
    juce::PopupMenu menu;
    menu.addSectionHeader ("Factory tables");

    for (int i = 0; i < synth::numFactoryTables; ++i)
    {
        const auto& info = synth::FactoryWavetables::getInfo (static_cast<synth::FactoryTable> (i));
        const auto id = juce::String (info.id.data(), info.id.size());
        juce::PopupMenu::Item item (juce::String (info.displayName.data(), info.displayName.size()));
        item.itemID = factoryTableItemBase + i;
        item.isTicked = requested.kind == WavetableReference::Kind::factory && requested.id == id;
        item.shortcutKeyDescription = juce::String (info.numFrames) + " frames";
        menu.addItem (item);
    }

    menu.addSeparator();

    if (requested.kind == WavetableReference::Kind::user)
    {
        juce::PopupMenu::Item item (displayNameOf (requested));
        item.itemID = currentUserTableItem;
        item.isTicked = true;
        menu.addSectionHeader ("Imported");
        menu.addItem (item);
        menu.addSeparator();
    }

    menu.addItem (importItem, "Import WAV" + juce::String::fromUTF8 ("\xe2\x80\xa6"));
    return menu;
}

void SynthPanel::handleTableMenuResult (int slot, int itemId)
{
    if (itemId >= factoryTableItemBase && itemId < factoryTableItemBase + synth::numFactoryTables)
    {
        const auto& info = synth::FactoryWavetables::getInfo (static_cast<synth::FactoryTable> (itemId - factoryTableItemBase));
        chooseFactoryTable (slot, juce::String (info.id.data(), info.id.size()));
    }
    else if (itemId == importItem)
    {
        launchImport (slot);
    }
}

void SynthPanel::chooseFactoryTable (int slot, const juce::String& factoryId)
{
    const auto requested = context.worker.getRequestedReference (slot).reference;

    if (requested.kind == WavetableReference::Kind::factory && requested.id == factoryId)
        return;

    context.worker.requestFactory (slot, factoryId);
    context.presets.markModified();
    timerCallback();
}

void SynthPanel::importTable (int slot, const juce::File& file, int frameSizeOverride)
{
    context.worker.requestImport (slot, file, frameSizeOverride);
    context.presets.markModified();
    parts->lastImportFolder = file.getParentDirectory();
    timerCallback();
}

void SynthPanel::showTableMenu (int slot)
{
    juce::Component::SafePointer<SynthPanel> safeThis (this);
    auto& button = parts->tableButtons[static_cast<std::size_t> (slot)];
    createTableMenu (slot).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&button).withStandardItemHeight (22),
                                          [safeThis, slot] (int result)
                                          {
                                              if (safeThis != nullptr && result != 0)
                                                  safeThis->handleTableMenuResult (slot, result);
                                          });
}

void SynthPanel::launchImport (int slot)
{
    auto& p = *parts;
    auto folder = p.lastImportFolder;

    if (! folder.isDirectory())
        folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    p.chooser = std::make_unique<juce::FileChooser> ("Import a wavetable (WAV)", folder, "*.wav;*.WAV");
    juce::Component::SafePointer<SynthPanel> safeThis (this);

    p.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                            [safeThis, slot] (const juce::FileChooser& chooser)
                            {
                                const auto file = chooser.getResult();

                                if (safeThis == nullptr || ! file.existsAsFile())
                                    return;

                                if (! needsFrameSizeChoice (file))
                                {
                                    safeThis->importTable (slot, file);
                                    return;
                                }

                                // Ambiguous file: ask for the frame size in a second step
                                juce::PopupMenu menu;
                                menu.addSectionHeader ("Frame size of " + file.getFileName());

                                for (const auto size : { 256, 512, 1024, 2048, 4096 })
                                    menu.addItem (size, juce::String (size) + " samples" + (size == 2048 ? " (default)" : ""));

                                auto& button = safeThis->parts->tableButtons[static_cast<std::size_t> (slot)];
                                menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&button),
                                                    [safeThis, slot, file] (int size)
                                                    {
                                                        if (safeThis != nullptr && size > 0)
                                                            safeThis->importTable (slot, file, size);
                                                    });
                            });
}

bool SynthPanel::needsFrameSizeChoice (const juce::File& wavFile)
{
    juce::FileInputStream in (wavFile);

    if (! in.openedOk())
        return false;

    char id[4] {};

    if (in.read (id, 4) != 4 || std::memcmp (id, "RIFF", 4) != 0)
        return false;

    in.readInt();

    if (in.read (id, 4) != 4 || std::memcmp (id, "WAVE", 4) != 0)
        return false;

    int blockAlign = 0;
    juce::int64 dataBytes = -1;
    bool hasClm = false;

    while (! in.isExhausted())
    {
        if (in.read (id, 4) != 4)
            break;

        const auto size = static_cast<juce::int64> (static_cast<uint32_t> (in.readInt()));
        const auto start = in.getPosition();

        if (std::memcmp (id, "fmt ", 4) == 0 && size >= 16)
        {
            in.readShort();          // format
            in.readShort();          // channels
            in.readInt();            // sample rate
            in.readInt();            // byte rate
            blockAlign = in.readShort();
        }
        else if (std::memcmp (id, "clm ", 4) == 0)
        {
            juce::MemoryBlock text;
            in.readIntoMemoryBlock (text, static_cast<juce::pointer_sized_int> (juce::jmin<juce::int64> (size, 256)));
            hasClm = synth::WavReader::parseClmFrameSize (
                         std::string_view (static_cast<const char*> (text.getData()), text.getSize()))
                     > 0;
        }
        else if (std::memcmp (id, "data", 4) == 0)
        {
            dataBytes = juce::jmin (size, in.getTotalLength() - start);
        }

        if (! in.setPosition (start + size + (size & 1)))
            break;
    }

    if (hasClm || blockAlign <= 0 || dataBytes < 0)
        return false;

    const auto frames = dataBytes / blockAlign;
    return frames > static_cast<juce::int64> (synth::WavetableBuilder::maxSingleCycleSamples)
           && frames % synth::Wavetable::frameSize != 0;
}

} // namespace vocalsynth::ui
