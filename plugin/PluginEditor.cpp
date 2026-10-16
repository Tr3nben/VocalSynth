#include "PluginEditor.h"

#include "Parameters.h"
#include "TestClipExporter.h"

#include <cmath>

namespace vocalsynth
{

namespace
{
constexpr int margin = 16;
constexpr int headerHeight = 52;
constexpr int pageButtonHeight = 28;
constexpr int pageButtonWidth = 150;
constexpr int rowHeight = 30;
constexpr int voiceRowHeight = 32;
constexpr int labelWidth = 100;
constexpr int sliderTextWidth = 64;

constexpr int refreshHz = 30;
constexpr int tunerFadeSteps = 32;           // note name / needle fade quantisation (repaint only on a visible step)
constexpr double dspTextIntervalMs = 500.0;  // DSP load texts change at most twice per second
constexpr float meterDecayPerTick = 0.85f;
constexpr float meterFloorDb = -60.0f;
constexpr float voiceMeterFloorDb = -70.0f; // lowest gate threshold
constexpr int histogramRowHeight = 12;
constexpr int dragThresholdPixels = 4;
constexpr int pageRadioGroup = 4711;

const juce::Colour backgroundColour { 0xff15171c };
const juce::Colour panelColour { 0xff2a2e36 };
const juce::Colour accentColour { 0xff3ddc97 };
const juce::Colour warnColour { 0xffffa53d };
const juce::Colour textColour { 0xffd8dbe0 };
const juce::Colour dimTextColour { 0xff8a9099 };

juce::String formatCount (uint64_t value)
{
    return juce::String ((juce::int64) value);
}

/** U+2013 EN DASH (the source stays ASCII; UTF-8 bytes). */
juce::String enDash()
{
    return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"));
}

/** U+2014 EM DASH. */
juce::String emDash()
{
    return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
}

/** U+2248 ALMOST EQUAL TO. */
juce::String approx()
{
    return juce::String (juce::CharPointer_UTF8 ("\xe2\x89\x88"));
}

/** " U+00B7 " MIDDLE DOT with spaces, the separator of the latency texts. */
juce::String dotSeparator()
{
    return juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  "));
}

void styleValueLabel (juce::Label& label)
{
    label.setFont (juce::FontOptions (14.0f));
    label.setColour (juce::Label::textColourId, textColour);
    label.setJustificationType (juce::Justification::centredLeft);
    label.setMinimumHorizontalScale (0.8f);
    label.setBorderSize ({ 1, 0, 1, 0 }); // align with the painted captions
}

void styleCaptionLabel (juce::Label& label)
{
    label.setFont (juce::FontOptions (14.0f));
    label.setColour (juce::Label::textColourId, dimTextColour);
    label.setJustificationType (juce::Justification::centredLeft);
    label.setBorderSize ({ 1, 0, 1, 0 });
}

float proportionOf (float db, float floorDb)
{
    return juce::jlimit (0.0f, 1.0f, juce::jmap (db, floorDb, 0.0f, 0.0f, 1.0f));
}
} // namespace

//==============================================================================
VocalSynthEditor::FileDragSource::FileDragSource (juce::String t, std::function<juce::File()> exporter)
    : text (std::move (t)),
      exportFile (std::move (exporter))
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    setTooltip ("Drag into FL Studio (pattern / playlist)");
}

void VocalSynthEditor::FileDragSource::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);

    g.setColour (isPressed ? panelColour.brighter (0.2f) : panelColour);
    g.fillRoundedRectangle (bounds, 6.0f);

    g.setColour (accentColour);
    g.drawRoundedRectangle (bounds, 6.0f, 1.5f);

    auto area = getLocalBounds().reduced (8, 2);

    g.setColour (textColour);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (text, area.removeFromTop (area.getHeight() / 2 + 4), juce::Justification::centredBottom);

    g.setColour (dimTextColour);
    g.setFont (juce::FontOptions (12.0f));
    g.drawFittedText (statusText.isNotEmpty() ? statusText : juce::String ("drag me into FL Studio"), area,
                      juce::Justification::centredTop, 1, 0.8f);
}

void VocalSynthEditor::FileDragSource::mouseDown (const juce::MouseEvent&)
{
    dragStartedThisGesture = false;
    isPressed = true;
    repaint();
}

void VocalSynthEditor::FileDragSource::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStartedThisGesture || e.getDistanceFromDragStart() < dragThresholdPixels)
        return;

    // Only once per gesture: exporting does file I/O, and a second external drag must not start
    dragStartedThisGesture = true;

    const auto file = exportFile != nullptr ? exportFile() : juce::File();

    if (! file.existsAsFile())
    {
        statusText = "export failed";
        repaint();
        return;
    }

    statusText = file.getFileName();
    repaint();

    juce::Component::SafePointer<FileDragSource> safeThis (this);

    if (! juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                      [safeThis]
                                                                      {
                                                                          if (safeThis != nullptr)
                                                                          {
                                                                              safeThis->isPressed = false;
                                                                              safeThis->repaint();
                                                                          }
                                                                      }))
    {
        statusText = "drag failed: " + file.getFileName();
        repaint();
    }
}

void VocalSynthEditor::FileDragSource::mouseUp (const juce::MouseEvent&)
{
    isPressed = false;
    repaint();
}
//==============================================================================
void VocalSynthEditor::TunerDisplay::setState (const TunerDisplayModel::State& state, bool active)
{
    if (active != isActive)
    {
        shown = state;
        isActive = active;
        repaint();
        return;
    }

    const auto oldNote = noteKey();
    const auto oldText = textKey();
    const auto oldNeedle = needleKey();
    const auto oldBottom = bottomKey();
    shown = state;

    // Only the regions whose visible content changed
    if (! (noteKey() == oldNote))
        repaint (noteArea);

    if (! (textKey() == oldText))
        repaint (textArea);

    if (! (needleKey() == oldNeedle))
        repaint (scaleArea);

    if (! (bottomKey() == oldBottom))
        repaint (bottomArea);
}

