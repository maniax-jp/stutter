#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"
#include "LaneParamPanel.h"
#include "CurveEditor.h"
#include "ModRoutePanel.h"
#include <array>

class StutterAudioProcessor;

namespace stutter::ui
{

/** Bottom area: tab strip (LANE / VOLUME / FILTER / PAN) + the corresponding content view. */
/**
    A tab that can show a lamp as well as its selected state.

    The two have to be distinguishable at a glance: selection is already the tab's fill and
    border, so "this curve is shaping the sound" cannot also be a fill or a border without
    becoming a shade to compare rather than a mark to notice. The lamp is drawn by the button
    itself because a button paints over its parent -- drawing it from BottomTabs::paint put it
    underneath.
*/
class CurveTabButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    void setLampOn (bool shouldBeOn)
    {
        if (lampOn == shouldBeOn)
            return;
        lampOn = shouldBeOn;
        repaint();
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        juce::TextButton::paintButton (g, highlighted, down);

        if (! lampOn)
            return;

        constexpr float radius = 3.0f;
        const juce::Rectangle<float> lamp (6.0f, getHeight() * 0.5f - radius,
                                           radius * 2.0f, radius * 2.0f);

        g.setColour (Palette::accent);
        g.fillEllipse (lamp);
        g.setColour (Palette::accent.withAlpha (0.30f));
        g.drawEllipse (lamp.expanded (1.6f), 1.2f);
    }

private:
    bool lampOn = false;
};

class BottomTabs : public juce::Component, private juce::Timer
{
public:
    explicit BottomTabs (StutterAudioProcessor& processor);

    void setSelectedLane (int laneIndex);

    /** Point the modulation table at the scene the rest of the editor is showing. Without this
        the MOD tab stays on scene 0 while the grid and browser follow the user, so it lists a
        different scene's routes -- in practice an empty table, since factory content lives from
        C4 up. */
    void setSceneIndex (int sceneIndex);

    /** Forces all child views (lane knobs / curve editors) to repaint immediately -- used after
        a preset load, since curve breakpoints and step-grid data aren't APVTS parameters and so
        don't auto-refresh via attachment listeners the way sliders/combo boxes do. */
    void refreshAfterPresetLoad();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class Tab { Lane, Volume, Filter, Pan, Mod };

    void selectTab (Tab t);
    void updateLaneTabLabel();

    [[maybe_unused]] StutterAudioProcessor& proc;
    Tab currentTab = Tab::Lane;
    int selectedLane = 0;

    juce::TextButton laneTabButton;
    /** Tint the VOLUME / FILTER / PAN tabs when their curve is actually shaping something.
        A curve that is not flat changes the sound whether or not its tab is open, so without
        this the plugin could be audibly doing something with nothing on screen to say so. */
    void refreshCurveTabIndicators();
    void timerCallback() override;


    CurveTabButton volumeTabButton { "VOLUME" };
    CurveTabButton filterTabButton { "FILTER" };
    CurveTabButton panTabButton { "PAN" };
    juce::TextButton modTabButton { "MOD" };

    LaneParamPanel laneParamPanel;
    CurveEditor volumeCurveEditor;
    CurveEditor filterCurveEditor;
    CurveEditor panCurveEditor;
    ModRoutePanel modRoutePanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BottomTabs)
};

} // namespace stutter::ui
