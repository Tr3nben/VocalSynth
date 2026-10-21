#include "CapturePanel.h"

#include "ExportFiles.h"
#include "Parameters.h"
#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

namespace
{
// Same palette as the editor (PluginEditor.cpp)
const juce::Colour backgroundColour { 0xff15171c };
const juce::Colour panelColour { 0xff2a2e36 };
const juce::Colour accentColour { 0xff3ddc97 };
const juce::Colour warnColour { 0xffffa53d };
const juce::Colour textColour { 0xffd8dbe0 };
const juce::Colour dimTextColour { 0xff8a9099 };

constexpr int gutter = 6;
constexpr int leftWidth = 440;
constexpr int dragHeight = 50;
constexpr int controlRowHeight = 28;
constexpr int captionWidth = 64;
constexpr int previewHeight = 96;

/** U+2013 EN DASH (the source stays ASCII; UTF-8 bytes). */
juce::String enDash()
{
    return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"));
}

/** " U+00B7 " MIDDLE DOT with spaces. */
juce::String dot()
{
    return juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 "));
}

/** U+00D7 MULTIPLICATION SIGN. */
juce::String times()
{
    return juce::String (juce::CharPointer_UTF8 ("\xc3\x97"));
}

juce::String megabytes (std::size_t bytes)
{
    return juce::String (static_cast<double> (bytes) / (1024.0 * 1024.0), 1) + " MB";
}

void styleLabel (juce::Label& label, float size, juce::Colour colour)
{
    label.setFont (juce::FontOptions (size));
    label.setColour (juce::Label::textColourId, colour);
    label.setJustificationType (juce::Justification::centredLeft);
    label.setMinimumHorizontalScale (0.7f);
    label.setBorderSize ({ 1, 0, 1, 0 });
}

int64_t ticksPerBar (const RenderedTake& r)
{
    const auto& sig = r.timeSignatures.empty() ? TickTimeSignature {} : r.timeSignatures.front();
    const auto beats = 4.0 * static_cast<double> (juce::jmax (1, sig.numerator)) / static_cast<double> (juce::jmax (1, sig.denominator));
    return juce::jmax ((int64_t) 1, (int64_t) std::llround (beats * r.ticksPerQuarterNote));
}

const juce::StringArray versionChoices { "Raw", "Clean" };
const juce::StringArray quantizeChoices { "Off", "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
const juce::StringArray bendChoices { "As plugin", "Off", "On" };
} // namespace

//==============================================================================
CapturePanel::CapturePanel (VocalSynthProcessor& p)
    : vsProcessor (p),
      worker (p.getCaptureWorker()),
      midiDrag ("MIDI", [this] { return exportSelectedMidi(); }),
      wavDrag ("WAV", [this] { return exportSelectedWav(); })
{
    auto& apvts = p.getParameters();

    styleLabel (statusLabel, 14.0f, textColour);
    styleLabel (memoryLabel, 12.0f, dimTextColour);
    styleLabel (hintLabel, 12.0f, dimTextColour);
    styleLabel (autoOffsetLabel, 12.0f, dimTextColour);
    hintLabel.setText ("Takes are not saved with the project " + enDash() + " drag or save what you want to keep.",
                       juce::dontSendNotification);
    statusLabel.setTooltip ("Synced: the transport is playing, takes follow FL's bars. Free: stopped, a pause "
                            "starts a new take");
    memoryLabel.setTooltip ("RAM used by this instance's capture (audio look-back and the listed takes)");

    for (auto* label : { &statusLabel, &memoryLabel, &hintLabel, &autoOffsetLabel })
        addAndMakeVisible (*label);

    takeList.setRowHeight (rowHeight);
    takeList.setColour (juce::ListBox::backgroundColourId, panelColour.withAlpha (0.6f));
    takeList.setColour (juce::ListBox::outlineColourId, panelColour);
    takeList.setOutlineThickness (1);
    takeList.setTooltip ("Newest take on top. Select one for the MIDI / WAV handles");
    addAndMakeVisible (takeList);

    const auto dragTooltip = "Drag into FL Studio " + enDash() + " hold Shift while dropping to skip FL's import dialog";
    midiDrag.setTooltip (dragTooltip + " (MIDI: onto a channel / into the piano roll; place the pattern at the "
                                       "take's bar)");
    wavDrag.setTooltip (dragTooltip + " (WAV: into the playlist; saved in Documents\\VocalSynth\\Takes)");
    addAndMakeVisible (midiDrag);
    addAndMakeVisible (wavDrag);

    saveAsButton.setTooltip ("Save the selected take as .mid or .wav");
    saveAsButton.onClick = [this] { showSaveMenu(); };
    newTakeButton.setTooltip ("End the current take here and start the next one");
    newTakeButton.onClick = [this] { pressNewTake(); };
    grabButton.setTooltip ("Make a take of what you just sang, even if no take was running");
    grabButton.onClick = [this] { pressGrab(); };
    clearButton.setTooltip ("Delete every take and the look-back (asks first)");
    clearButton.onClick = [this] { pressClear(); };

    for (auto* b : { &saveAsButton, &newTakeButton, &grabButton, &clearButton })
        addAndMakeVisible (*b);

    for (const auto bars : TakeSegmenter::grabBarChoices)
        grabBarsBox.addItem (juce::String (bars) + (bars == 1 ? " bar" : " bars"), bars);

    for (const auto seconds : TakeSegmenter::grabSecondChoices)
        grabSecondsBox.addItem (juce::String (seconds) + " s", seconds);

    grabBarsBox.setTooltip ("Grab length while the transport plays (bars)");
    grabSecondsBox.setTooltip ("Grab length while the transport is stopped (seconds)");
    grabBarsBox.onChange = [this] { changeState ([this] (CaptureState& s) { s.grabBars = grabBarsBox.getSelectedId(); }); };
    grabSecondsBox.onChange = [this]
    { changeState ([this] (CaptureState& s) { s.grabSeconds = grabSecondsBox.getSelectedId(); }); };

    versionBox.addItemList (versionChoices, 1);
    versionBox.setTooltip ("Clean: ghost notes, slides and octave jumps removed. Raw: exactly what was sent");
    versionBox.onChange = [this]
    {
        changeState ([this] (CaptureState& s)
                     { s.render.version = static_cast<ExportVersion> (juce::jmax (0, versionBox.getSelectedItemIndex())); });
    };

    bendsBox.addItemList (bendChoices, 1);
    bendsBox.setTooltip ("Pitch bend in the .mid. As plugin: when Bend Mode was on while you sang (FL may assume "
                         "a +-2 semitone range)");
    bendsBox.onChange = [this]
    {
        changeState ([this] (CaptureState& s)
                     { s.render.bends = static_cast<BendExport> (juce::jmax (0, bendsBox.getSelectedItemIndex())); });
    };

    ccButton.setTooltip ("Export controller (CC) data when there is some (test pattern)");
    ccButton.onClick = [this] { changeState ([this] (CaptureState& s) { s.render.controlChanges = ccButton.getToggleState(); }); };

    quantizeBox.addItemList (quantizeChoices, 1);
    quantizeBox.setTooltip ("Moves note starts to the grid (takes recorded with the transport playing only)");
    quantizeBox.onChange = [this]
    {
        changeState ([this] (CaptureState& s)
                     { s.render.quantize = static_cast<QuantizeGrid> (juce::jmax (0, quantizeBox.getSelectedItemIndex())); });
    };

    strengthSlider.setRange (0.0, 100.0, 1.0);
    strengthSlider.setTextValueSuffix (" %");
    strengthSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 56, 20);
    strengthSlider.setTooltip ("Quantize strength: how far note starts move towards the grid");
    strengthSlider.onValueChange = [this]
    { changeState ([this] (CaptureState& s) { s.render.quantizeStrengthPercent = strengthSlider.getValue(); }); };

