#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"

class StutterAudioProcessor;

namespace stutter::ui
{

/**
    8-lane x 16-step grid. Click/drag to paint steps on/off, click a lane name
    to select it (drives LaneParamPanel display), and shows a live playhead.
*/
class StepGrid : public juce::Component, private juce::Timer
{
public:
    explicit StepGrid (StutterAudioProcessor& processor);
    ~StepGrid() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    /** Called when the user clicks a lane name to select it. */
    std::function<void (int laneIndex)> onLaneSelected;

    int getSelectedLane() const noexcept { return selectedLane; }

    static const char* getLaneName (int laneIndex);

private:
    void timerCallback() override;

    struct HitInfo
    {
        bool onLabel = false;
        bool onCell = false;
        int lane = -1;
        int step = -1;
    };
    HitInfo hitTestCell (juce::Point<int> p) const;

    juce::Rectangle<int> getGridArea() const;
    juce::Rectangle<int> getLabelArea() const;
    juce::Rectangle<float> getCellBounds (int lane, int step) const;

    StutterAudioProcessor& proc;

    int selectedLane = 0;
    int lastPlayheadStep = -1;

    // Drag-paint state
    bool isDragPainting = false;
    bool dragPaintValue = false;
    int dragLane = -1;

    static constexpr int numLanes = 8;
    static constexpr int numSteps = 16;

    static constexpr int labelColumnWidth = 108;
    static constexpr int rowGap = 3;
    static constexpr int colGap = 3;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StepGrid)
};

} // namespace stutter::ui
