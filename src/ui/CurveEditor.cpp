#include "CurveEditor.h"
#include "../PluginProcessor.h"

namespace stutter::ui
{

namespace
{
constexpr float plotMargin = 10.0f;
constexpr float pointRadius = 5.5f;
constexpr float hitRadius = 11.0f;

const char* presetNames[] = { "SawDown", "SawUp", "Sine", "Square", "SidechainDuck", "Steps" };
const char* presetLabels[] = { "Saw Dn", "Saw Up", "Sine", "Square", "Duck", "Steps" };
}

CurveEditor::CurveEditor (StutterAudioProcessor& processor, stutter::ModTarget target, juce::Colour accent)
    : proc (processor), modTarget (target), accentColour (accent)
{
    enableButton.setClickingTogglesState (true);
    enableButton.setToggleState (curve().isEnabled(), juce::dontSendNotification);
    enableButton.setColour (juce::TextButton::buttonOnColourId, accentColour.withAlpha (0.35f));
    enableButton.onClick = [this]
    {
        curve().setEnabled (enableButton.getToggleState());
        proc.getPresetManager().markDirty();
        repaint();
    };
    addAndMakeVisible (enableButton);

    for (int i = 0; i < (int) (sizeof (presetNames) / sizeof (presetNames[0])); ++i)
    {
        auto btn = std::make_unique<juce::TextButton> (presetLabels[i]);
        const juce::String name = presetNames[i];
        btn->onClick = [this, name]
        {
            curve().applyPreset (name);
            proc.getPresetManager().markDirty();
            repaint();
        };
        addAndMakeVisible (*btn);
        presetButtons.push_back (std::move (btn));
    }

    startTimerHz (30);
}

CurveEditor::~CurveEditor() { stopTimer(); }

stutter::CurveModulator& CurveEditor::curve() const { return proc.getCurve (modTarget); }

void CurveEditor::timerCallback()
{
    // Keep the enable button in sync if state changes elsewhere (e.g. preset load).
    const bool en = curve().isEnabled();
    if (enableButton.getToggleState() != en)
        enableButton.setToggleState (en, juce::dontSendNotification);
}

juce::Rectangle<float> CurveEditor::getPlotArea() const
{
    auto r = getLocalBounds().toFloat();
    r.removeFromTop (36.0f); // toolbar row (enable + presets)
    return r.reduced (plotMargin, plotMargin);
}

juce::Point<float> CurveEditor::pointToXY (const stutter::CurvePoint& p, juce::Rectangle<float> plot) const
{
    return { plot.getX() + p.position * plot.getWidth(),
             plot.getBottom() - p.value * plot.getHeight() };
}

stutter::CurvePoint CurveEditor::xyToPoint (juce::Point<float> xy, juce::Rectangle<float> plot) const
{
    stutter::CurvePoint p;
    p.position = juce::jlimit (0.0f, 1.0f, (xy.x - plot.getX()) / plot.getWidth());
    p.value = juce::jlimit (0.0f, 1.0f, (plot.getBottom() - xy.y) / plot.getHeight());
    return p;
}

int CurveEditor::findNearestPointIndex (juce::Point<float> screenPos, juce::Rectangle<float> plot, float maxDist) const
{
    const auto& pts = curve().getPoints();
    int best = -1;
    float bestDist = maxDist;
    for (int i = 0; i < (int) pts.size(); ++i)
    {
        const auto xy = pointToXY (pts[(size_t) i], plot);
        const float d = xy.getDistanceFrom (screenPos);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

int CurveEditor::findSegmentIndex (float screenX, juce::Rectangle<float> plot) const
{
    const auto& pts = curve().getPoints();
    if (pts.size() < 2)
        return -1;

    const float pos = juce::jlimit (0.0f, 1.0f, (screenX - plot.getX()) / plot.getWidth());
    for (int i = 0; i + 1 < (int) pts.size(); ++i)
        if (pos >= pts[(size_t) i].position && pos <= pts[(size_t) i + 1].position)
            return i;
    return (int) pts.size() - 2;
}

void CurveEditor::pushPoints (std::vector<stutter::CurvePoint> pts)
{
    curve().setPoints (std::move (pts));
    proc.getPresetManager().markDirty();
    repaint();
}

void CurveEditor::mouseDown (const juce::MouseEvent& e)
{
    auto plot = getPlotArea();
    if (! plot.contains (e.position))
        return;

    const bool wantsCurvature = e.mods.isRightButtonDown() || e.mods.isAltDown() || e.mods.isCommandDown();

    if (wantsCurvature)
    {
        curvatureSegmentIndex = findSegmentIndex (e.position.x, plot);
        isCurvatureDrag = true;
        lastDragScreenPos = e.position;
        return;
    }

    const int idx = findNearestPointIndex (e.position, plot, hitRadius);
    if (idx >= 0)
    {
        draggingPointIndex = idx;
    }
}

void CurveEditor::mouseDrag (const juce::MouseEvent& e)
{
    auto plot = getPlotArea();

    if (isCurvatureDrag && curvatureSegmentIndex >= 0)
    {
        auto pts = curve().getPoints();
        if (curvatureSegmentIndex < (int) pts.size())
        {
            const float dy = lastDragScreenPos.y - e.position.y; // up = increase curvature
            float& c = pts[(size_t) curvatureSegmentIndex].curvature;
            c = juce::jlimit (-1.0f, 1.0f, c + dy / 120.0f);
            lastDragScreenPos = e.position;
            pushPoints (std::move (pts));
        }
        return;
    }

    if (draggingPointIndex >= 0)
    {
        auto pts = curve().getPoints();
        if (draggingPointIndex < (int) pts.size())
        {
            auto newPoint = xyToPoint (e.position, plot);

            // Keep endpoints pinned at position 0 / 1; only interior points move in x.
            const bool isFirst = draggingPointIndex == 0;
            const bool isLast = draggingPointIndex == (int) pts.size() - 1;

            float minPos = isFirst ? 0.0f : pts[(size_t) draggingPointIndex - 1].position + 0.001f;
            float maxPos = isLast ? 1.0f : pts[(size_t) draggingPointIndex + 1].position - 0.001f;

            if (isFirst || isLast)
                newPoint.position = pts[(size_t) draggingPointIndex].position; // pinned x
            else
                newPoint.position = juce::jlimit (minPos, maxPos, newPoint.position);

            newPoint.curvature = pts[(size_t) draggingPointIndex].curvature;
            pts[(size_t) draggingPointIndex] = newPoint;
            pushPoints (std::move (pts));
        }
    }
}

void CurveEditor::mouseUp (const juce::MouseEvent&)
{
    draggingPointIndex = -1;
    isCurvatureDrag = false;
    curvatureSegmentIndex = -1;
}

void CurveEditor::mouseMove (const juce::MouseEvent& e)
{
    auto plot = getPlotArea();
    const int idx = plot.contains (e.position) ? findNearestPointIndex (e.position, plot, hitRadius) : -1;
    if (idx != hoverPointIndex)
    {
        hoverPointIndex = idx;
        repaint();
    }
}

void CurveEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto plot = getPlotArea();
    if (! plot.contains (e.position))
        return;

    auto pts = curve().getPoints();
    const int idx = findNearestPointIndex (e.position, plot, hitRadius);

    if (idx >= 0)
    {
        // Delete point, but keep at least 2 points and never delete the endpoints.
        if (pts.size() > 2 && idx != 0 && idx != (int) pts.size() - 1)
        {
            pts.erase (pts.begin() + idx);
            pushPoints (std::move (pts));
        }
        return;
    }

    // Add a new point at the clicked position/value.
    auto newPoint = xyToPoint (e.position, plot);
    pts.push_back (newPoint);
    pushPoints (std::move (pts));
}

void CurveEditor::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bg1);

