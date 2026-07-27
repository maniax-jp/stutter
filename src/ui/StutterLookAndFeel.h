#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace stutter::ui
{

/** Central palette for the whole plugin. Keep colour count small and consistent. */
struct Palette
{
    // Background tiers
    static const juce::Colour bg0;   // deepest background
    static const juce::Colour bg1;   // panel background
    static const juce::Colour bg2;   // raised panel / cell background
    static const juce::Colour bg3;   // hairlines / dividers

    // Text
    static const juce::Colour textHi;
    static const juce::Colour textLo;

    // Accent (global / neutral interactive elements)
    static const juce::Colour accent;
    static const juce::Colour accentDim;

    // Lane accent colours (12). Buffer lanes (0-4) trend cyan/blue/violet, Texture lanes
    // (5-7, 11) trend warm/saturated, and the v2 additions (8-11) continue the same split so
    // the two categories still read as distinct groups at a glance.
    static const std::array<juce::Colour, 12> laneColours;

    /** Lane accent, clamped. Prefer this over indexing laneColours directly so a lane index
        from state cannot read past the end. */
    static juce::Colour laneColour (int laneIndex) noexcept
    {
        return laneColours[(size_t) juce::jlimit (0, (int) laneColours.size() - 1, laneIndex)];
    }
};

/** Custom JUCE LookAndFeel: dark theme, glow rotary knobs, custom toggle. */
class StutterLookAndFeel : public juce::LookAndFeel_V4
{
public:
    StutterLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPosProportional, float rotaryStartAngle,
                            float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPos, float minSliderPos, float maxSliderPos,
                            const juce::Slider::SliderStyle, juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                            bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&,
                          bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    static juce::Font monoFont (float size);
    static juce::Font titleFont (float size);

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                        int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
};

} // namespace stutter::ui
