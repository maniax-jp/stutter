#include "BlockGrid.h"
#include "../PluginProcessor.h"
#include "../dsp/ParamIndex.h"

namespace stutter::ui
{

const char* BlockGrid::getLaneName (int laneIndex)
{
    static const char* names[] = {
        "STUTTER", "TAPE STOP", "TAPE START", "REVERSE",
        "REPITCH", "GATE", "FILTER", "CRUSH",
        "STRETCHER", "SHUFFLER", "DELAY", "DISTORT"
    };
    if (laneIndex >= 0 && laneIndex < maxLanes)
        return names[laneIndex];
    return "";
}

BlockGrid::BlockGrid (StutterAudioProcessor& processor, SceneDocument& document)
    : proc (processor), doc (document)
{
    setWantsKeyboardFocus (false);
    startTimerHz (45);
}

BlockGrid::~BlockGrid() { stopTimer(); }

juce::Rectangle<int> BlockGrid::getLabelArea() const
{
    return getLocalBounds().removeFromLeft (labelColumnWidth);
}

juce::Rectangle<int> BlockGrid::getGridArea() const
{
    auto b = getLocalBounds();
    b.removeFromLeft (labelColumnWidth);
    return b;
}

int BlockGrid::getLaneHeight() const
{
    return juce::jmax (1, getGridArea().getHeight() / maxLanes);
}

float BlockGrid::getDivisionWidth() const
{
    const int divs = juce::jmax (1, doc.totalDivisions (sceneIndex));
    return (float) getGridArea().getWidth() / (float) divs;
}

juce::Rectangle<float> BlockGrid::getCellBounds (int lane, int division) const
{
    auto grid = getGridArea().toFloat();
    const float rowH = grid.getHeight() / (float) maxLanes;
    const float colW = getDivisionWidth();

    return { grid.getX() + (float) division * colW + colGap * 0.5f,
             grid.getY() + (float) lane * rowH + rowGap * 0.5f,
             colW - colGap,
             rowH - rowGap };
}

BlockGrid::HitInfo BlockGrid::hitTest (juce::Point<int> p) const
{
    HitInfo info;
    const auto labelArea = getLabelArea();
    const auto gridArea = getGridArea();

    if (labelArea.contains (p))
    {
        const float rowH = (float) labelArea.getHeight() / (float) maxLanes;
        info.onLabel = true;
        info.lane = juce::jlimit (0, maxLanes - 1, (int) ((float) (p.y - labelArea.getY()) / rowH));
        return info;
    }

    if (! gridArea.contains (p))
        return info;

    const int divs = juce::jmax (1, doc.totalDivisions (sceneIndex));
    const float rowH = (float) gridArea.getHeight() / (float) maxLanes;
    const float colW = getDivisionWidth();

    info.onGrid = true;
    info.lane = juce::jlimit (0, maxLanes - 1, (int) ((float) (p.y - gridArea.getY()) / rowH));
    info.division = juce::jlimit (0, divs - 1, (int) ((float) (p.x - gridArea.getX()) / colW));

    // Resolve which block, if any, sits under the cursor, and whether the pointer is close
    // enough to an edge that the user means to resize rather than move.
    auto scene = doc.ensureScene (sceneIndex);
    auto blocks = scene.getChildWithName (SceneIDs::blocksNode);
    if (blocks.isValid())
    {
        for (int i = 0; i < blocks.getNumChildren(); ++i)
        {
            const auto b = blocks.getChild (i);
            if ((int) b.getProperty (SceneIDs::laneRef, -1) != info.lane)
                continue;

            const int s = (int) b.getProperty (SceneIDs::start, 0);
            const int len = juce::jmax (1, (int) b.getProperty (SceneIDs::length, 1));
            if (info.division >= s && info.division < s + len)
            {
                info.blockStart = s;
                info.blockLength = len;

                const float leftX = gridArea.getX() + (float) s * colW;
                const float rightX = gridArea.getX() + (float) (s + len) * colW;
                info.nearLeftEdge = std::abs ((float) p.x - leftX) <= (float) edgeGrabPixels;
                info.nearRightEdge = std::abs ((float) p.x - rightX) <= (float) edgeGrabPixels;
                break;
            }
        }
    }

    return info;
}

void BlockGrid::mouseMove (const juce::MouseEvent& e)
{
    const auto hit = hitTest (e.getPosition());
    if (hit.lane != hoverLane || hit.division != hoverDivision)
    {
        hoverLane = hit.lane;
        hoverDivision = hit.division;
        repaint();
    }

    // Edge feedback: a left-right cursor is the only affordance telling the user that an
    // edge resizes rather than moves, since both are plain drags.
    if (hit.onGrid && hit.blockStart >= 0 && (hit.nearLeftEdge || hit.nearRightEdge))
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void BlockGrid::mouseDown (const juce::MouseEvent& e)
{
    const auto hit = hitTest (e.getPosition());

    if (hit.onLabel)
    {
        selectedLane = hit.lane;
        if (onLaneSelected)
            onLaneSelected (selectedLane);
        repaint();
        return;
    }

    if (! hit.onGrid)
        return;

    // One transaction per gesture, so a drag that moves a block across ten divisions is one
    // undo rather than ten.
    doc.getUndoManager().beginNewTransaction();

    dragLane = hit.lane;
    dragAnchorDiv = hit.division;

    if (e.mods.isRightButtonDown())
    {
        dragMode = DragMode::Erase;
        doc.removeBlockAt (sceneIndex, hit.lane, hit.division);
        doc.publish();
        repaint();
        return;
    }

    if (hit.blockStart >= 0)
    {
        dragOriginStart = hit.blockStart;
        dragOriginLength = hit.blockLength;

        if (hit.nearLeftEdge)        dragMode = DragMode::ResizeLeft;
        else if (hit.nearRightEdge)  dragMode = DragMode::ResizeRight;
        else                         dragMode = DragMode::Move;
    }
    else
    {
        dragMode = DragMode::Create;
        doc.addBlock (sceneIndex, hit.lane, hit.division, 1);
        dragOriginStart = hit.division;
        dragOriginLength = 1;
        doc.publish();
    }

    repaint();
}

void BlockGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMode == DragMode::None || dragLane < 0)
        return;