VocalSynthEditor::TunerDisplay::NoteKey VocalSynthEditor::TunerDisplay::noteKey() const noexcept
{
    return { shown.note, shown.noteIsSegmented, juce::roundToInt (shown.noteAlpha * (float) tunerFadeSteps) };
}

VocalSynthEditor::TunerDisplay::TextKey VocalSynthEditor::TunerDisplay::textKey() const noexcept
{
    return { juce::roundToInt (shown.displayHz * 10.0f), shown.displayCents };
}

VocalSynthEditor::TunerDisplay::NeedleKey VocalSynthEditor::TunerDisplay::needleKey() const noexcept
{
    if (! isActive || shown.note < 0 || shown.displayHz <= 0.0f)
        return {};

    return { juce::roundToInt (needleX (shown.needleCents) * 2.0f),
             juce::roundToInt (shown.noteAlpha * (float) tunerFadeSteps),
             std::abs (shown.needleCents) <= TunerDisplayModel::inTuneCents };
}

VocalSynthEditor::TunerDisplay::BottomKey VocalSynthEditor::TunerDisplay::bottomKey() const noexcept
{
    return { shown.ledOn, juce::roundToInt ((float) barArea.getWidth() * juce::jlimit (0.0f, 1.0f, shown.confidence)) };
}

float VocalSynthEditor::TunerDisplay::needleX (float cents) const noexcept
{
    const auto line = scaleLineArea.toFloat();
    const auto range = TunerDisplayModel::maxNeedleCents;
    return line.getX() + line.getWidth() * (juce::jlimit (-range, range, cents) + range) / (2.0f * range);
}

void VocalSynthEditor::TunerDisplay::resized()
{
    auto area = getLocalBounds().reduced (14, 10);

    auto top = area.removeFromTop (56);
    noteArea = top.removeFromLeft (130);
    textArea = top;

    area.removeFromTop (6);
    scaleArea = area.removeFromTop (44);
    scaleLineArea = scaleArea.withHeight (30);

    area.removeFromTop (6);
    bottomArea = area.removeFromTop (22);
    barArea = bottomArea.withTrimmedLeft (76).withTrimmedRight (78).reduced (2, 5);
}

void VocalSynthEditor::TunerDisplay::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panelColour);
    g.fillRoundedRectangle (bounds, 8.0f);

    const bool hasNote = isActive && shown.note >= 0;
    const bool hasPitch = hasNote && shown.displayHz > 0.0f;
    const auto alpha = juce::jlimit (0.0f, 1.0f, shown.noteAlpha);

    // Note name (FL convention): white while it is the note being sent, lighter grey for the nearest note
    // without a MIDI note; fades to grey during the release
    const auto noteColour = shown.noteIsSegmented ? juce::Colours::white : textColour;
    g.setColour (hasNote ? dimTextColour.interpolatedWith (noteColour, alpha) : dimTextColour);
    g.setFont (juce::FontOptions (44.0f, juce::Font::bold));
    g.drawText (hasNote ? getNoteName (shown.note) : juce::String ("-"), noteArea, juce::Justification::centredLeft);

    // Frequency and cents (stepped values, at most 5 changes per second)
    auto text = textArea;
    g.setFont (juce::FontOptions (16.0f));
    g.setColour (textColour);
    g.drawText (hasPitch ? formatTunerHz (shown.displayHz) : juce::String ("- Hz"), text.removeFromTop (28),
                juce::Justification::centredRight);
    g.setColour (dimTextColour);
    g.drawText (hasPitch ? (shown.displayCents >= 0 ? "+" : "") + juce::String (shown.displayCents) + " cents"
                         : juce::String ("- cents"),
                text, juce::Justification::centredRight);

    // Cents scale -50 .. +50 with the needle
    const auto scaleLine = scaleLineArea.toFloat();
    g.setColour (backgroundColour);
    g.fillRoundedRectangle (scaleLine, 4.0f);

    g.setColour (dimTextColour);
    for (const auto tick : { -50, -25, 0, 25, 50 })
    {
        const auto x = needleX ((float) tick);
        const auto inset = tick == 0 ? 2.0f : 9.0f;
        g.drawLine (x, scaleLine.getY() + inset, x, scaleLine.getBottom() - inset, tick == 0 ? 2.0f : 1.0f);
    }

    g.setFont (juce::FontOptions (11.0f));
    for (const auto tick : { -50, 0, 50 })
        g.drawText ((tick > 0 ? "+" : "") + juce::String (tick),
                    juce::Rectangle<float> (needleX ((float) tick) - 20.0f, scaleLine.getBottom(), 40.0f, 14.0f),
                    juce::Justification::centred);

    if (hasPitch)
    {
        const auto x = needleX (shown.needleCents);
        const auto inTune = std::abs (shown.needleCents) <= TunerDisplayModel::inTuneCents;
        g.setColour ((inTune ? accentColour : warnColour).withMultipliedAlpha (0.25f + 0.75f * alpha));
        g.fillRoundedRectangle (x - 2.5f, scaleLine.getY() + 1.0f, 5.0f, scaleLine.getHeight() - 2.0f, 2.0f);
    }

    // Bottom: confidence bar and voiced LED
    auto bottom = bottomArea;
    auto led = bottom.removeFromRight (78);
    const auto ledCircle = led.removeFromLeft (20).toFloat().withSizeKeepingCentre (14.0f, 14.0f);
    const bool ledOn = isActive && shown.ledOn;
    g.setColour (ledOn ? accentColour : backgroundColour);
    g.fillEllipse (ledCircle);
    g.setColour (dimTextColour);
    g.drawEllipse (ledCircle, 1.0f);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Voiced", led.withTrimmedLeft (4), juce::Justification::centredLeft);

    g.drawText ("Confidence", bottom.removeFromLeft (76), juce::Justification::centredLeft);
    const auto bar = barArea.toFloat();
    g.setColour (backgroundColour);
    g.fillRoundedRectangle (bar, 3.0f);

    if (isActive)
    {
        g.setColour (ledOn ? accentColour : dimTextColour);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, shown.confidence)), 3.0f);
    }
    else
    {
        g.setColour (dimTextColour);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Source = Test Pattern: no voice analysis", getLocalBounds().reduced (14, 10).withTop (bottomArea.getBottom()),
                    juce::Justification::centred);
    }
}

