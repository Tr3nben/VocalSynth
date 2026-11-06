#include "ui/WavetableView.h"

#include "ui/Theme.h"

#include <cmath>

namespace vocalsynth::ui
{

WavetableView::WavetableView()
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
}

WavetableView::~WavetableView() = default;

void WavetableView::setTable (std::shared_ptr<const synth::Wavetable> newTable)
{
    if (newTable == table)
        return;

    table = std::move (newTable);
    repaint();
}

void WavetableView::setPosition (float newPosition)
{
    newPosition = juce::jlimit (0.0f, 1.0f, newPosition);

    if (juce::exactlyEqual (newPosition, position))
        return;

    const auto oldFrame = getShownFrame();
    const bool pathChanges = std::abs (newPosition - cachedPosition) > positionTolerance;
    position = newPosition;

    // The marker moves with every change; the path only beyond the tolerance (or on a new frame number)
    if (pathChanges || oldFrame != getShownFrame())
        repaint();
    else
        repaint (getLocalBounds().removeFromBottom (11));
}

void WavetableView::setMessage (const juce::String& newMessage, bool isError)
{
    if (newMessage == message && isError == messageIsError)
        return;

    message = newMessage;
    messageIsError = isError;
    repaint();
}

void WavetableView::setShowsName (bool shouldShowName)
{
    showsName = shouldShowName;
    repaint();
}

int WavetableView::getShownFrame() const noexcept
{
    if (table == nullptr)
        return -1;

    return juce::roundToInt (position * static_cast<float> (table->getNumFrames() - 1));
}

void WavetableView::resized()
{
    cacheValid = false;
}

juce::Rectangle<float> WavetableView::getWaveArea() const
{
    return getLocalBounds().toFloat().reduced (6.0f, 0.0f).withTrimmedTop (16.0f).withTrimmedBottom (10.0f);
}

void WavetableView::rebuildPathIfNeeded()
{
    const auto area = getWaveArea();

    if (cacheValid && cachedTable == table.get() && cachedArea == area
        && std::abs (position - cachedPosition) <= positionTolerance)
        return;

    cacheValid = true;
    cachedTable = table.get();
    cachedArea = area;
    cachedPosition = position;
    ++pathRebuilds;
    strokePath.clear();
    fillPath.clear();

    const auto columns = juce::jmax (2, juce::roundToInt (area.getWidth()));
    const auto mid = area.getCentreY();
    const auto amp = area.getHeight() * 0.5f * 0.92f;

    if (table == nullptr || area.isEmpty())
    {
        strokePath.startNewSubPath (area.getX(), mid);
        strokePath.lineTo (area.getRight(), mid);
        return;
    }

    // Frame position like the oscillator: pos * (frames - 1), linear between the two neighbour frames
    const auto framePos = static_cast<double> (position) * (table->getNumFrames() - 1);
    const auto f0 = juce::jlimit (0, table->getNumFrames() - 1, static_cast<int> (std::floor (framePos)));
    const auto f1 = juce::jmin (f0 + 1, table->getNumFrames() - 1);
    const auto t = static_cast<float> (framePos - f0);
    const auto* a = table->getLevel (f0, 0);
    const auto* b = table->getLevel (f1, 0);
    constexpr int n = synth::Wavetable::mipLength (0);

    fillPath.startNewSubPath (area.getX(), mid);
    bool first = true;

    for (int c = 0; c < columns; ++c)
    {
        const auto i0 = static_cast<int> (static_cast<int64_t> (c) * n / columns);
        const auto i1 = juce::jmax (i0 + 1, static_cast<int> (static_cast<int64_t> (c + 1) * n / columns));
        auto lo = 1.0e9f, hi = -1.0e9f, firstValue = 0.0f, last = 0.0f;

        for (int i = i0; i < i1; ++i)
        {
            const auto v = a[i] + t * (b[i] - a[i]);
            lo = juce::jmin (lo, v);
            hi = juce::jmax (hi, v);

            if (i == i0)
                firstValue = v;

            last = v;
        }

        const auto x = area.getX() + area.getWidth() * (static_cast<float> (c) + 0.5f) / static_cast<float> (columns);
        const auto toY = [&] (float v) { return mid - juce::jlimit (-1.1f, 1.1f, v) * amp; };
        const float order[2] = { firstValue <= last ? lo : hi, firstValue <= last ? hi : lo };

        for (int k = 0; k < (hi - lo > 0.02f ? 2 : 1); ++k)
        {
            const auto y = k == 0 && hi - lo <= 0.02f ? toY (0.5f * (lo + hi)) : toY (order[k]);

            if (first)
                strokePath.startNewSubPath (x, y);
            else
                strokePath.lineTo (x, y);

            first = false;

            fillPath.lineTo (x, y);
        }
    }

    fillPath.lineTo (area.getRight(), mid);
    fillPath.closeSubPath();
}

