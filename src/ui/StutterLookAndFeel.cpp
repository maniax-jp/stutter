#include "StutterLookAndFeel.h"

namespace stutter::ui
{

const juce::Colour Palette::bg0 = juce::Colour (0xff0d0e12);
const juce::Colour Palette::bg1 = juce::Colour (0xff14161c);
const juce::Colour Palette::bg2 = juce::Colour (0xff1d2028);
const juce::Colour Palette::bg3 = juce::Colour (0xff2b2f3a);

const juce::Colour Palette::textHi = juce::Colour (0xfff0f1f5);
const juce::Colour Palette::textLo = juce::Colour (0xff868c9c);

const juce::Colour Palette::accent    = juce::Colour (0xff5ce1e6);
const juce::Colour Palette::accentDim = juce::Colour (0xff2f6f73);

const std::array<juce::Colour, 8> Palette::laneColours = { {
    juce::Colour (0xff37e2e8), // 0 Stutter    - cyan
    juce::Colour (0xff5b8cff), // 1 TapeStop   - blue
    juce::Colour (0xff8f6bff), // 2 TapeStart  - violet
    juce::Colour (0xffd85bd0), // 3 Reverse    - magenta
    juce::Colour (0xff4fd192), // 4 Repitch    - teal green (buffer group ends, cool tones)
    juce::Colour (0xffffb648), // 5 Gate       - orange (texture group starts, warm tones)
    juce::Colour (0xffff7a5c), // 6 Filter     - coral/red
    juce::Colour (0xffd6e04a), // 7 Crush      - acid yellow-green
} };

StutterLookAndFeel::StutterLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, Palette::bg1);
    setColour (juce::Slider::textBoxTextColourId, Palette::textHi);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, Palette::textHi);
    setColour (juce::TextButton::buttonColourId, Palette::bg2);
    setColour (juce::TextButton::buttonOnColourId, Palette::accentDim);
    setColour (juce::TextButton::textColourOffId, Palette::textLo);
    setColour (juce::TextButton::textColourOnId, Palette::textHi);
    setColour (juce::ComboBox::backgroundColourId, Palette::bg2);
    setColour (juce::ComboBox::textColourId, Palette::textHi);
    setColour (juce::ComboBox::outlineColourId, Palette::bg3);
    setColour (juce::PopupMenu::backgroundColourId, Palette::bg2);
    setColour (juce::PopupMenu::textColourId, Palette::textHi);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::accentDim);
}

juce::Font StutterLookAndFeel::monoFont (float size)
{
    return juce::Font (juce::FontOptions (size, juce::Font::plain).withName (juce::Font::getDefaultMonospacedFontName()));
}

juce::Font StutterLookAndFeel::titleFont (float size)
{
    return juce::Font (juce::FontOptions (size, juce::Font::bold));
}

juce::Font StutterLookAndFeel::getLabelFont (juce::Label& l)
{
    return juce::Font (juce::FontOptions (juce::jmax (11.0f, l.getHeight() * 0.62f), juce::Font::plain));
}

juce::Font StutterLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::Font (juce::FontOptions (juce::jmax (11.0f, buttonHeight * 0.45f), juce::Font::plain));
}

void StutterLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                            juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (4.0f);
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
    if (accent == juce::Colours::transparentBlack)
        accent = Palette::accent;

    const bool enabled = slider.isEnabled();
    const float alphaScale = enabled ? 1.0f : 0.35f;

    // --- Outer track (full sweep, dim) ---
    const float trackThickness = juce::jmax (2.5f, radius * 0.16f);
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius - trackThickness * 0.5f, radius - trackThickness * 0.5f,
                          0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (Palette::bg3.withAlpha (alphaScale));
    g.strokePath (track, juce::PathStrokeType (trackThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // --- Active arc with glow ---
    if (sliderPos > 0.001f || angle > rotaryStartAngle + 0.001f)
    {
        juce::Path active;
        active.addCentredArc (centre.x, centre.y, radius - trackThickness * 0.5f, radius - trackThickness * 0.5f,
                               0.0f, rotaryStartAngle, angle, true);

        if (enabled)
        {
            // glow: several soft passes
            for (int i = 4; i >= 1; --i)
            {
                const float glowWidth = trackThickness + (float) i * 2.6f;
                g.setColour (accent.withAlpha (0.05f * (float) i));
                g.strokePath (active, juce::PathStrokeType (glowWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
        g.setColour (accent.withAlpha (alphaScale));
        g.strokePath (active, juce::PathStrokeType (trackThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // --- Knob body ---
    const float knobRadius = radius - trackThickness - 4.0f;
    if (knobRadius > 2.0f)
    {
        juce::ColourGradient grad (Palette::bg2.brighter (0.06f), centre.x, centre.y - knobRadius,
                                    Palette::bg0, centre.x, centre.y + knobRadius, false);
        g.setGradientFill (grad);
        g.fillEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);

        g.setColour (Palette::bg3.withAlpha (0.8f));
        g.drawEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

        // pointer
        juce::Path pointer;
        const float pointerLen = knobRadius * 0.78f;
        const float pointerThick = juce::jmax (1.6f, knobRadius * 0.12f);
        pointer.addRoundedRectangle (-pointerThick * 0.5f, -pointerLen, pointerThick, pointerLen * 0.62f, pointerThick * 0.5f);
        g.setColour (enabled ? accent : Palette::textLo);
        g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
    }
}

void StutterLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float minSliderPos, float maxSliderPos,
                                            const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    juce::ignoreUnused (minSliderPos, maxSliderPos);
    auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);

    auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
    if (accent == juce::Colours::transparentBlack)
        accent = Palette::accent;

    if (style == juce::Slider::LinearHorizontal)
    {
        const float trackY = bounds.getCentreY();
        const float trackH = 4.0f;
        auto trackRect = juce::Rectangle<float> (bounds.getX(), trackY - trackH * 0.5f, bounds.getWidth(), trackH);
        g.setColour (Palette::bg3);
        g.fillRoundedRectangle (trackRect, trackH * 0.5f);

        auto fillRect = juce::Rectangle<float> (bounds.getX(), trackY - trackH * 0.5f, sliderPos - bounds.getX(), trackH);
        g.setColour (accent);
        g.fillRoundedRectangle (fillRect, trackH * 0.5f);

        const float thumbR = 6.5f;
        g.setColour (accent.withAlpha (0.25f));
        g.fillEllipse (sliderPos - thumbR * 1.6f, trackY - thumbR * 1.6f, thumbR * 3.2f, thumbR * 3.2f);
        g.setColour (Palette::textHi);
        g.fillEllipse (sliderPos - thumbR, trackY - thumbR, thumbR * 2.0f, thumbR * 2.0f);
    }
    else
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
    }
}

void StutterLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                            bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused (shouldDrawButtonAsDown);
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);

    // Pill-style switch when there's no text (compact toggle), else a labelled switch row.
    const bool hasText = button.getButtonText().isNotEmpty();

    juce::Rectangle<float> pill;
    if (hasText)
    {
        pill = bounds.removeFromLeft (juce::jmin (40.0f, bounds.getWidth() * 0.42f)).reduced (0.0f, bounds.getHeight() * 0.22f);
    }
    else
    {
        pill = bounds.reduced (0.0f, bounds.getHeight() * 0.12f);
    }

    const bool on = button.getToggleState();
    auto trackColour = on ? Palette::accentDim : Palette::bg3;
    g.setColour (trackColour);
    g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);

    if (on)
    {
        g.setColour (Palette::accent.withAlpha (0.35f));
        g.drawRoundedRectangle (pill.expanded (1.5f), pill.getHeight() * 0.5f + 1.0f, 2.0f);
    }

    const float knobD = pill.getHeight() - 4.0f;
    const float knobX = on ? pill.getRight() - knobD - 2.0f : pill.getX() + 2.0f;
    g.setColour (on ? Palette::accent : Palette::textLo.withAlpha (0.8f));
    g.fillEllipse (knobX, pill.getY() + 2.0f, knobD, knobD);

    if (hasText)
    {
        g.setColour (shouldDrawButtonAsHighlighted ? Palette::textHi : Palette::textLo);
        g.setFont (juce::Font (juce::FontOptions (juce::jmax (11.0f, bounds.getHeight() * 0.5f))));
        g.drawText (button.getButtonText(), bounds.withTrimmedLeft (8), juce::Justification::centredLeft);
    }
}

void StutterLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                                bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    const float corner = juce::jmin (6.0f, bounds.getHeight() * 0.3f);

    const bool on = button.getToggleState();
    juce::Colour base = on ? Palette::accentDim : Palette::bg2;

    if (shouldDrawButtonAsDown)
        base = base.darker (0.15f);
    else if (shouldDrawButtonAsHighlighted)
        base = base.brighter (0.08f);

    g.setColour (base);
    g.fillRoundedRectangle (bounds, corner);

    if (on)
    {
        g.setColour (Palette::accent.withAlpha (0.55f));
        g.drawRoundedRectangle (bounds, corner, 1.4f);
    }
    else
    {
        g.setColour (Palette::bg3);
        g.drawRoundedRectangle (bounds, corner, 1.0f);
    }
}

void StutterLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                          bool shouldDrawButtonAsHighlighted, bool)
{
    const bool on = button.getToggleState();
    g.setColour (on ? Palette::textHi : (shouldDrawButtonAsHighlighted ? Palette::textHi : Palette::textLo));
    g.setFont (getTextButtonFont (button, button.getHeight()));
    g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred);
}

void StutterLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                        int, int, int, int, juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (1.0f);
    g.setColour (Palette::bg2);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (Palette::bg3);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    auto arrowZone = bounds.removeFromRight (18.0f);
    juce::Path arrow;
    arrow.addTriangle (arrowZone.getCentreX() - 4.0f, arrowZone.getCentreY() - 2.5f,
                        arrowZone.getCentreX() + 4.0f, arrowZone.getCentreY() - 2.5f,
                        arrowZone.getCentreX(), arrowZone.getCentreY() + 3.5f);
    g.setColour (Palette::textLo);
    g.fillPath (arrow);
    juce::ignoreUnused (box);
}

} // namespace stutter::ui