//==============================================================================
juce::String VocalSynthEditor::getFormatName (juce::AudioProcessor::WrapperType wrapperType)
{
    // clap-juce-extensions calls createPluginFilter() directly and never sets a wrapper type
    if (wrapperType == juce::AudioProcessor::wrapperType_Undefined)
        return "CLAP";

    return juce::AudioProcessor::getWrapperTypeDescription (wrapperType);
}

juce::String VocalSynthEditor::getNoteName (int midiNote)
{
    if (midiNote < 0 || midiNote > 127)
        return "-";

    // FL Studio's piano roll: C5 = 60 (JUCE's octave number for middle C = 5)
    return juce::MidiMessage::getMidiNoteName (midiNote, true, true, 5);
}

juce::String VocalSynthEditor::formatLatency (double milliseconds)
{
    return juce::String (milliseconds, 1) + " ms";
}

juce::String VocalSynthEditor::formatTunerHz (float hz)
{
    if (! (hz > 0.0f))
        return "- Hz";

    if (juce::roundToInt (hz) < 1000)
        return juce::String (juce::roundToInt (hz)) + " Hz";

    return juce::String (hz / 1000.0f, 2) + " kHz";
}

VocalSynthEditor::LatencyInfo VocalSynthEditor::describeLatency (double analysisMs, int bufferSamples, double sampleRate)
{
    LatencyInfo info;

    if (bufferSamples <= 0 || sampleRate <= 0.0)
    {
        info.bufferText = "Buffer: - (no audio yet)";
        info.totalText = "Total: - (needs the buffer size)";
        return info;
    }

    const auto bufferMs = 1000.0 * (double) bufferSamples / sampleRate;

    // What the user hears: analysis + input buffer + output buffer (one host block each)
    const auto totalMs = analysisMs + 2.0 * bufferMs;

    info.bufferText = "Buffer " + juce::String (bufferSamples) + " smp = " + formatLatency (bufferMs);
    info.totalText = "Total " + approx() + " " + juce::String (juce::roundToInt (totalMs)) + " ms (analysis + in/out buffer)"
                     + dotSeparator() + "FL MIDI port " + approx() + " +" + juce::String (juce::roundToInt (bufferMs))
                     + " ms";
    info.largeBuffer = bufferSamples > largeBufferSamples;
    return info;
}

juce::String VocalSynthEditor::largeBufferHint()
{
    return "Large audio buffer adds latency " + emDash() + " try 128 or 256 samples in FL's audio settings";
}

juce::String VocalSynthEditor::formatDspLoad (const DspLoadMeter::Reading& reading)
{
    if (! reading.valid)
        return "VocalSynth DSP: - (no audio yet)";

    return "VocalSynth DSP: avg " + juce::String (100.0f * reading.averageProportion, 1) + " % / peak "
           + juce::String (100.0f * reading.peakProportion, 1) + " % of the block time";
}

