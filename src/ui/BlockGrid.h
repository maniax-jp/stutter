#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"
#include "../state/SceneDocument.h"

class StutterAudioProcessor;

namespace stutter::ui
{

/**
    Variable-length block editor. Replaces StepGrid's fixed on/off cells.

    The mouse vocabulary is borrowed from Glitch 2, which has the best-tuned set of the five
    reference products: every operation is a single gesture with no modifier keys, so the
    common edits stay reachable one-handed.

      drag on empty space   create a block, length follows the drag
      drag a block          move it
      drag a block's edge   resize it
      right-click           erase one block
      right-drag            erase several
      double-right-click    clear the lane
      click a lane header   select the lane (drives the parameter panel)

    Edits go through SceneDocument, so each drag is a single undo step: the transaction opens
    on mouse-down and every intermediate mutation joins it.
*/
class BlockGrid : public juce::Component, private juce::Timer
{
public:
    BlockGrid (StutterAudioProcessor& processor, SceneDocument& document);
    ~BlockGrid() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    std::function<void (int laneIndex)> onLaneSelected;

    int getSelectedLane() const noexcept { return selectedLane; }
    void setSceneIndex (int index) { sceneIndex = index; repaint(); }

    static const char* getLaneName (int laneIndex);

private:
    void timerCallback() override;

    enum class DragMode { None, Create, Move, ResizeLeft, ResizeRight, Erase };

    struct HitInfo
    {
        bool onLabel = false;
        bool onGrid = false;
        int lane = -1;
        int division = -1;
        /** Index of the block under the cursor within its lane, or -1. */
        int blockStart = -1;
        int blockLength = 0;
        bool nearLeftEdge = false;
        bool nearRightEdge = false;
    };

    HitInfo hitTest (juce::Point<int> p) const;

    juce::Rectangle<int> getGridArea() const;
    juce::Rectangle<int> getLabelArea() const;
    juce::Rectangle<float> getCellBounds (int lane, int division) const;
    float getDivisionWidth() const;
    int getLaneHeight() const;

    StutterAudioProcessor& proc;
    SceneDocument& doc;

    int sceneIndex = 0;
    int selectedLane = 0;
    int lastPlayheadDivision = -1;

    DragMode dragMode = DragMode::None;
    int dragLane = -1;
    int dragAnchorDiv = -1;
    int dragOriginStart = -1;
    int dragOriginLength = 0;

    /** Cursor feedback: which edge, if any, the pointer is hovering. */
    int hoverLane = -1;
    int hoverDivision = -1;

    static constexpr int labelColumnWidth = 108;
    static constexpr int rowGap = 3;
    static constexpr int colGap = 2;
    /** How close to a block edge counts as "grab the edge" rather than "grab the body". */
    static constexpr int edgeGrabPixels = 6;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BlockGrid)
};

} // namespace stutter::ui
