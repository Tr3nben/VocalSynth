#include "PluginEditor.h"

#include "Parameters.h"
#include "TestClipExporter.h"

namespace vocalsynth
{

namespace
{
constexpr int editorWidth = 640;
constexpr int editorHeight = 440;
constexpr int margin = 16;
constexpr int headerHeight = 52;
constexpr int rowHeight = 30;
constexpr int labelWidth = 100;

constexpr int refreshHz = 30;
constexpr float meterDecayPerTick = 0.85f;
constexpr float meterFloorDb = -60.0f;
constexpr int histogramRowHeight = 12;
constexpr int dragThresholdPixels = 4;

const juce::Colour backgroundColour { 0xff15171c };
const juce::Colour panelColour { 0xff2a2e36 };
const juce::Colour accentColour { 0xff3ddc97 };
const juce::Colour textColour { 0xffd8dbe0 };
const juce::Colour dimTextColour { 0xff8a9099 };

juce::String formatCount (uint64_t value)
{
    return juce::String ((juce::int64) value);
}

void styleValueLabel (juce::Label& label)
{
    label.setFont (juce::FontOptions (14.0f));
    label.setColour (juce::Label::textColourId, textColour);
    label.setJustificationType (juce::Justification::centredLeft);
    label.setMinimumHorizontalScale (0.8f);
    label.setBorderSize ({ 1, 0, 1, 0 }); // align with the painted captions
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
juce::String VocalSynthEditor::getFormatName (juce::AudioProcessor::WrapperType wrapperType)
{
    // clap-juce-extensions calls createPluginFilter() directly and never sets a wrapper type
    if (wrapperType == juce::AudioProcessor::wrapperType_Undefined)
        return "CLAP";

    return juce::AudioProcessor::getWrapperTypeDescription (wrapperType);
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

    for (auto* label : { &patternLabel, &channelLabel, &tempoLabel, &ccLabel })
    {
        label->setFont (juce::FontOptions (14.0f));
        label->setColour (juce::Label::textColourId, dimTextColour);
        label->setJustificationType (juce::Justification::centredLeft);
        label->setBorderSize ({ 1, 0, 1, 0 });
        addAndMakeVisible (*label);
    }

    // Item IDs are choice index + 1 (the attachment maps them)
    patternBox.addItemList (params::testPatternChoices, 1);
    addAndMakeVisible (patternBox);

    for (auto* slider : { &channelSlider, &tempoSlider, &ccSlider })
    {
        slider->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 64, 22);
        addAndMakeVisible (*slider);
    }

    channelSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    ccSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);

    monitorToneButton.setTooltip ("Internal sine that follows the test pattern (T6, no routing needed)");
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

    // The instrument never outputs the dry voice, so the switch only matters for the FX
    muteDryButton.setVisible (p.getVariant() == Variant::fx);

    for (auto* c : std::initializer_list<juce::Component*> { &monitorToneButton, &virtualMidiButton, &restartButton,
                                                             &resetHistogramButton })
        addAndMakeVisible (c);

    addChildComponent (muteDryButton);

    for (auto* label : { &virtualMidiStatusLabel, &inputChannelsLabel, &testNoteLabel, &sampleRateLabel,
                         &sentCountsLabel, &histogramSummaryLabel })
    {
        styleValueLabel (*label);
        addAndMakeVisible (*label);
    }

    virtualMidiStatusLabel.setFont (juce::FontOptions (13.0f));
    inputChannelsLabel.setJustificationType (juce::Justification::centredRight);
    sentCountsLabel.setJustificationType (juce::Justification::topLeft);
    sentCountsLabel.setFont (juce::FontOptions (13.0f));
    histogramSummaryLabel.setFont (juce::FontOptions (13.0f));

    addAndMakeVisible (midiDragSource);
    addAndMakeVisible (wavDragSource);

    using APVTS = juce::AudioProcessorValueTreeState;
    patternAttachment = std::make_unique<APVTS::ComboBoxAttachment> (apvts, params::testPattern.getParamID(), patternBox);
    channelAttachment = std::make_unique<APVTS::SliderAttachment> (apvts, params::midiChannel.getParamID(), channelSlider);
    tempoAttachment = std::make_unique<APVTS::SliderAttachment> (apvts, params::testTempo.getParamID(), tempoSlider);
    ccAttachment = std::make_unique<APVTS::SliderAttachment> (apvts, params::ccNumber.getParamID(), ccSlider);
    monitorToneAttachment =
        std::make_unique<APVTS::ButtonAttachment> (apvts, params::monitorTone.getParamID(), monitorToneButton);
    virtualMidiAttachment =
        std::make_unique<APVTS::ButtonAttachment> (apvts, params::virtualMidiOut.getParamID(), virtualMidiButton);
    muteDryAttachment = std::make_unique<APVTS::ButtonAttachment> (apvts, params::muteDry.getParamID(), muteDryButton);