VocalSynthEditor::VocalSynthEditor (VocalSynthProcessor& p)
    : AudioProcessorEditor (p),
      vsProcessor (p),
      formatName (getFormatName (p.wrapperType)),
      midiDragSource ("Drag test MIDI",
                      [&p] { return TestClipExporter::exportMidi (p.getTestPatternSettings()); }),
      wavDragSource ("Drag test WAV", [] { return TestClipExporter::exportWav(); })
{
    auto& apvts = p.getParameters();

    // --- Page switch ---
    for (auto* button : { &voicePageButton, &testPageButton })
    {
        button->setClickingTogglesState (false);
        button->setRadioGroupId (pageRadioGroup, juce::dontSendNotification);
        button->setColour (juce::TextButton::buttonOnColourId, accentColour.darker (0.6f));
        addAndMakeVisible (*button);
    }

    voicePageButton.setTooltip ("Tuner and the voice -> MIDI settings");
    testPageButton.setTooltip ("M1 test pattern, MIDI channel, Monitor Tone, virtual port, diagnostics");
    voicePageButton.onClick = [this] { showPage (Page::voice); };
    testPageButton.onClick = [this] { showPage (Page::testAndRouting); };

    const auto addCombo = [this] (juce::ComboBox& box, const juce::StringArray& items, const juce::String& tooltip)
    {
        box.addItemList (items, 1); // item ID = choice index + 1 (the attachment maps them)
        box.setTooltip (tooltip);
        addChildComponent (box);
    };

    const auto addCaption = [this] (juce::Label& label, std::vector<juce::Component*>& page)
    {
        styleCaptionLabel (label);
        addChildComponent (label);
        page.push_back (&label);
    };

    const auto addSlider = [this] (juce::Slider& slider, const juce::String& tooltip)
    {
        slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, sliderTextWidth, 22);
        slider.setTooltip (tooltip);
        addChildComponent (slider);
    };

    // --- Voice page ---
    addChildComponent (tuner);
    voicePageComponents.push_back (&tuner);

    addCombo (sourceBox, params::sourceChoices, "Voice: your singing makes the MIDI. Test Pattern: the M1 test MIDI");
    addCombo (voiceRangeBox, params::voiceRangeChoices,
              "Your voice type: limits the pitch search. Lower ranges need a longer window (more latency)");
    addCombo (algorithmBox, params::pitchAlgorithmChoices, "Pitch detector: YIN or MPM");
    addCombo (keyBox, params::keyChoices, "Key of the scale");
    addCombo (scaleBox, params::scaleChoices, "Notes are snapped to this scale (Chromatic = every semitone)");
    addCombo (bendModeBox, params::bendModeChoices,
              "Off: notes only. Anchored: bends only when you clearly leave the note. True: follows every cent");
    addCombo (bendRangeBox, params::bendRangeChoices, "Must match the pitch-bend range set in the synth");

    addSlider (stickinessSlider, "How firmly a note is held before switching to the next one (more = steadier, slower)");
    addSlider (gateSlider, "Input level below which nothing is detected (the line in the meter)");
    addSlider (minNoteSlider, "Shortest note length: shorter wobbles are merged into the previous note");
    addSlider (velocitySlider, "0 = every note at velocity 100; 100 = velocity follows how loud you start the note");
    addSlider (octaveSlider, "Shifts the MIDI notes by whole octaves");
    octaveSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);

    legatoButton.setTooltip ("On: the new note starts before the old one ends (for legato/glide synths)");
    addChildComponent (legatoButton);

    const std::pair<juce::Label*, juce::Component*> voiceRows[] = {
        { &sourceLabel, &sourceBox },         { &voiceRangeLabel, &voiceRangeBox },
        { &algorithmLabel, &algorithmBox },   { &keyLabel, &keyBox },
        { &scaleLabel, &scaleBox },           { &bendModeLabel, &bendModeBox },
        { &bendRangeLabel, &bendRangeBox },   { &stickinessLabel, &stickinessSlider },
        { &gateLabel, &gateSlider },          { &minNoteLabel, &minNoteSlider },
        { &velocityLabel, &velocitySlider },  { &octaveLabel, &octaveSlider },
    };

    for (const auto& [label, control] : voiceRows)
    {
        addCaption (*label, voicePageComponents);
        voicePageComponents.push_back (control);
    }

    voicePageComponents.push_back (&legatoButton);

    for (auto* label : { &soundingNoteLabel, &voiceHintLabel, &meterCaptionLabel, &latencyLabel, &totalLatencyLabel,
                         &bufferWarningLabel, &dspLoadVoiceLabel })
    {
        styleValueLabel (*label);
        addChildComponent (*label);
        voicePageComponents.push_back (label);
    }

    voiceHintLabel.setFont (juce::FontOptions (13.0f));
    voiceHintLabel.setColour (juce::Label::textColourId, dimTextColour);
    meterCaptionLabel.setColour (juce::Label::textColourId, dimTextColour);
    latencyLabel.setTooltip ("Analysis: worst-case delay from the start of a sung note to its note-on (analysis "
                             "window + hops) for the selected voice range. Buffer: the largest audio block FL "
                             "sent in the last 1-2 seconds (= the ASIO buffer length)");
    totalLatencyLabel.setTooltip ("Estimate of what you hear: analysis + audio input buffer + audio output buffer "
                                  "(driver extras not included). Sending the MIDI through FL's MIDI port to "
                                  "another plugin can add about one more buffer");
    bufferWarningLabel.setTooltip (largeBufferHint() + " (Options > Audio settings > Buffer length)");
    bufferWarningLabel.setFont (juce::FontOptions (13.0f));
    bufferWarningLabel.setColour (juce::Label::textColourId, warnColour);
    dspLoadVoiceLabel.setFont (juce::FontOptions (13.0f));
    dspLoadVoiceLabel.setColour (juce::Label::textColourId, dimTextColour);

    const auto dspTooltip = juce::String ("Time VocalSynth itself needs per audio block, as a share of the block's "
                                          "duration (average over ~1 s, peak falls back over a few seconds). FL's CPU "
                                          "meter also includes the synth and everything else");
    dspLoadVoiceLabel.setTooltip (dspTooltip);

    // --- Test & Routing page (M1) ---
    for (auto* label : { &patternLabel, &channelLabel, &tempoLabel, &ccLabel })
        addCaption (*label, testPageComponents);

    addCombo (patternBox, params::testPatternChoices, "Used when Source = Test Pattern");
    testPageComponents.push_back (&patternBox);

    for (auto* slider : { &channelSlider, &tempoSlider, &ccSlider })
    {
        addSlider (*slider, {});
        testPageComponents.push_back (slider);
    }

    channelSlider.setTooltip ("MIDI channel of the voice notes and the test pattern");
    channelSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    ccSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);

    monitorToneButton.setTooltip ("Internal sine that plays the MIDI notes (test pattern or voice), no routing needed");
    virtualMidiButton.setTooltip ("Mirror the MIDI to a virtual MIDI port (T7)");
    restartButton.setTooltip ("Note-offs for everything and restart the pattern from its beginning");
    restartButton.onClick = [this] { vsProcessor.requestFlush(); };
    resetHistogramButton.onClick = [this]
    {
        vsProcessor.resetBlockSizeHistogram();
        histogram = vsProcessor.getBlockSizeHistogram();
        updateStatusTexts();
        repaint (histogramBounds);
    };

    for (auto* c : std::initializer_list<juce::Component*> { &monitorToneButton, &virtualMidiButton, &restartButton,
                                                             &resetHistogramButton, &midiDragSource, &wavDragSource })
    {
        addChildComponent (c);
        testPageComponents.push_back (c);
    }

    // The instrument never outputs the dry voice, so the switch only matters for the FX
    addChildComponent (muteDryButton);
    if (p.getVariant() == Variant::fx)
        testPageComponents.push_back (&muteDryButton);

    for (auto* label : { &virtualMidiStatusLabel, &inputChannelsLabel, &testNoteLabel, &sampleRateLabel,
                         &sentCountsLabel, &dspLoadTestLabel, &histogramSummaryLabel })
    {
        styleValueLabel (*label);
        addChildComponent (*label);
        testPageComponents.push_back (label);
    }

    virtualMidiStatusLabel.setFont (juce::FontOptions (13.0f));
    inputChannelsLabel.setJustificationType (juce::Justification::centredRight);
    sentCountsLabel.setJustificationType (juce::Justification::topLeft);
    sentCountsLabel.setFont (juce::FontOptions (13.0f));
    histogramSummaryLabel.setFont (juce::FontOptions (13.0f));
    dspLoadTestLabel.setFont (juce::FontOptions (13.0f));
    dspLoadTestLabel.setTooltip (dspTooltip);

    // --- Attachments ---
    const auto attachCombo = [this, &apvts] (const juce::ParameterID& id, juce::ComboBox& box)
    { comboBoxAttachments.push_back (std::make_unique<ComboBoxAttachment> (apvts, id.getParamID(), box)); };
    const auto attachSlider = [this, &apvts] (const juce::ParameterID& id, juce::Slider& slider)
    { sliderAttachments.push_back (std::make_unique<SliderAttachment> (apvts, id.getParamID(), slider)); };
    const auto attachButton = [this, &apvts] (const juce::ParameterID& id, juce::Button& button)
    { buttonAttachments.push_back (std::make_unique<ButtonAttachment> (apvts, id.getParamID(), button)); };

    attachCombo (params::source, sourceBox);
    attachCombo (params::voiceRange, voiceRangeBox);
    attachCombo (params::pitchAlgorithm, algorithmBox);
    attachCombo (params::key, keyBox);
    attachCombo (params::scale, scaleBox);
    attachCombo (params::bendMode, bendModeBox);
    attachCombo (params::bendRange, bendRangeBox);
    attachSlider (params::stickiness, stickinessSlider);
    attachSlider (params::gateThreshold, gateSlider);
    attachSlider (params::minNoteLength, minNoteSlider);
    attachSlider (params::velocitySensitivity, velocitySlider);
    attachSlider (params::octaveShift, octaveSlider);
    attachButton (params::legato, legatoButton);

    attachCombo (params::testPattern, patternBox);
    attachSlider (params::midiChannel, channelSlider);
    attachSlider (params::testTempo, tempoSlider);
    attachSlider (params::ccNumber, ccSlider);
    attachButton (params::monitorTone, monitorToneButton);
    attachButton (params::virtualMidiOut, virtualMidiButton);
    attachButton (params::muteDry, muteDryButton);

    histogram = vsProcessor.getBlockSizeHistogram();
    updateStatusTexts();
    updateVoiceTexts (vsProcessor.getVoiceSnapshot(), 0.0);
    updateDspLoadTexts();

    setSize (defaultWidth, defaultHeight);
    showPage (Page::voice);
    startTimerHz (refreshHz);
}