    quantizeEndsButton.setTooltip ("Quantize note ends too");
    quantizeEndsButton.onClick = [this]
    { changeState ([this] (CaptureState& s) { s.render.quantizeEnds = quantizeEndsButton.getToggleState(); }); };

    offsetModeBox.addItemList (params::captureOffsetModeChoices, 1);
    offsetModeBox.setTooltip ("Removes the sound card delay from the takes. Auto: 2 x the audio buffer (+ Fine). "
                              "Manual: the value below. Off: none");

    for (auto* slider : { &offsetMsSlider, &fineOffsetSlider })
    {
        slider->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 64, 20);
        slider->setTextValueSuffix (" ms");
    }

    offsetMsSlider.setTooltip ("Manual capture offset (sound card in + out delay)");
    fineOffsetSlider.setTooltip ("Added to Auto: move the notes earlier (+) or later (-) if they still sit off the beat");
    autoOffsetLabel.setTooltip ("What Auto removes: 2 x the audio buffer FL prepared VocalSynth with. "
                                "'FL block' = the largest block FL actually sent");

    captureButton.setTooltip ("Always-on capture of what VocalSynth sends");
    audioButton.setTooltip ("Also record the dry voice (WAV handle; 10 minutes look-back, ~115 MB at 48 kHz)");

    gapSlider.setRange (TakeSegmenter::minTakeGapSeconds, TakeSegmenter::maxTakeGapSeconds, 0.5);
    gapSlider.setTextValueSuffix (" s");
    gapSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 56, 20);
    gapSlider.setTooltip ("Transport stopped: a pause this long without a note starts a new take");
    gapSlider.onValueChange = [this] { changeState ([this] (CaptureState& s) { s.takeGapSeconds = gapSlider.getValue(); }); };

    for (auto* label : { &versionLabel, &bendsLabel, &quantizeLabel, &offsetLabel, &manualLabel, &fineLabel, &gapLabel })
    {
        styleLabel (*label, 13.0f, dimTextColour);
        addAndMakeVisible (*label);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &grabBarsBox, &grabSecondsBox, &versionBox, &bendsBox,
                                                             &ccButton, &quantizeBox, &strengthSlider,
                                                             &quantizeEndsButton, &offsetModeBox, &offsetMsSlider,
                                                             &fineOffsetSlider, &captureButton, &audioButton,
                                                             &gapSlider })
        addAndMakeVisible (c);

    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    offsetModeAttachment = std::make_unique<ComboBoxAttachment> (apvts, params::captureOffsetMode.getParamID(), offsetModeBox);
    offsetMsAttachment = std::make_unique<SliderAttachment> (apvts, params::captureOffsetMs.getParamID(), offsetMsSlider);
    fineOffsetAttachment = std::make_unique<SliderAttachment> (apvts, params::captureFineOffsetMs.getParamID(), fineOffsetSlider);
    captureAttachment = std::make_unique<ButtonAttachment> (apvts, params::captureEnabled.getParamID(), captureButton);
    audioAttachment = std::make_unique<ButtonAttachment> (apvts, params::captureAudio.getParamID(), audioButton);

    confirmClear = [this] (std::function<void (bool)> onResult)
    {
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel (
            juce::MessageBoxIconType::QuestionIcon, "Clear all takes?",
            "This deletes every take and the look-back of this VocalSynth instance. WAVs you dragged into FL stay.",
            "Clear", "Cancel", this);
        juce::AlertWindow::showAsync (options, [done = std::move (onResult)] (int result) { done (result == 1); });
    };

    // IDs for the tests and the layout check
    const std::pair<juce::Component*, const char*> ids[] = {
        { &statusLabel, "status" },           { &memoryLabel, "memory" },         { &hintLabel, "hint" },
        { &takeList, "takeList" },            { &midiDrag, "midiDrag" },          { &wavDrag, "wavDrag" },
        { &saveAsButton, "saveAs" },          { &newTakeButton, "newTake" },      { &grabButton, "grab" },
        { &grabBarsBox, "grabBars" },         { &grabSecondsBox, "grabSeconds" }, { &clearButton, "clear" },
        { &versionLabel, "versionLabel" },    { &versionBox, "version" },         { &bendsLabel, "bendsLabel" },
        { &bendsBox, "bends" },               { &ccButton, "cc" },                { &quantizeLabel, "quantizeLabel" },
        { &quantizeBox, "quantize" },         { &strengthSlider, "strength" },    { &quantizeEndsButton, "quantizeEnds" },
        { &offsetLabel, "offsetLabel" },      { &offsetModeBox, "offsetMode" },   { &autoOffsetLabel, "autoOffset" },
        { &manualLabel, "manualLabel" },      { &offsetMsSlider, "offsetMs" },    { &fineLabel, "fineLabel" },
        { &fineOffsetSlider, "fineOffset" },  { &captureButton, "capture" },      { &audioButton, "captureAudio" },
        { &gapLabel, "gapLabel" },            { &gapSlider, "gap" },
    };

    for (const auto& [component, id] : ids)
        component->setComponentID (id);

    refreshOptionControls();
    update();
}