    histogram = vsProcessor.getBlockSizeHistogram();
    updateStatusTexts();

    setSize (editorWidth, editorHeight);
    startTimerHz (refreshHz);
}

VocalSynthEditor::~VocalSynthEditor()
{
    stopTimer();
}

void VocalSynthEditor::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    auto header = getLocalBounds().reduced (margin, 0).withTrimmedTop (8).removeFromTop (headerHeight);

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText (vsProcessor.getName() + " - M1 routing test build", header.removeFromTop (26),
                juce::Justification::centredLeft);

    g.setColour (dimTextColour);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Version " VOCALSYNTH_VERSION_STRING "   |   Format: " + formatName
                    + "   |   generates test MIDI, no voice analysis yet",
                header.removeFromTop (18), juce::Justification::centredLeft);

    // Input level meter (dBFS, -60 .. 0)
    const auto label = vsProcessor.getVariant() == Variant::instrument ? "Sidechain input" : "Input";
    g.setColour (dimTextColour);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (label, meterBounds.withY (meterBounds.getY() - 20).withHeight (18), juce::Justification::centredLeft);

    g.setColour (panelColour);
    g.fillRoundedRectangle (meterBounds.toFloat(), 4.0f);

    const auto db = juce::Decibels::gainToDecibels (displayedLevel, meterFloorDb);
    const auto proportion = juce::jlimit (0.0f, 1.0f, juce::jmap (db, meterFloorDb, 0.0f, 0.0f, 1.0f));
    g.setColour (db > -3.0f ? juce::Colours::orangered : accentColour);
    g.fillRoundedRectangle (meterBounds.toFloat().withWidth ((float) meterBounds.getWidth() * proportion), 4.0f);

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

    // Bottom: drag sources
    auto dragArea = area.removeFromBottom (margin + 52).withTrimmedBottom (margin);
    const auto dragWidth = (dragArea.getWidth() - margin) / 2;
    midiDragSource.setBounds (dragArea.removeFromLeft (dragWidth));
    dragArea.removeFromLeft (margin);
    wavDragSource.setBounds (dragArea);

    auto left = area.removeFromLeft (296);
    area.removeFromLeft (margin + 8);
    auto right = area;

    // Left column: controls
    const auto layoutRow = [&left] (juce::Label& label, juce::Component& control)
    {
        auto row = left.removeFromTop (rowHeight);
        label.setBounds (row.removeFromLeft (labelWidth));
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
    meterBounds = right.removeFromTop (14);
    inputChannelsLabel.setBounds (meterBounds.withY (meterBounds.getY() - 20).withHeight (18));
    right.removeFromTop (8);

    testNoteLabel.setBounds (right.removeFromTop (20));
    sampleRateLabel.setBounds (right.removeFromTop (20));
    sentCountsLabel.setBounds (right.removeFromTop (36));
    right.removeFromTop (4);

    auto histogramHeader = right.removeFromTop (24);
    resetHistogramButton.setBounds (histogramHeader.removeFromRight (56).reduced (0, 2));
    histogramSummaryLabel.setBounds (histogramHeader);
    right.removeFromTop (2);
    histogramBounds = right.removeFromTop (histogramRowHeight * (int) BlockSizeHistogram::numBins);
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
    testNoteLabel.setText (note >= 0 ? "Test note: " + juce::MidiMessage::getMidiNoteName (note, true, true, 5) + " ("
                                           + juce::String (note) + ")"
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
    const auto peak = vsProcessor.takeInputPeak();
    displayedLevel = juce::jmax (peak, displayedLevel * meterDecayPerTick);
    repaint (meterBounds);

    const auto newHistogram = vsProcessor.getBlockSizeHistogram();
    const auto histogramChanged = newHistogram.totalBlocks != histogram.totalBlocks
                                  || newHistogram.emptyBlocks != histogram.emptyBlocks
                                  || newHistogram.lastBlockSize != histogram.lastBlockSize;
    histogram = newHistogram;

    if (histogramChanged)
        repaint (histogramBounds);

    // Label::setText only repaints when the text actually changes
    updateStatusTexts();
}

} // namespace vocalsynth