VocalSynthEditor::~VocalSynthEditor()
{
    stopTimer();
}

void VocalSynthEditor::showPage (Page page)
{
    currentPage = page;

    for (auto* c : voicePageComponents)
        c->setVisible (page == Page::voice);

    for (auto* c : testPageComponents)
        c->setVisible (page == Page::testAndRouting);

    voicePageButton.setToggleState (page == Page::voice, juce::dontSendNotification);
    testPageButton.setToggleState (page == Page::testAndRouting, juce::dontSendNotification);
    repaint();
}

void VocalSynthEditor::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    auto header = getLocalBounds().reduced (margin, 0).withTrimmedTop (8).removeFromTop (headerHeight);

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText (vsProcessor.getName(), header.removeFromTop (26), juce::Justification::centredLeft);

    g.setColour (dimTextColour);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Version " VOCALSYNTH_VERSION_STRING " " + enDash() + " M2 pitch to MIDI   |   Format: " + formatName,
                header.removeFromTop (18), juce::Justification::centredLeft);

    if (currentPage == Page::voice)
    {
        g.setColour (panelColour.withAlpha (0.6f));
        g.fillRoundedRectangle (infoPanelBounds.toFloat(), 6.0f);
        paintInputMeter (g);
    }
    else
    {
        paintHistogram (g);
    }
}

void VocalSynthEditor::paintInputMeter (juce::Graphics& g)
{
    // -70 .. 0 dBFS: the full gate range. Bar = input peak, thin bar = analysis RMS (what the gate sees)
    g.setColour (panelColour);
    g.fillRoundedRectangle (voiceMeterBounds.toFloat(), 4.0f);

    const auto bounds = voiceMeterBounds.toFloat();
    const auto peakDb = juce::Decibels::gainToDecibels (displayedLevel, -100.0f);
    g.setColour (peakDb > -3.0f ? juce::Colours::orangered : accentColour.withAlpha (0.55f));
    g.fillRoundedRectangle (bounds.withWidth (bounds.getWidth() * proportionOf (peakDb, voiceMeterFloorDb)), 4.0f);

    const auto rmsBar = bounds.withTrimmedTop (bounds.getHeight() * 0.6f);
    g.setColour (meterGateOpen ? accentColour : dimTextColour);
    g.fillRect (rmsBar.withWidth (rmsBar.getWidth() * proportionOf (meterRmsDb, voiceMeterFloorDb)));

    // Gate line
    const auto gateX = bounds.getX() + bounds.getWidth() * proportionOf (meterGateDb, voiceMeterFloorDb);
    g.setColour (juce::Colours::white);
    g.drawLine (gateX, bounds.getY() - 3.0f, gateX, bounds.getBottom() + 3.0f, 2.0f);

    // Scale
    g.setColour (dimTextColour);
    g.setFont (juce::FontOptions (11.0f));

    for (const auto db : { -70, -50, -30, -10, 0 })
    {
        const auto x = bounds.getX() + bounds.getWidth() * proportionOf ((float) db, voiceMeterFloorDb);
        g.drawText (juce::String (db), juce::Rectangle<float> (x - 16.0f, bounds.getBottom() + 3.0f, 32.0f, 12.0f),
                    db == 0 ? juce::Justification::centredRight : juce::Justification::centred);
    }
}