CapturePanel::~CapturePanel()
{
    stopTimer();
    takeList.setModel (nullptr);
}

void CapturePanel::visibilityChanged()
{
    // Only while shown: a hidden Capture tab costs nothing
    if (isVisible())
    {
        update();
        startTimerHz (timerHz);
    }
    else
    {
        stopTimer();
    }
}

//==============================================================================
juce::String CapturePanel::formatStatus (const CaptureStatus& s, bool captureEnabled, int openTakeNotes)
{
    juce::String text;

    if (! captureEnabled)
        text = "Capture off";
    else if (! s.started || s.state == TakeSegmenter::State::off)
        text = "Capture on " + enDash() + " waiting for audio";
    else if (s.state == TakeSegmenter::State::synced)
    {
        text = "Synced " + juce::String (s.bpm > 0.0 ? s.bpm : TakeTimeline::fallbackBpm, 1) + " BPM "
               + juce::String (s.timeSigNumerator) + "/" + juce::String (s.timeSigDenominator);

        if (s.recordingTakeNumber > 0)
        {
            text << " " << enDash() << " recording take " << s.recordingTakeNumber;

            if (s.recordingBar > 0)
                text << " (bar " << (juce::int64) s.recordingBar << ")";
        }
    }
    else if (s.state == TakeSegmenter::State::freeRecording && s.recordingTakeNumber > 0)
        text = "Free " + enDash() + " recording take " + juce::String (s.recordingTakeNumber);
    else
        text = "Free " + enDash() + " waiting for a note";

    if (captureEnabled && s.recordingTakeNumber > 0 && openTakeNotes > 0)
        text << dot() << openTakeNotes << (openTakeNotes == 1 ? " note" : " notes");

    if (s.overflow.records > 0 || s.overflow.audioSamples > 0)
        text << dot() << "overflow: " << (juce::int64) s.overflow.records << " records, "
             << (juce::int64) s.overflow.audioSamples << " audio samples";

    return text;
}

juce::String CapturePanel::formatMemory (const CaptureStatus& s)
{
    return "Audio history " + megabytes (s.audioHistoryBytes) + dot() + "takes " + megabytes (s.takeBytes) + " ("
           + juce::String (s.numTakes) + ")";
}

