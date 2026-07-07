#include "StepGrid.h"
#include "../PluginProcessor.h"

namespace stutter::ui
{

const char* StepGrid::getLaneName (int laneIndex)
{
    static const char* names[] = { "STUTTER", "TAPE STOP", "TAPE START", "REVERSE",
                                    "REPITCH", "GATE", "FILTER", "CRUSH" };
    if (laneIndex >= 0 && laneIndex < 8)
        return names[laneIndex];
    return "";
}

StepGrid::StepGrid (StutterAudioProcessor& processor) : proc (processor)
{
    setWantsKeyboardFocus (false);
    startTimerHz (45);
}

StepGrid::~StepGrid() { stopTimer(); }

juce::Rectangle<int> StepGrid::getLabelArea() const
{
    return getLocalBounds().removeFromLeft (labelColumnWidth);
}

juce::Rectangle<int> StepGrid::getGridArea() const
{
    auto b = getLocalBounds();
    b.removeFromLeft (labelColumnWidth);
    return b;
}

juce::Rectangle<float> StepGrid::getCellBounds (int lane, int step) const
{
    auto grid = getGridArea().toFloat();
    const float rowH = grid.getHeight() / (float) numLanes;
    const float colW = grid.getWidth() / (float) numSteps;

    return juce::Rectangle<float> (grid.getX() + (float) step * colW + colGap * 0.5f,
                                    grid.getY() + (float) lane * rowH + rowGap * 0.5f,
                                    colW - colGap,
                                    rowH - rowGap);
}

StepGrid::HitInfo StepGrid::hitTestCell (juce::Point<int> p) const
{
    HitInfo info;
    auto labelArea = getLabelArea();
    auto gridArea = getGridArea();

    if (labelArea.contains (p))
    {
        const float rowH = labelArea.getHeight() / (float) numLanes;
        info.onLabel = true;
        info.lane = juce::jlimit (0, numLanes - 1, (int) ((p.y - labelArea.getY()) / rowH));
        return info;
    }

    if (gridArea.contains (p))
    {
        const float rowH = gridArea.getHeight() / (float) numLanes;
        const float colW = gridArea.getWidth() / (float) numSteps;
        info.onCell = true;
        info.lane = juce::jlimit (0, numLanes - 1, (int) ((p.y - gridArea.getY()) / rowH));
        info.step = juce::jlimit (0, numSteps - 1, (int) ((p.x - gridArea.getX()) / colW));
    }

    return info;
}

void StepGrid::mouseDown (const juce::MouseEvent& e)
{
    auto hit = hitTestCell (e.getPosition());

    if (hit.onLabel)
    {
        selectedLane = hit.lane;
        if (onLaneSelected)
            onLaneSelected (selectedLane);
        repaint();
        return;
    }

    if (hit.onCell)
    {
        const bool current = proc.getSequencer().getStep (hit.lane, hit.step);
        dragPaintValue = ! current;
        isDragPainting = true;
        dragLane = hit.lane;
        proc.getSequencer().setStep (hit.lane, hit.step, dragPaintValue);
        repaint();
    }
}

void StepGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (! isDragPainting)
        return;

    auto hit = hitTestCell (e.getPosition());
    if (hit.onCell && hit.lane == dragLane)
    {
        proc.getSequencer().setStep (hit.lane, hit.step, dragPaintValue);
        repaint();
    }
}

void StepGrid::mouseUp (const juce::MouseEvent&)
{
    isDragPainting = false;
    dragLane = -1;
}

void StepGrid::timerCallback()
{
    const int step = proc.getSequencer().isEnabled() ? proc.getSequencer().getCurrentPlayheadStep() : -1;
    if (step != lastPlayheadStep)
    {
        lastPlayheadStep = step;
        repaint();
    }
}