void VocalSynthEditor::paintHistogram (juce::Graphics& g)
{
    // Input level meter (dBFS, -60 .. 0) of the M1 page
    const auto label = vsProcessor.getVariant() == Variant::instrument ? "Sidechain input" : "Input";
    g.setColour (dimTextColour);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (label, testMeterBounds.withY (testMeterBounds.getY() - 20).withHeight (18), juce::Justification::centredLeft);

    g.setColour (panelColour);
    g.fillRoundedRectangle (testMeterBounds.toFloat(), 4.0f);

    const auto db = juce::Decibels::gainToDecibels (displayedLevel, meterFloorDb);
    g.setColour (db > -3.0f ? juce::Colours::orangered : accentColour);
    g.fillRoundedRectangle (testMeterBounds.toFloat().withWidth ((float) testMeterBounds.getWidth() * proportionOf (db, meterFloorDb)),
                            4.0f);

    // Block-size histogram: one horizontal bar per bin
    uint64_t maxCount = 0;

    for (auto c : histogram.counts)
        maxCount = juce::jmax (maxCount, c);

    g.setFont (juce::FontOptions (11.0f));
    auto rows = histogramBounds;

    for (std::size_t bin = 0; bin < BlockSizeHistogram::numBins; ++bin)
    {
        auto row = rows.removeFromTop (histogramRowHeight);
        const auto count = histogram.counts[bin];

        g.setColour (dimTextColour);
        g.drawText (BlockSizeHistogram::binLabel (bin), row.removeFromLeft (64), juce::Justification::centredLeft);

        auto countArea = row.removeFromRight (64);
        g.drawText (formatCount (count), countArea, juce::Justification::centredRight);

        auto barArea = row.reduced (2, 2).toFloat();
        g.setColour (panelColour);
        g.fillRect (barArea);

        if (count > 0 && maxCount > 0)
        {
            const auto width = juce::jmax (2.0f, barArea.getWidth() * (float) ((double) count / (double) maxCount));
            g.setColour (accentColour);
            g.fillRect (barArea.withWidth (width));
        }
    }
}

void VocalSynthEditor::resized()
{
    auto area = getLocalBounds().reduced (margin, 0);
    area.removeFromTop (8 + headerHeight + 4);

    auto tabs = area.removeFromTop (pageButtonHeight);
    voicePageButton.setBounds (tabs.removeFromLeft (pageButtonWidth));
    tabs.removeFromLeft (6);
    testPageButton.setBounds (tabs.removeFromLeft (pageButtonWidth));
    area.removeFromTop (12);
    area.removeFromBottom (margin);

    // Both pages are laid out; only one is visible (showPage)
    layoutVoicePage (area);
    layoutTestPage (area);
}

void VocalSynthEditor::layoutVoicePage (juce::Rectangle<int> area)
{
    auto left = area.removeFromLeft (300);
    area.removeFromLeft (24);
    auto middle = area.removeFromLeft (262);
    area.removeFromLeft (24);
    auto right = area;

    // Left: tuner, note, latency, meter
    tuner.setBounds (left.removeFromTop (172));
    left.removeFromTop (8);
    soundingNoteLabel.setBounds (left.removeFromTop (22));
    voiceHintLabel.setBounds (left.removeFromTop (20));
    left.removeFromTop (28);
    meterCaptionLabel.setBounds (left.removeFromTop (20));
    left.removeFromTop (3);
    voiceMeterBounds = left.removeFromTop (16);

    const auto layoutRow = [] (juce::Rectangle<int>& column, juce::Label& label, juce::Component& control, int width)
    {
        auto row = column.removeFromTop (voiceRowHeight);
        label.setBounds (row.removeFromLeft (width));
        control.setBounds (row.reduced (0, 4));
    };

    // Bottom of the middle and right columns: latency / DSP load box
    auto info = middle.getUnion (right).removeFromBottom (4 * 20 + 2 * 6);
    infoPanelBounds = info;
    info.reduce (10, 6);
    for (auto* label : { &latencyLabel, &totalLatencyLabel, &bufferWarningLabel, &dspLoadVoiceLabel })
        label->setBounds (info.removeFromTop (20));

    // Middle: dropdowns
    layoutRow (middle, sourceLabel, sourceBox, 92);
    layoutRow (middle, voiceRangeLabel, voiceRangeBox, 92);
    layoutRow (middle, algorithmLabel, algorithmBox, 92);
    layoutRow (middle, keyLabel, keyBox, 92);
    layoutRow (middle, scaleLabel, scaleBox, 92);
    layoutRow (middle, bendModeLabel, bendModeBox, 92);
    layoutRow (middle, bendRangeLabel, bendRangeBox, 92);

    // Right: sliders and Legato
    layoutRow (right, stickinessLabel, stickinessSlider, 104);
    layoutRow (right, gateLabel, gateSlider, 104);
    layoutRow (right, minNoteLabel, minNoteSlider, 104);
    layoutRow (right, velocityLabel, velocitySlider, 104);
    layoutRow (right, octaveLabel, octaveSlider, 104);
    right.removeFromTop (4);
    legatoButton.setBounds (right.removeFromTop (voiceRowHeight).withWidth (120));
}