    const auto hit = hitTest (e.getPosition());
    const int divs = juce::jmax (1, doc.totalDivisions (sceneIndex));
    const int div = juce::jlimit (0, divs - 1, hit.division >= 0 ? hit.division : dragAnchorDiv);

    switch (dragMode)
    {
        case DragMode::Erase:
            // Right-drag erases everything it passes over, which is how a lane gets cleared
            // in a single gesture.
            if (doc.removeBlockAt (sceneIndex, dragLane, div))
            {
                doc.publish();
                repaint();
            }
            break;

        case DragMode::Create:
        {
            const int from = juce::jmin (dragAnchorDiv, div);
            const int to = juce::jmax (dragAnchorDiv, div) + 1;
            doc.removeBlockAt (sceneIndex, dragLane, dragAnchorDiv);
            doc.addBlock (sceneIndex, dragLane, from, to - from);
            doc.publish();
            repaint();
            break;
        }

        case DragMode::Move:
        {
            const int delta = div - dragAnchorDiv;
            const int newStart = juce::jlimit (0, divs - dragOriginLength, dragOriginStart + delta);
            doc.removeBlockAt (sceneIndex, dragLane, dragOriginStart);
            doc.addBlock (sceneIndex, dragLane, newStart, dragOriginLength);
            dragOriginStart = newStart;
            dragAnchorDiv = div;
            doc.publish();
            repaint();
            break;
        }

        case DragMode::ResizeRight:
        {
            const int newLen = juce::jmax (1, div - dragOriginStart + 1);
            doc.removeBlockAt (sceneIndex, dragLane, dragOriginStart);
            doc.addBlock (sceneIndex, dragLane, dragOriginStart, newLen);
            doc.publish();
            repaint();
            break;
        }

        case DragMode::ResizeLeft:
        {
            const int end = dragOriginStart + dragOriginLength;
            const int newStart = juce::jlimit (0, end - 1, div);
            doc.removeBlockAt (sceneIndex, dragLane, dragOriginStart);
            doc.addBlock (sceneIndex, dragLane, newStart, end - newStart);
            dragOriginStart = newStart;
            doc.publish();
            repaint();
            break;
        }

        case DragMode::None:
        default:
            break;
    }
}

void BlockGrid::mouseUp (const juce::MouseEvent&)
{
    dragMode = DragMode::None;
    dragLane = -1;
    dragAnchorDiv = -1;
    doc.publish();
    repaint();
}

void BlockGrid::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto hit = hitTest (e.getPosition());
    if (! hit.onGrid)
        return;

    if (e.mods.isRightButtonDown())
    {
        doc.getUndoManager().beginNewTransaction();
        doc.clearLane (sceneIndex, hit.lane);
        doc.publish();
        repaint();
    }
}

void BlockGrid::timerCallback()
{
    const int d = proc.getBlockPlayheadDivision();
    if (d != lastPlayheadDivision)
    {
        lastPlayheadDivision = d;
        repaint();
    }
}

void BlockGrid::resized() {}

