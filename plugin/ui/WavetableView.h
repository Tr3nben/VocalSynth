#pragma once

#include "vocalsynth/synth/Wavetable.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace vocalsynth::ui
{

/** 2D view of one wavetable frame (docs/SYNTH-DESIGN.md 20.4): draws mip level 0 of the frame at the current
    position (interpolated like the oscillator, decimated to the view width) as a path, plus a position bar
    with a marker. The path is cached and rebuilt only when the table or the position changes by more than
    0.5 %; repaint at most 30 Hz (owner's timer). No table: a flat line and "Loading...". Message thread;
    the table is shared (std::shared_ptr from WavetableStore::getSlot), never the audio thread's pointer.
    Owner: agent E.

    Details (agent E): decimation = min / max of the level-0 samples per pixel column (sharp edges and noisy
    tables stay visible); the table name top left, "frame / frames" top right; setMessage shows a status
    ("Loading...", worker errors in theme::warning) at the bottom. */
class WavetableView : public juce::Component
{
public:
    WavetableView();
    ~WavetableView() override;

    void setTable (std::shared_ptr<const synth::Wavetable> table);
    void setPosition (float position0to1);

    float getPosition() const noexcept { return position; }
    const synth::Wavetable* getTable() const noexcept { return table.get(); }

    /** Status line at the bottom (empty = none); isError draws it in the warning colour. */
    void setMessage (const juce::String& message, bool isError);
    const juce::String& getMessage() const noexcept { return message; }

    /** Draws the table name top left (default on; off where a table button already shows it). */
    void setShowsName (bool shouldShowName);

    /** The frame index the view shows (0-based, nearest), -1 without a table. */
    int getShownFrame() const noexcept;

    /** How often the cached path was rebuilt (tests: caching). */
    int getPathRebuildCount() const noexcept { return pathRebuilds; }

    /** Position changes smaller than this (fraction of the range) keep the cached path. */
    static constexpr float positionTolerance = 0.005f;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    juce::Rectangle<float> getWaveArea() const;
    void rebuildPathIfNeeded();

    std::shared_ptr<const synth::Wavetable> table;
    float position = 0.0f;
    juce::String message;
    bool messageIsError = false;
    bool showsName = true;

    // Cache
    juce::Path strokePath, fillPath;
    const synth::Wavetable* cachedTable = nullptr;
    float cachedPosition = -1.0f;
    juce::Rectangle<float> cachedArea;
    bool cacheValid = false;
    int pathRebuilds = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavetableView)
};

} // namespace vocalsynth::ui