void VocalSynthEditor::layoutTestPage (juce::Rectangle<int> area)
{
    // Bottom: drag sources
    auto dragArea = area.removeFromBottom (52);
    area.removeFromBottom (margin);
    const auto dragWidth = (dragArea.getWidth() - margin) / 2;
    midiDragSource.setBounds (dragArea.removeFromLeft (dragWidth));
    dragArea.removeFromLeft (margin);
    wavDragSource.setBounds (dragArea);

    auto left = area.removeFromLeft (320);
    area.removeFromLeft (margin + 8);
    auto right = area;

    // Left column: controls
    const auto layoutRow = [&left] (juce::Label& label, juce::Component& control)
    {
        auto row = left.removeFromTop (rowHeight);
        label.setBounds (row.removeFromLeft (labelWidth + 16));
        control.setBounds (row.reduced (0, 3));
    };

    layoutRow (patternLabel, patternBox);
    layoutRow (channelLabel, channelSlider);
    layoutRow (tempoLabel, tempoSlider);
    layoutRow (ccLabel, ccSlider);

    left.removeFromTop (4);
    monitorToneButton.setBounds (left.removeFromTop (rowHeight).withWidth (160));
    virtualMidiButton.setBounds (left.removeFromTop (rowHeight).withWidth (160));
    virtualMidiStatusLabel.setBounds (left.removeFromTop (22).withTrimmedLeft (28));
    muteDryButton.setBounds (left.removeFromTop (rowHeight).withWidth (160));
    left.removeFromTop (6);
    restartButton.setBounds (left.removeFromTop (28).withWidth (150));

    // Right column: diagnostics
    right.removeFromTop (20); // meter caption (painted)
    testMeterBounds = right.removeFromTop (14);
    inputChannelsLabel.setBounds (testMeterBounds.withY (testMeterBounds.getY() - 20).withHeight (18));
    right.removeFromTop (8);

    testNoteLabel.setBounds (right.removeFromTop (20));
    sampleRateLabel.setBounds (right.removeFromTop (20));
    sentCountsLabel.setBounds (right.removeFromTop (36));
    dspLoadTestLabel.setBounds (right.removeFromTop (20));
    right.removeFromTop (2);

    auto histogramHeader = right.removeFromTop (24);
    resetHistogramButton.setBounds (histogramHeader.removeFromRight (56).reduced (0, 2));
    histogramSummaryLabel.setBounds (histogramHeader);
    right.removeFromTop (2);
    histogramBounds = right.removeFromTop (histogramRowHeight * (int) BlockSizeHistogram::numBins);
}

void VocalSynthEditor::updateVoiceTexts (const VoiceAnalysisSnapshot& snapshot, double elapsedMs)
{
    const auto voiceSettings = vsProcessor.getVoiceSettings();
    const bool voiceSource = vsProcessor.getSource() == params::Source::voice;

    // Tuner: the sent note in the sung octave (the Octave shift is shown by "Sounding note"). Taken
    // from the same publish as activeNote: with the live Octave parameter, an Octave change flashed
    // the name one octave off until the next publish (up to ~2 blocks at a 2048 buffer).
    TunerDisplayModel::Input input;
    input.active = voiceSource;
    input.frequencyHz = snapshot.frequencyHz;
    input.nearestNote = snapshot.nearestNote;
    input.confidence = snapshot.confidence;
    input.voiced = snapshot.voiced;
    input.sungNote = snapshot.activeNote >= 0 && snapshot.quantisedNote >= 0 && snapshot.quantisedNote <= 127
                         ? snapshot.quantisedNote
                         : -1;

    tuner.setState (tunerModel.update (input, elapsedMs), voiceSource);

    const auto note = voiceSource ? snapshot.activeNote : -1;
    soundingNoteLabel.setText (note >= 0 ? "Sounding note: " + getNoteName (note) + " (" + juce::String (note) + ")"
                                         : juce::String ("Sounding note: -"),
                               juce::dontSendNotification);

    updateLatencyTexts (voiceSettings);

    voiceHintLabel.setText (voiceSource ? "MIDI channel, Monitor Tone: page Test & Routing"
                                        : "Source = Test Pattern: the test MIDI is sent",
                            juce::dontSendNotification);

    meterCaptionLabel.setText ((vsProcessor.getVariant() == Variant::instrument ? "Sidechain input" : "Input")
                                   + juce::String ("   |   gate ") + juce::String (voiceSettings.gateThresholdDb, 1)
                                   + " dB",
                               juce::dontSendNotification);

    meterRmsDb = voiceSource ? snapshot.rmsDb : -100.0f;
    meterGateDb = voiceSettings.gateThresholdDb;
    meterGateOpen = voiceSource && meterRmsDb >= meterGateDb;
}

void VocalSynthEditor::updateLatencyTexts (const VoiceSettings& voiceSettings)
{
    const auto rate = vsProcessor.getCurrentSampleRate();
    const auto shownRate = rate > 0.0 ? rate : 48000.0;
    const auto rangeName = params::voiceRangeChoices[static_cast<int> (voiceSettings.range)];
    const auto analysisMs = vsProcessor.getVoiceLatencyMs();
    const auto info = describeLatency (analysisMs, vsProcessor.getRecentMaxBlockSize(), rate);

    latencyLabel.setText ("Analysis " + formatLatency (analysisMs) + " (" + rangeName + ", "
                              + juce::String (shownRate / 1000.0, 1) + " kHz)" + dotSeparator() + info.bufferText,
                          juce::dontSendNotification);
    totalLatencyLabel.setText (info.totalText, juce::dontSendNotification);
    totalLatencyLabel.setColour (juce::Label::textColourId, info.largeBuffer ? warnColour : textColour);
    bufferWarningLabel.setText (info.largeBuffer ? largeBufferHint() : juce::String(), juce::dontSendNotification);
}

