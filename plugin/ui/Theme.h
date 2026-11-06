#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vocalsynth::ui
{

/** Design tokens of the 0.4 UI foundation (docs/SYNTH-DESIGN.md 20.1): dark theme with one accent colour.
    Every widget takes its colours, sizes and fonts from here (no literal colours elsewhere), so 0.5 can
    restyle by editing this file. Values may be tuned by agent E; the names are frozen (new tokens may be
    added). */
namespace theme
{
// Colours
inline const juce::Colour background { 0xff111317 };      ///< editor background
inline const juce::Colour panel { 0xff1b1e24 };           ///< SectionPanel fill
inline const juce::Colour panelOutline { 0xff2a2e37 };
inline const juce::Colour text { 0xffe6e8ec };
inline const juce::Colour textDim { 0xff8d95a3 };
inline const juce::Colour accent { 0xff4fc3f7 };          ///< values, active states
inline const juce::Colour accentDim { 0xff2a6f8f };
inline const juce::Colour modulation { 0xffffb74d };      ///< modulation ring of the knobs
inline const juce::Colour warning { 0xffef5350 };
inline const juce::Colour knobTrack { 0xff30353f };

// Added by agent E (0.4)
inline const juce::Colour panelHeader { 0xff20242b };     ///< title band of a SectionPanel
inline const juce::Colour control { 0xff262a32 };         ///< chips, buttons, combo boxes, slider tracks
inline const juce::Colour controlHover { 0xff2f343d };
inline const juce::Colour controlOutline { 0xff383d48 };
inline const juce::Colour knobBody { 0xff2a2f38 };        ///< rotary cap (top of the gradient)
inline const juce::Colour knobBodyDark { 0xff1f2329 };    ///< rotary cap (bottom of the gradient)
inline const juce::Colour textOnAccent { 0xff0d1a22 };    ///< text on an accent fill
inline const juce::Colour meterBackground { 0xff15181d };
inline const juce::Colour success { 0xff66bb6a };

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
inline constexpr int chipCaptionHeight = 13; ///< caption above a ChoiceChip (agent E)
inline constexpr float controlCorner = 4.0f; ///< corner radius of chips and buttons (agent E)

// Fonts
inline constexpr float fontSmall = 11.5f;
inline constexpr float fontNormal = 13.0f;
inline constexpr float fontTitle = 12.5f;

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

/** Section titles: bold, slightly spaced capitals (agent E). */
inline juce::Font titleFont()
{
    return font (fontTitle, true).withExtraKerningFactor (0.06f);
}
} // namespace theme

} // namespace vocalsynth::ui