void BlockGrid::paint (juce::Graphics& g)
{
    const auto labelArea = getLabelArea();
    const auto gridArea = getGridArea();
    const int divs = juce::jmax (1, doc.totalDivisions (sceneIndex));
    const int divisionsPerBeat = doc.getDivisions (sceneIndex);
    const float rowH = (float) gridArea.getHeight() / (float) maxLanes;

    g.fillAll (Palette::bg1);

    // --- Lane labels -----------------------------------------------------------------
    for (int lane = 0; lane < maxLanes; ++lane)
    {
        const auto row = juce::Rectangle<float> ((float) labelArea.getX(),
                                                 (float) labelArea.getY() + (float) lane * rowH,
                                                 (float) labelArea.getWidth(), rowH);
        const auto accent = Palette::laneColour (lane);
        const bool selected = lane == selectedLane;

        if (selected)
        {
            g.setColour (accent.withAlpha (0.16f));
            g.fillRect (row.reduced (2.0f, 1.0f));
        }

        g.setColour (selected ? accent : Palette::textLo);
        g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
        g.drawText (getLaneName (lane), row.reduced (10.0f, 0.0f),
                    juce::Justification::centredLeft, false);

        // Category dot: the Buffer/Texture distinction governs whether lanes are exclusive
        // or stack, and it is otherwise invisible in the grid.
        g.setColour (accent.withAlpha (0.9f));
        g.fillEllipse (row.getX() + 4.0f, row.getCentreY() - 2.0f, 4.0f, 4.0f);
    }

    // --- Grid background and beat divisions --------------------------------------------
    g.setColour (Palette::bg0);
    g.fillRect (gridArea);

    for (int d = 0; d <= divs; ++d)
    {
        const float x = (float) gridArea.getX() + (float) d * getDivisionWidth();
        const bool isBeat = (d % juce::jmax (1, divisionsPerBeat)) == 0;
        g.setColour (isBeat ? Palette::bg3 : Palette::bg2);
        g.fillRect (x - 0.5f, (float) gridArea.getY(), isBeat ? 1.5f : 1.0f,
                    (float) gridArea.getHeight());
    }

    // --- Blocks --------------------------------------------------------------------------
    auto scene = doc.ensureScene (sceneIndex);
    auto blocks = scene.getChildWithName (SceneIDs::blocksNode);
    if (blocks.isValid())
    {
        for (int i = 0; i < blocks.getNumChildren(); ++i)
        {
            const auto b = blocks.getChild (i);
            const int lane = (int) b.getProperty (SceneIDs::laneRef, -1);
            if (lane < 0 || lane >= maxLanes)
                continue;

            const int s = (int) b.getProperty (SceneIDs::start, 0);
            const int len = juce::jmax (1, (int) b.getProperty (SceneIDs::length, 1));
            const int tier = (int) b.getProperty (SceneIDs::tier, 0);

            const float colW = getDivisionWidth();
            const juce::Rectangle<float> r ((float) gridArea.getX() + (float) s * colW + colGap * 0.5f,
                                            (float) gridArea.getY() + (float) lane * rowH + rowGap * 0.5f,
                                            (float) len * colW - colGap,
                                            rowH - rowGap);

            const auto accent = Palette::laneColour (lane);

            // Tier is drawn as fill treatment rather than a badge: Locked is solid, Split
            // gets a gradient plus a direction arrow, Custom shows its curve inline. Seeing
            // the shape in the block is worth far more than a label would be.
            if (tier == 1)
            {
                g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.35f), r.getX(), 0.0f,
                                                         accent.withAlpha (0.95f), r.getRight(), 0.0f,
                                                         false));
                g.fillRoundedRectangle (r, 3.0f);
            }
            else
            {
                g.setColour (accent.withAlpha (0.85f));
                g.fillRoundedRectangle (r, 3.0f);
            }

            // Multi-pass glow, matching the treatment active cells get elsewhere in the UI.
            g.setColour (accent.withAlpha (0.25f));
            g.drawRoundedRectangle (r.expanded (1.0f), 4.0f, 1.5f);

            // Internal division ticks, so a long block still reads as N divisions rather
            // than as an undifferentiated bar.
            if (len > 1)
            {
                g.setColour (Palette::bg0.withAlpha (0.35f));
                for (int k = 1; k < len; ++k)
                    g.fillRect (r.getX() + (float) k * colW - 0.5f, r.getY() + 2.0f,
                                1.0f, r.getHeight() - 4.0f);
            }
        }
    }

    // --- Hover feedback --------------------------------------------------------------
    if (hoverLane >= 0 && hoverDivision >= 0 && dragMode == DragMode::None)
    {
        g.setColour (Palette::accent.withAlpha (0.10f));
        g.fillRect (getCellBounds (hoverLane, hoverDivision));
    }

    // --- Playhead -----------------------------------------------------------------------
    if (lastPlayheadDivision >= 0 && lastPlayheadDivision < divs)
    {
        const float colW = getDivisionWidth();
        const float x = (float) gridArea.getX() + (float) lastPlayheadDivision * colW;

        g.setColour (Palette::accent.withAlpha (0.12f));
        g.fillRect (x, (float) gridArea.getY(), colW, (float) gridArea.getHeight());

        g.setColour (Palette::accent.withAlpha (0.85f));
        g.fillRect (x, (float) gridArea.getY(), 2.0f, (float) gridArea.getHeight());
    }
}

} // namespace stutter::ui