juce::String CapturePanel::describeTake (const Take& take, const RenderedTake& rendered)
{
    auto text = "Take " + juce::String (take.number);

    if (take.mode == TakeMode::synced)
    {
        const auto bars = juce::jmax ((int64_t) 1, (rendered.endTick + ticksPerBar (rendered) - 1) / ticksPerBar (rendered));
        text << dot() << "Bar " << (juce::int64) rendered.originBar << dot() << (juce::int64) bars
             << (bars == 1 ? " bar" : " bars");
    }
    else
    {
        const auto rate = take.sampleRate > 0.0 ? take.sampleRate : 48000.0;
        const auto seconds = static_cast<double> (take.endSample - take.startSample) / rate;
        const auto start = take.wallClockStartMs > 0 ? juce::Time (take.wallClockStartMs).formatted ("%H:%M:%S")
                                                     : juce::String ("free");
        text << dot() << start << dot() << juce::String (seconds, 1) << " s";
    }

    text << dot() << rendered.noteCount << (rendered.noteCount == 1 ? " note" : " notes");

    if (take.kind == TakeKind::grab)
        text << dot() << "Grab";

    if (take.incomplete)
        text << dot() << "incomplete";

    if (take.audio == TakeAudio::truncated)
        text << dot() << "audio truncated";

    return text;
}

juce::String CapturePanel::grabButtonText (bool synced, int bars, int seconds)
{
    if (synced)
        return "Grab last " + juce::String (bars) + (bars == 1 ? " bar" : " bars");

    return "Grab last " + juce::String (seconds) + " s";
}

juce::String CapturePanel::getStatusText() const
{
    return statusLabel.getText();
}

juce::String CapturePanel::getMemoryText() const
{
    return memoryLabel.getText();
}

juce::String CapturePanel::getAutoOffsetText() const
{
    return autoOffsetLabel.getText();
}

juce::String CapturePanel::getHintText() const
{
    return hintLabel.getText();
}

juce::String CapturePanel::getRowText (int row)
{
    if (row < 0 || row >= getNumTakeRows())
        return {};

    const auto& take = *shownTakes[static_cast<std::size_t> (row)];
    return describeTake (take, renderedFor (take));
}

//==============================================================================
void CapturePanel::update()
{
    const auto status = worker.getStatus();
    const auto open = worker.getOpenTake();
    const auto state = vsProcessor.getCaptureState();

    statusLabel.setText (formatStatus (status, vsProcessor.isCaptureEnabled(), open != nullptr ? open->noteCount : 0),
                         juce::dontSendNotification); // repaints only on a change
    statusLabel.setColour (juce::Label::textColourId,
                           status.overflow.records > 0 || status.overflow.audioSamples > 0 ? warnColour : textColour);
    memoryLabel.setText (formatMemory (status), juce::dontSendNotification);

    // Auto value of the capture offset, and the block FL actually sends
    const auto autoMs = vsProcessor.getAutoCaptureOffsetMs();
    const auto prepared = vsProcessor.getPreparedMaxBlockSize();
    const auto seen = vsProcessor.getRecentMaxBlockSize();
    juce::String autoText;

    if (prepared > 0)
    {
        autoText << "Auto = " << juce::String (autoMs, 1) << " ms (2 " << times() << " " << prepared << " smp)";

        if (seen > 0)
            autoText << dot() << "FL block " << seen;

        if (seen > 0 && seen * 2 <= prepared)
            autoText << dot() << "Manual may fit better";
    }
    else
    {
        autoText = "Auto: - (no audio yet)";
    }

    autoOffsetLabel.setText (autoText, juce::dontSendNotification);
    autoOffsetLabel.setColour (juce::Label::textColourId, seen > 0 && seen * 2 <= prepared ? warnColour : dimTextColour);

    if (! (state == shownState))
        refreshOptionControls();

    const auto synced = status.state == TakeSegmenter::State::synced;

    if (synced != shownSynced)
    {
        shownSynced = synced;
        updateGrabButton();
    }

    // Take list: only when the worker published something
    const auto version = worker.getTakeListVersion();
    bool contentChanged = false;

    if (! hasShownVersion || version != shownVersion)
    {
        hasShownVersion = true;
        shownVersion = version;

        const auto previousTop = shownTakes.empty() ? 0 : shownTakes.front()->number;
        auto takes = worker.getTakes();
        const auto listChanged = takes != shownTakes;
        shownTakes = std::move (takes);

        // Drop renders of takes that are gone
        std::erase_if (renderCache,
                       [this] (const CachedRender& c)
                       {
                           return std::none_of (shownTakes.begin(), shownTakes.end(),
                                                [&c] (const auto& t) { return t.get() == c.take; });
                       });

        // Keep the selection; a new take on top is selected if the previous top one was (or nothing)
        const auto newTop = shownTakes.empty() ? 0 : shownTakes.front()->number;
        const auto selectionGone = getSelectedTake() == nullptr;

        if (newTop != previousTop && (selectedNumber == previousTop || selectedNumber == 0))
            selectedNumber = newTop;
        else if (selectionGone)
            selectedNumber = newTop;

        if (listChanged)
        {
            takeList.updateContent();
            contentChanged = true;
        }

        for (int row = 0; row < getNumTakeRows(); ++row)
            if (shownTakes[static_cast<std::size_t> (row)]->number == selectedNumber)
                takeList.selectRow (row, false, true);

        if (selectedNumber == 0)
            takeList.deselectAllRows();

        updateDragHandles();
    }

    // The selected take's rendering follows the export options and the offset parameters
    if (const auto selected = getSelectedTake())
    {
        const auto options = vsProcessor.getRenderOptionsFor (*selected);

        if (! previewOptions.has_value() || ! sameRenderOptions (*previewOptions, options))
        {
            previewOptions = options;
            contentChanged = true;
            takeList.repaint(); // row texts (note counts) follow the version
        }
    }

    if (contentChanged)
    {
        ++contentRepaints;
        repaint (previewBounds);
    }
}