void StepGrid::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (Palette::bg1);
    g.fillRect (bounds);

    auto labelArea = getLabelArea();
    auto gridArea = getGridArea();

    // Vertical divider between labels and grid
    g.setColour (Palette::bg3.withAlpha (0.5f));
    g.fillRect (juce::Rectangle<float> ((float) labelArea.getRight() - 1.0f, bounds.getY(), 1.0f, bounds.getHeight()));

    const float rowH = (float) gridArea.getHeight() / (float) numLanes;
    const float colW = (float) gridArea.getWidth() / (float) numSteps;

    // 4-step group divider lines (drawn first, subtle, spanning full grid height)
    for (int s = 0; s <= numSteps; ++s)
    {
        if (s % 4 != 0)
            continue;
        const float x = (float) gridArea.getX() + (float) s * colW;
        g.setColour (Palette::bg3.withAlpha (s == 0 || s == numSteps ? 0.0f : 0.55f));
        g.fillRect (juce::Rectangle<float> (x - 0.5f, (float) gridArea.getY(), 1.0f, (float) gridArea.getHeight()));
    }

    auto& seq = proc.getSequencer();

    for (int lane = 0; lane < numLanes; ++lane)
    {
        const auto laneColour = Palette::laneColours[(size_t) lane];
        const bool laneSelected = (lane == selectedLane);

        // ---- Lane label ----
        auto labelRow = juce::Rectangle<float> ((float) labelArea.getX(), (float) labelArea.getY() + (float) lane * rowH,
                                                 (float) labelArea.getWidth(), rowH);

        if (laneSelected)
        {
            g.setColour (Palette::bg2);
            g.fillRect (labelRow);
            g.setColour (laneColour);
            g.fillRect (labelRow.removeFromLeft (3.0f));
            labelRow.removeFromLeft (0.0f);
        }

        auto textArea = labelRow.reduced (10.0f, 0.0f);
        auto swatchArea = textArea.removeFromLeft (7.0f).withSizeKeepingCentre (7.0f, 7.0f);
        g.setColour (laneColour.withAlpha (laneSelected ? 1.0f : 0.65f));
        g.fillEllipse (swatchArea);

        textArea.removeFromLeft (6.0f);
        g.setColour (laneSelected ? Palette::textHi : Palette::textLo);
        g.setFont (juce::Font (juce::FontOptions (11.5f, laneSelected ? juce::Font::bold : juce::Font::plain)));
        g.drawText (getLaneName (lane), textArea, juce::Justification::centredLeft);

        // ---- Steps ----
        for (int step = 0; step < numSteps; ++step)
        {
            auto cell = getCellBounds (lane, step);
            const bool on = seq.getStep (lane, step);
            const float corner = juce::jmin (3.5f, cell.getHeight() * 0.25f);

            if (on)
            {
                // glow
                for (int i = 3; i >= 1; --i)
                {
                    g.setColour (laneColour.withAlpha (0.10f * (float) i));
                    g.fillRoundedRectangle (cell.expanded ((float) i * 1.6f), corner + (float) i);
                }
                g.setColour (laneColour);
                g.fillRoundedRectangle (cell, corner);
                g.setColour (juce::Colours::white.withAlpha (0.18f));
                g.fillRoundedRectangle (cell.removeFromTop (cell.getHeight() * 0.45f), corner);
            }
            else
            {
                g.setColour (Palette::bg2);
                g.fillRoundedRectangle (cell, corner);
                g.setColour (Palette::bg3.withAlpha (0.7f));
                g.drawRoundedRectangle (cell, corner, 1.0f);
            }
        }
    }

    // ---- Playhead ----
    if (lastPlayheadStep >= 0 && lastPlayheadStep < numSteps)
    {
        const float x = (float) gridArea.getX() + (float) lastPlayheadStep * colW;
        auto playheadRect = juce::Rectangle<float> (x, (float) gridArea.getY(), colW, (float) gridArea.getHeight());

        g.setColour (Palette::textHi.withAlpha (0.06f));
        g.fillRect (playheadRect);

        g.setColour (Palette::textHi.withAlpha (0.8f));
        g.fillRect (juce::Rectangle<float> (x, (float) gridArea.getY(), 2.0f, (float) gridArea.getHeight()));
        g.setColour (Palette::textHi.withAlpha (0.25f));
        g.fillRect (juce::Rectangle<float> (x - 2.0f, (float) gridArea.getY(), 2.0f, (float) gridArea.getHeight()));
        g.fillRect (juce::Rectangle<float> (x + 2.0f, (float) gridArea.getY(), 2.0f, (float) gridArea.getHeight()));
    }
}

void StepGrid::resized() {}

} // namespace stutter::ui