    // ---- Toolbar row background ----
    auto toolbar = getLocalBounds().removeFromTop (36).toFloat();
    g.setColour (Palette::bg2.withAlpha (0.4f));
    g.fillRect (toolbar);

    auto plot = getPlotArea();
    const bool enabled = curve().isEnabled();

    // ---- Plot background ----
    g.setColour (Palette::bg0.withAlpha (0.6f));
    g.fillRoundedRectangle (plot, 4.0f);
    g.setColour (Palette::bg3.withAlpha (0.6f));
    g.drawRoundedRectangle (plot, 4.0f, 1.0f);

    // Center gridline (unity/0.5) as a subtle reference
    g.setColour (Palette::bg3.withAlpha (0.45f));
    const float midY = plot.getCentreY();
    g.drawLine (plot.getX(), midY, plot.getRight(), midY, 1.0f);

    // quarter gridlines vertically (bar quarters)
    for (int i = 1; i < 4; ++i)
    {
        const float x = plot.getX() + plot.getWidth() * (float) i / 4.0f;
        g.setColour (Palette::bg3.withAlpha (0.25f));
        g.drawLine (x, plot.getY(), x, plot.getBottom(), 1.0f);
    }

    const auto& pts = curve().getPoints();
    if (pts.size() < 2)
        return;

