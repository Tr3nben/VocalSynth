#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vocalsynth::ui
{

/** Design tokens of the 0.4 UI foundation (docs/SYNTH-DESIGN.md 20.1): dark theme with one accent colour.
    Every widget takes its colours, sizes and fonts from here (no literal colours elsewhere), so 0.5 can
    restyle by editing this file. Values may be tuned by agent E; the names are frozen. */
namespace theme
{
// Colours
inline const juce::Colour background { 0xff15171c };      ///< editor background
inline const juce::Colour panel { 0xff1e2128 };           ///< SectionPanel fill
inline const juce::Colour panelOutline { 0xff2c3039 };
inline const juce::Colour text { 0xffe6e8ec };
inline const juce::Colour textDim { 0xff9aa1ad };
inline const juce::Colour accent { 0xff4fc3f7 };          ///< values, active states
inline const juce::Colour accentDim { 0xff2a6f8f };
inline const juce::Colour modulation { 0xffffb74d };      ///< modulation ring of the knobs
inline const juce::Colour warning { 0xffef5350 };
inline const juce::Colour knobTrack { 0xff383d48 };

// Sizes (design pixels at 100 %)
inline constexpr int gap = 6;              ///< between widgets
inline constexpr int sectionPadding = 8;
inline constexpr int sectionTitleHeight = 20;
inline constexpr int cornerRadius = 6;
inline constexpr int knobSize = 52;        ///< rotary diameter + ring
inline constexpr int knobLabelHeight = 14;
inline constexpr int knobValueHeight = 14;
inline constexpr int chipHeight = 22;
inline constexpr int presetBarHeight = 30;

// Fonts
inline constexpr float fontSmall = 11.0f;
inline constexpr float fontNormal = 13.0f;
inline constexpr float fontTitle = 14.0f;

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}
} // namespace theme

} // namespace vocalsynth::ui
