#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"
#include "LaneParamPanel.h"
#include "CurveEditor.h"

class StutterAudioProcessor;

namespace stutter::ui
{

/** Bottom area: tab strip (LANE / VOLUME / FILTER / PAN) + the corresponding content view. */
class BottomTabs : public juce::Component
{
public:
    explicit BottomTabs (StutterAudioProcessor& processor);

    void setSelectedLane (int laneIndex);

    /** Forces all child views (lane knobs / curve editors) to repaint immediately -- used after
        a preset load, since curve breakpoints and step-grid data aren't APVTS parameters and so
        don't auto-refresh via attachment listeners the way sliders/combo boxes do. */
    void refreshAfterPresetLoad();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Tab { Lane, Volume, Filter, Pan };

    void selectTab (Tab t);
    void updateLaneTabLabel();

    [[maybe_unused]] StutterAudioProcessor& proc;
    Tab currentTab = Tab::Lane;
    int selectedLane = 0;

    juce::TextButton laneTabButton;
    juce::TextButton volumeTabButton { "VOLUME" };
    juce::TextButton filterTabButton { "FILTER" };
    juce::TextButton panTabButton { "PAN" };

    LaneParamPanel laneParamPanel;
    CurveEditor volumeCurveEditor;
    CurveEditor filterCurveEditor;
    CurveEditor panCurveEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BottomTabs)
};

} // namespace stutter::ui