    const float dimAlpha = enabled ? 1.0f : 0.35f;

    // ---- Build smooth path by sampling the same curvature-shaping the DSP uses ----
    juce::Path fillPath, linePath;
    const int samples = juce::jmax (2, (int) plot.getWidth());

    auto evalAt = [&pts] (float phase) -> float
    {
        size_t seg = 0;
        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            if (phase >= pts[i].position && phase <= pts[i + 1].position)
            {
                seg = i;
                break;
            }
            seg = i;
        }
        const auto& a = pts[seg];
        const auto& b = pts[juce::jmin (seg + 1, pts.size() - 1)];
        const float span = b.position - a.position;
        float t = span > 1.0e-6f ? (phase - a.position) / span : 0.0f;
        t = juce::jlimit (0.0f, 1.0f, t);
        float shapedT = t;
        if (std::abs (a.curvature) > 1.0e-3f)
        {
            const float exponent = std::pow (10.0f, -a.curvature);
            shapedT = std::pow (t, exponent);
        }
        return a.value + shapedT * (b.value - a.value);
    };

    for (int i = 0; i <= samples; ++i)
    {
        const float phase = (float) i / (float) samples;
        const float value = juce::jlimit (0.0f, 1.0f, evalAt (phase));
        const float x = plot.getX() + phase * plot.getWidth();
        const float y = plot.getBottom() - value * plot.getHeight();

        if (i == 0)
        {
            linePath.startNewSubPath (x, y);
            fillPath.startNewSubPath (x, plot.getBottom());
            fillPath.lineTo (x, y);
        }
        else
        {
            linePath.lineTo (x, y);
            fillPath.lineTo (x, y);
        }
    }
    fillPath.lineTo (plot.getRight(), plot.getBottom());
    fillPath.closeSubPath();

    juce::ColourGradient fillGrad (accentColour.withAlpha (0.32f * dimAlpha), plot.getX(), plot.getY(),
                                    accentColour.withAlpha (0.0f), plot.getX(), plot.getBottom(), false);
    g.setGradientFill (fillGrad);
    g.fillPath (fillPath);

    // glow pass on the line
    if (enabled)
    {
        for (int i = 3; i >= 1; --i)
        {
            g.setColour (accentColour.withAlpha (0.06f * (float) i));
            g.strokePath (linePath, juce::PathStrokeType (1.4f + (float) i * 1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
    g.setColour (accentColour.withAlpha (dimAlpha));
    g.strokePath (linePath, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // ---- Breakpoints ----
    for (int i = 0; i < (int) pts.size(); ++i)
    {
        const auto xy = pointToXY (pts[(size_t) i], plot);
        const bool hovered = (i == hoverPointIndex) || (i == draggingPointIndex);

        if (hovered)
        {
            g.setColour (accentColour.withAlpha (0.28f));
            g.fillEllipse (juce::Rectangle<float> (pointRadius * 2.6f, pointRadius * 2.6f).withCentre (xy));
        }

        g.setColour (Palette::bg0);
        g.fillEllipse (juce::Rectangle<float> (pointRadius * 2.0f + 2.0f, pointRadius * 2.0f + 2.0f).withCentre (xy));
        g.setColour (accentColour.withAlpha (dimAlpha));
        g.fillEllipse (juce::Rectangle<float> (pointRadius * 2.0f, pointRadius * 2.0f).withCentre (xy));
    }
}

void CurveEditor::resized()
{
    auto toolbar = getLocalBounds().removeFromTop (36).reduced (10, 6);
    enableButton.setBounds (toolbar.removeFromLeft (44));
    toolbar.removeFromLeft (10);

    const int n = (int) presetButtons.size();
    if (n > 0)
    {
        const int gap = 6;
        const int w = (toolbar.getWidth() - gap * (n - 1)) / n;
        for (auto& btn : presetButtons)
        {
            btn->setBounds (toolbar.removeFromLeft (w));
            toolbar.removeFromLeft (gap);
        }
    }
}

} // namespace stutter::ui