void CapturePanel::refreshOptionControls()
{
    const auto s = vsProcessor.getCaptureState();
    shownState = s;

    versionBox.setSelectedItemIndex (static_cast<int> (s.render.version), juce::dontSendNotification);
    bendsBox.setSelectedItemIndex (static_cast<int> (s.render.bends), juce::dontSendNotification);
    ccButton.setToggleState (s.render.controlChanges, juce::dontSendNotification);
    quantizeBox.setSelectedItemIndex (static_cast<int> (s.render.quantize), juce::dontSendNotification);
    strengthSlider.setValue (s.render.quantizeStrengthPercent, juce::dontSendNotification);
    quantizeEndsButton.setToggleState (s.render.quantizeEnds, juce::dontSendNotification);
    grabBarsBox.setSelectedId (s.grabBars, juce::dontSendNotification);
    grabSecondsBox.setSelectedId (s.grabSeconds, juce::dontSendNotification);
    gapSlider.setValue (s.takeGapSeconds, juce::dontSendNotification);

    const auto quantizeOn = s.render.quantize != QuantizeGrid::off;
    strengthSlider.setEnabled (quantizeOn);
    quantizeEndsButton.setEnabled (quantizeOn);
    updateGrabButton();
}

void CapturePanel::changeState (const std::function<void (CaptureState&)>& change)
{
    auto s = vsProcessor.getCaptureState();
    change (s);
    vsProcessor.setCaptureState (s);
    refreshOptionControls(); // shows the stored (clamped) values
    update();
}

void CapturePanel::updateGrabButton()
{
    grabButton.setButtonText (grabButtonText (shownSynced, shownState.grabBars, shownState.grabSeconds));
}

void CapturePanel::updateDragHandles()
{
    const auto take = getSelectedTake();

    if (take == nullptr)
    {
        midiDrag.setAvailable (false, shownTakes.empty() ? "no take yet" : "select a take");
        wavDrag.setAvailable (false, shownTakes.empty() ? "no take yet" : "select a take");
        saveAsButton.setEnabled (false);
        return;
    }

    saveAsButton.setEnabled (true);
    midiDrag.setAvailable (true);
    midiDrag.setIdleText ("Take " + juce::String (take->number) + " into a pattern");

    const auto wav = worker.getWavState (take->number);

    switch (wav.kind)
    {
        case TakeWavState::Kind::unavailable:
            wavDrag.setAvailable (false, take->audio == TakeAudio::none ? "no audio for this take" : "WAV failed");
            break;
        case TakeWavState::Kind::preparing:
            wavDrag.setAvailable (false, "preparing...");
            break;
        case TakeWavState::Kind::ready:
            wavDrag.setAvailable (true);
            wavDrag.setIdleText ("Take " + juce::String (take->number) + " into the playlist");
            break;
        case TakeWavState::Kind::persisted:
            wavDrag.setAvailable (true);
            wavDrag.setIdleText ("saved in Takes folder");
            break;
    }
}

std::shared_ptr<const Take> CapturePanel::getSelectedTake() const
{
    for (const auto& t : shownTakes)
        if (t->number == selectedNumber)
            return t;

    return nullptr;
}

void CapturePanel::selectTake (int takeNumber)
{
    for (int row = 0; row < getNumTakeRows(); ++row)
        if (shownTakes[static_cast<std::size_t> (row)]->number == takeNumber)
        {
            takeList.selectRow (row);
            selectedRowsChanged (row);
            return;
        }
}

const RenderedTake& CapturePanel::renderedFor (const Take& take)
{
    const auto options = vsProcessor.getRenderOptionsFor (take);

    for (auto& c : renderCache)
        if (c.take == &take && c.number == take.number)
        {
            if (! sameRenderOptions (c.options, options))
            {
                c.options = options;
                c.rendered = TakeRenderer::render (take, options);
            }

            return c.rendered;
        }

    renderCache.push_back ({ take.number, &take, options, TakeRenderer::render (take, options) });
    return renderCache.back().rendered;
}