void WavetableView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (theme::meterBackground);
    g.fillRoundedRectangle (bounds, theme::controlCorner);
    g.setColour (theme::panelOutline);
    g.drawRoundedRectangle (bounds, theme::controlCorner, 1.0f);

    rebuildPathIfNeeded();
    const auto area = getWaveArea();

    // Grid: centre line and quarter marks
    g.setColour (theme::panelOutline);
    g.fillRect (juce::Rectangle<float> (area.getX(), area.getCentreY() - 0.5f, area.getWidth(), 1.0f));

    for (int q = 1; q < 4; ++q)
        g.fillRect (juce::Rectangle<float> (area.getX() + area.getWidth() * static_cast<float> (q) * 0.25f - 0.5f,
                                            area.getY(), 1.0f, area.getHeight()));

    const auto active = isEnabled() && table != nullptr;
    const auto waveColour = active ? theme::accent : theme::textDim;

    if (! fillPath.isEmpty())
    {
        juce::ColourGradient fill (waveColour.withAlpha (0.30f), 0.0f, area.getY(), waveColour.withAlpha (0.30f), 0.0f,
                                   area.getBottom(), false);
        fill.addColour (0.5, waveColour.withAlpha (0.05f));
        g.setGradientFill (fill);
        g.fillPath (fillPath);
    }

    g.setColour (waveColour);
    g.strokePath (strokePath, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Texts
    auto top = getLocalBounds().reduced (7, 2).removeFromTop (14);
    g.setFont (theme::font (theme::fontSmall));

    if (table != nullptr)
    {
        const auto frames = "Frame " + juce::String (getShownFrame() + 1) + " / " + juce::String (table->getNumFrames());
        g.setColour (theme::textDim);
        g.drawText (frames, top, juce::Justification::centredRight, false);
        top.removeFromRight (juce::GlyphArrangement::getStringWidthInt (theme::font (theme::fontSmall), frames) + 8);
        g.setColour (theme::text.withAlpha (0.85f));

        if (showsName)
            g.drawFittedText (juce::String::fromUTF8 (table->getName().c_str()), top, juce::Justification::centredLeft, 1, 0.8f);
    }

    const auto shownMessage = message.isNotEmpty() ? message : (table == nullptr ? juce::String ("Loading...") : juce::String());

    if (shownMessage.isNotEmpty())
    {
        const auto textArea = area.toNearestInt().withTrimmedTop (area.toNearestInt().getHeight() - 14);
        g.setColour (theme::meterBackground.withAlpha (0.85f));
        g.fillRect (textArea.toFloat());
        g.setColour (messageIsError ? theme::warning : theme::textDim);
        g.drawFittedText (shownMessage, textArea, juce::Justification::centredLeft, 1, 0.7f);
    }

    // Position bar with marker
    const auto bar = juce::Rectangle<float> (area.getX(), bounds.getBottom() - 6.0f, area.getWidth(), 2.0f);
    g.setColour (theme::knobTrack);
    g.fillRoundedRectangle (bar, 1.0f);
    g.setColour (waveColour);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * position), 1.0f);
    const auto markerX = bar.getX() + bar.getWidth() * position;
    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ markerX, bar.getCentreY() }));
}

} // namespace vocalsynth::ui