void VocalSynthEditor::updateDspLoadTexts()
{
    const auto text = formatDspLoad (vsProcessor.getDspLoad());
    dspLoadVoiceLabel.setText (text, juce::dontSendNotification);
    dspLoadTestLabel.setText (text, juce::dontSendNotification);
}

void VocalSynthEditor::updateStatusTexts()
{
    // Virtual MIDI port (T7)
    juce::String portText;

    switch (vsProcessor.getVirtualMidiStatus())
    {
        case VirtualMidiPort::Status::off:    portText = "Port: off"; break;
        case VirtualMidiPort::Status::open:   portText = "Port: open: " + vsProcessor.getVirtualMidiDeviceName(); break;
        case VirtualMidiPort::Status::failed: portText = "Port: failed - see FL-ROUTING.md"; break;
    }

    if (const auto overflows = vsProcessor.getVirtualMidiOverflowCount(); overflows > 0)
        portText << "  (overflow " << (int) overflows << ")";

    virtualMidiStatusLabel.setText (portText, juce::dontSendNotification);

    // Input channels (T3: shows whether FL enabled the sidechain bus)
    inputChannelsLabel.setText ("Input: " + juce::String (vsProcessor.getTotalNumInputChannels()) + " ch",
                                juce::dontSendNotification);

    // Current test note, named like FL Studio's piano roll (MIDI note 60 = C5)
    const auto note = vsProcessor.getCurrentTestNote();
    testNoteLabel.setText (note >= 0 ? "Test note: " + getNoteName (note) + " (" + juce::String (note) + ")"
                                     : juce::String ("Test note: -"),
                           juce::dontSendNotification);

    const auto sampleRate = vsProcessor.getCurrentSampleRate();
    sampleRateLabel.setText (sampleRate > 0.0 ? "Sample rate: " + juce::String (juce::roundToInt (sampleRate)) + " Hz"
                                              : juce::String ("Sample rate: - (not prepared)"),
                             juce::dontSendNotification);

    // Sent counters: tells "plugin did not send" apart from "FL did not route"
    const auto sent = vsProcessor.getMidiSentCounts();
    sentCountsLabel.setText ("MIDI sent: note-on " + juce::String ((juce::int64) sent.noteOns) + ", note-off "
                                 + juce::String ((juce::int64) sent.noteOffs) + "\nMIDI sent: pitch bend "
                                 + juce::String ((juce::int64) sent.pitchBends) + ", CC "
                                 + juce::String ((juce::int64) sent.controlChanges),
                             juce::dontSendNotification);

    // Block sizes
    juce::String summary;

    if (histogram.totalBlocks == 0)
        summary = "Block sizes: no blocks yet";
    else
        summary << "Block sizes  min " << histogram.minBlockSize << "  max " << histogram.maxBlockSize << "  last "
                << histogram.lastBlockSize;

    if (histogram.emptyBlocks > 0)
        summary << "  empty " << formatCount (histogram.emptyBlocks);

    histogramSummaryLabel.setText (summary, juce::dontSendNotification);
}

void VocalSynthEditor::timerCallback()
{
    // Real elapsed time, so the tuner's smoothing does not depend on how regularly the timer fires
    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    const auto elapsedMs = lastTickMs > 0.0 ? nowMs - lastTickMs : 1000.0 / refreshHz;
    lastTickMs = nowMs;

    // Pixel key of the meter as last drawn, taken before the peak and the snapshot values move
    const auto oldMeter = voiceMeterKey();

    const auto peak = vsProcessor.takeInputPeak();
    displayedLevel = juce::jmax (peak, displayedLevel * meterDecayPerTick);

    // Label::setText only repaints when the text actually changes
    updateStatusTexts();
    updateVoiceTexts (vsProcessor.getVoiceSnapshot(), elapsedMs);

    sinceDspTextMs += elapsedMs;
    if (sinceDspTextMs >= dspTextIntervalMs)
    {
        sinceDspTextMs = 0.0;
        updateDspLoadTexts();
    }

    if (currentPage == Page::voice)
    {
        if (voiceMeterKey() != oldMeter)
        {
            ++voiceMeterRepaints;
            repaint (voiceMeterBounds.expanded (4, 4));
        }

        return;
    }

    repaint (testMeterBounds);

    const auto newHistogram = vsProcessor.getBlockSizeHistogram();
    const auto histogramChanged = newHistogram.totalBlocks != histogram.totalBlocks
                                  || newHistogram.emptyBlocks != histogram.emptyBlocks
                                  || newHistogram.lastBlockSize != histogram.lastBlockSize;
    histogram = newHistogram;

    if (histogramChanged)
        repaint (histogramBounds);
}

std::array<int, 5> VocalSynthEditor::voiceMeterKey() const noexcept
{
    // Pixel positions of everything the Voice page meter draws
    const auto width = (float) voiceMeterBounds.getWidth();
    const auto peakDb = juce::Decibels::gainToDecibels (displayedLevel, -100.0f);
    return { juce::roundToInt (width * proportionOf (peakDb, voiceMeterFloorDb)), peakDb > -3.0f ? 1 : 0,
             juce::roundToInt (width * proportionOf (meterRmsDb, voiceMeterFloorDb)),
             juce::roundToInt (width * proportionOf (meterGateDb, voiceMeterFloorDb)), meterGateOpen ? 1 : 0 };
}

} // namespace vocalsynth