//==============================================================================
void CapturePanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= getNumTakeRows())
        return;

    const auto& take = *shownTakes[static_cast<std::size_t> (row)];

    if (selected)
    {
        g.setColour (accentColour.withAlpha (0.25f));
        g.fillRect (0, 0, width, height);
    }

    // Small mode marker: synced = bar grid, free = clock
    g.setColour (take.mode == TakeMode::synced ? accentColour : dimTextColour);
    g.fillRoundedRectangle (6.0f, (float) height * 0.5f - 4.0f, 8.0f, 8.0f, 2.0f);

    g.setColour (take.incomplete ? warnColour : textColour);
    g.setFont (juce::FontOptions (13.0f));
    g.drawFittedText (describeTake (take, renderedFor (take)), juce::Rectangle<int> (20, 0, width - 24, height),
                      juce::Justification::centredLeft, 1, 0.8f);
}

void CapturePanel::selectedRowsChanged (int lastRowSelected)
{
    const auto number = lastRowSelected >= 0 && lastRowSelected < getNumTakeRows()
                            ? shownTakes[static_cast<std::size_t> (lastRowSelected)]->number
                            : 0;

    if (number == selectedNumber)
        return;

    selectedNumber = number;
    previewOptions.reset();
    updateDragHandles();
    ++contentRepaints;
    repaint (previewBounds);
}

//==============================================================================
juce::File CapturePanel::exportSelectedMidi()
{
    const auto take = getSelectedTake();

    if (take == nullptr)
        return {};

    return CaptureExporter::writeMidi (*take, vsProcessor.getRenderOptionsFor (*take),
                                       vsProcessor.getCaptureState().smfFormat, worker.getInstanceFolder());
}

juce::File CapturePanel::exportSelectedWav()
{
    const auto take = getSelectedTake();

    if (take == nullptr)
        return {};

    const auto file = worker.persistWav (take->number, vsProcessor.getTakesFolder());
    updateDragHandles();
    return file;
}

void CapturePanel::pressNewTake()
{
    vsProcessor.getCaptureTap().requestNewTake();
}

void CapturePanel::pressGrab()
{
    const auto s = vsProcessor.getCaptureState();
    vsProcessor.getCaptureTap().requestGrab (s.grabBars, static_cast<double> (s.grabSeconds));
}

void CapturePanel::pressClear()
{
    if (confirmClear == nullptr)
        return;

    juce::Component::SafePointer<CapturePanel> safeThis (this);
    confirmClear ([safeThis] (bool clear)
                  {
                      if (clear && safeThis != nullptr)
                          safeThis->vsProcessor.getCaptureTap().requestClear();
                  });
}

void CapturePanel::showSaveMenu()
{
    const auto take = getSelectedTake();

    if (take == nullptr)
        return;

    const auto wav = worker.getWavState (take->number);
    const auto wavReady = wav.kind == TakeWavState::Kind::ready || wav.kind == TakeWavState::Kind::persisted;

    juce::PopupMenu menu;
    menu.addItem (1, "Save MIDI as...");
    menu.addItem (2, "Save WAV as...", wavReady);

    juce::Component::SafePointer<CapturePanel> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&saveAsButton),
                        [safeThis] (int result)
                        {
                            if (safeThis == nullptr)
                                return;

                            if (result == 1)
                                safeThis->saveMidiAs();
                            else if (result == 2)
                                safeThis->saveWavAs();
                        });
}

void CapturePanel::saveMidiAs()
{
    const auto take = getSelectedTake();

    if (take == nullptr)
        return;

    const auto options = vsProcessor.getRenderOptionsFor (*take);
    const auto rendered = TakeRenderer::render (*take, options);
    const auto suggested = vsProcessor.getTakesFolder().getChildFile (CaptureExporter::makeMidiFileName (*take, rendered, options));

    fileChooser = std::make_unique<juce::FileChooser> ("Save the take as MIDI", suggested, "*.mid");
    juce::Component::SafePointer<CapturePanel> safeThis (this);

    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safeThis, take] (const juce::FileChooser& chooser)
                              {
                                  if (safeThis == nullptr)
                                      return;

                                  const auto target = chooser.getResult().withFileExtension ("mid");

                                  if (target == juce::File())
                                      return;

                                  const auto renderOptions = safeThis->vsProcessor.getRenderOptionsFor (*take);
                                  SmfOptions smf;
                                  smf.format = safeThis->vsProcessor.getCaptureState().smfFormat;
                                  smf.trackName = ("VocalSynth Take " + juce::String (take->number)).toStdString();
                                  const auto bytes = SmfWriter::write (TakeRenderer::render (*take, renderOptions), smf);
                                  ExportFiles::writeBytesAtomically (target, bytes.data(), bytes.size());
                              });
}

void CapturePanel::saveWavAs()
{
    const auto take = getSelectedTake();

    if (take == nullptr)
        return;

    const auto wav = worker.getWavState (take->number);

    if (wav.kind != TakeWavState::Kind::ready && wav.kind != TakeWavState::Kind::persisted)
        return;

    fileChooser = std::make_unique<juce::FileChooser> ("Save the take's voice as WAV",
                                                       vsProcessor.getTakesFolder().getChildFile (wav.file.getFileName()),
                                                       "*.wav");
    juce::Component::SafePointer<CapturePanel> safeThis (this);
    const auto number = take->number;

    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safeThis, number] (const juce::FileChooser& chooser)
                              {
                                  if (safeThis == nullptr)
                                      return;

                                  const auto target = chooser.getResult().withFileExtension ("wav");

                                  if (target == juce::File())
                                      return;

                                  // A copy: the cached / persisted WAV keeps serving drags
                                  const auto state = safeThis->worker.getWavState (number);

                                  if (state.file.existsAsFile() && state.file != target)
                                      ExportFiles::writeAtomically (target, [&state] (const juce::File& f)
                                                                    { return state.file.copyFileTo (f); });
                              });
}

//==============================================================================
void CapturePanel::paint (juce::Graphics& g)
{
    paintPreview (g);
}

void CapturePanel::paintPreview (juce::Graphics& g)
{
    if (previewBounds.isEmpty())
        return;

    g.setColour (panelColour.withAlpha (0.6f));
    g.fillRoundedRectangle (previewBounds.toFloat(), 4.0f);

    const auto take = getSelectedTake();

    if (take == nullptr)
    {
        g.setColour (dimTextColour);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (shownTakes.empty() ? "Sing something: takes appear here (Synced with the transport playing, "
                                               "Free when stopped)"
                                             : "Select a take",
                          previewBounds.reduced (10), juce::Justification::centred, 2);
        return;
    }

    const auto& rendered = renderedFor (*take);
    auto area = previewBounds.reduced (6, 4);
    const auto hasAudio = ! take->overview.empty();
    auto waveArea = hasAudio ? area.removeFromBottom (18) : juce::Rectangle<int>();
    auto rollArea = area.toFloat();

    // Time axis: seconds, so MIDI ticks (beats) and audio (samples) line up
    const auto bpm = rendered.tempos.empty() ? TakeTimeline::fallbackBpm : rendered.tempos.front().bpm;
    const auto secondsPerTick = 60.0 / (juce::jmax (1.0, bpm) * static_cast<double> (rendered.ticksPerQuarterNote));
    const auto rate = take->sampleRate > 0.0 ? take->sampleRate : 48000.0;
    const auto midiSeconds = static_cast<double> (juce::jmax ((int64_t) 1, rendered.endTick)) * secondsPerTick;
    const auto audioSeconds = static_cast<double> (rendered.audio.numSamples) / rate;
    const auto totalSeconds = juce::jmax (midiSeconds, hasAudio ? audioSeconds : 0.0, 1.0e-3);
    const auto xOfSeconds = [&] (double s) { return rollArea.getX() + rollArea.getWidth() * (float) (s / totalSeconds); };

    // Bar lines (synced)
    if (take->mode == TakeMode::synced)
    {
        const auto barTicks = ticksPerBar (rendered);
        g.setColour (dimTextColour.withAlpha (0.35f));

        for (int64_t t = 0; t <= rendered.endTick; t += barTicks)
        {
            const auto x = xOfSeconds (static_cast<double> (t) * secondsPerTick);
            g.drawVerticalLine (juce::roundToInt (x), rollArea.getY(), rollArea.getBottom());
        }
    }

    // Notes: pair note-ons with their note-offs; auto pitch range
    struct Note
    {
        int64_t on = 0, off = 0;
        int pitch = 0;
    };

    std::vector<Note> notes;
    std::array<std::array<int64_t, 128>, 16> openAt {};

    for (auto& ch : openAt)
        ch.fill (-1);

    for (const auto& e : rendered.events)
    {
        auto& slot = openAt[static_cast<std::size_t> (juce::jlimit (1, 16, (int) e.channel) - 1)][e.data1 & 0x7f];

        if (e.type == TickEventType::noteOn)
            slot = e.tick;
        else if (e.type == TickEventType::noteOff && slot >= 0)
        {
            notes.push_back ({ slot, e.tick, e.data1 & 0x7f });
            slot = -1;
        }
    }

    if (notes.empty())
    {
        g.setColour (dimTextColour);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("no notes in this version", rollArea, juce::Justification::centred);
    }
    else
    {
        auto lo = 127, hi = 0;

        for (const auto& n : notes)
        {
            lo = juce::jmin (lo, n.pitch);
            hi = juce::jmax (hi, n.pitch);
        }

        lo = juce::jmax (0, lo - 2);
        hi = juce::jmin (127, hi + 2);
        const auto rowH = rollArea.getHeight() / (float) (hi - lo + 1);

        g.setColour (accentColour);

        for (const auto& n : notes)
        {
            const auto x0 = xOfSeconds (static_cast<double> (n.on) * secondsPerTick);
            const auto x1 = xOfSeconds (static_cast<double> (n.off) * secondsPerTick);
            const auto y = rollArea.getBottom() - (float) (n.pitch - lo + 1) * rowH;
            g.fillRect (juce::Rectangle<float> (x0, y, juce::jmax (1.5f, x1 - x0), juce::jmax (1.5f, rowH - 0.5f)));
        }
    }

    // Waveform strip (min / max per Take::overviewSamplesPerPoint)
    if (hasAudio)
    {
        const auto points = take->overview.size() / 2;
        const auto mid = (float) waveArea.getCentreY();
        const auto half = (float) waveArea.getHeight() * 0.5f;
        const auto pointSeconds = static_cast<double> (Take::overviewSamplesPerPoint) / rate;
        g.setColour (dimTextColour.withAlpha (0.8f));

        // One line per pixel column (max of the points that fall into it)
        const auto width = juce::jmax (1, waveArea.getWidth());

        for (int px = 0; px < width; ++px)
        {
            const auto s0 = (double) px / (double) width * totalSeconds;
            const auto s1 = (double) (px + 1) / (double) width * totalSeconds;
            auto p0 = static_cast<std::size_t> (s0 / pointSeconds);
            const auto p1 = juce::jmin (points, static_cast<std::size_t> (std::ceil (s1 / pointSeconds)));

            if (p0 >= points)
                break;

            float mn = 0.0f, mx = 0.0f;

            for (; p0 < p1; ++p0)
            {
                mn = juce::jmin (mn, take->overview[2 * p0]);
                mx = juce::jmax (mx, take->overview[2 * p0 + 1]);
            }

            const auto x = (float) (waveArea.getX() + px);
            g.drawVerticalLine ((int) x, mid - juce::jlimit (0.0f, 1.0f, mx) * half - 0.5f,
                                mid - juce::jlimit (-1.0f, 0.0f, mn) * half + 0.5f);
        }
    }

    // Caption
    g.setColour (textColour);
    g.setFont (juce::FontOptions (12.0f));
    const auto versionName = previewOptions.has_value() && previewOptions->version == ExportVersion::raw ? "Raw" : "Clean";
    g.drawText ("Take " + juce::String (take->number) + " " + versionName, previewBounds.reduced (8, 4),
                juce::Justification::topRight);
}

void CapturePanel::resized()
{
    auto area = getLocalBounds();
    auto left = area.removeFromLeft (leftWidth);
    area.removeFromLeft (16);
    auto right = area;

    // Left column
    statusLabel.setBounds (left.removeFromTop (20));
    memoryLabel.setBounds (left.removeFromTop (18));
    left.removeFromTop (4);
    hintLabel.setBounds (left.removeFromBottom (20));
    left.removeFromBottom (4);
    previewBounds = left.removeFromBottom (previewHeight);
    left.removeFromBottom (6);
    takeList.setBounds (left);

    // Right column: drag handles + Save as
    auto drags = right.removeFromTop (dragHeight);
    const auto dragWidth = (drags.getWidth() - 2 * gutter) * 3 / 8;
    midiDrag.setBounds (drags.removeFromLeft (dragWidth));
    drags.removeFromLeft (gutter);
    wavDrag.setBounds (drags.removeFromLeft (dragWidth));
    drags.removeFromLeft (gutter);
    saveAsButton.setBounds (drags.reduced (0, 10));
    right.removeFromTop (8);

    // Actions
    {
        auto row = right.removeFromTop (controlRowHeight).reduced (0, 2);
        const auto unit = (row.getWidth() - 4 * gutter) / 10;
        newTakeButton.setBounds (row.removeFromLeft (2 * unit));
        row.removeFromLeft (gutter);
        grabButton.setBounds (row.removeFromLeft (3 * unit));
        row.removeFromLeft (gutter);
        grabBarsBox.setBounds (row.removeFromLeft (2 * unit - unit / 4));
        row.removeFromLeft (gutter);
        grabSecondsBox.setBounds (row.removeFromLeft (unit + unit / 2));
        row.removeFromLeft (gutter);
        clearButton.setBounds (row);
    }

    right.removeFromTop (gutter);

    const auto captionRow = [&right] (juce::Label& caption)
    {
        auto row = right.removeFromTop (controlRowHeight);
        caption.setBounds (row.removeFromLeft (captionWidth));
        return row.reduced (0, 3);
    };

    {
        auto row = captionRow (versionLabel);
        versionBox.setBounds (row.removeFromLeft (96));
        row.removeFromLeft (12);
        bendsLabel.setBounds (row.removeFromLeft (48));
        bendsBox.setBounds (row.removeFromLeft (110));
        row.removeFromLeft (12);
        ccButton.setBounds (row.removeFromLeft (60));
    }

    {
        auto row = captionRow (quantizeLabel);
        quantizeBox.setBounds (row.removeFromLeft (96));
        row.removeFromLeft (8);
        quantizeEndsButton.setBounds (row.removeFromRight (64));
        row.removeFromRight (4);
        strengthSlider.setBounds (row);
    }

    {
        auto row = captionRow (offsetLabel);
        offsetModeBox.setBounds (row.removeFromLeft (96));
        row.removeFromLeft (8);
        autoOffsetLabel.setBounds (row);
    }

    offsetMsSlider.setBounds (captionRow (manualLabel));
    fineOffsetSlider.setBounds (captionRow (fineLabel));

    {
        auto row = right.removeFromTop (controlRowHeight).reduced (0, 3);
        captureButton.setBounds (row.removeFromLeft (90));
        audioButton.setBounds (row.removeFromLeft (124));
        row.removeFromLeft (4);
        gapLabel.setBounds (row.removeFromLeft (captionWidth - 4));
        gapSlider.setBounds (row);
    }
}

} // namespace vocalsynth
