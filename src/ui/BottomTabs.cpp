#include "BottomTabs.h"
#include "BlockGrid.h"
#include "../PluginProcessor.h"

namespace stutter::ui
{

BottomTabs::BottomTabs (StutterAudioProcessor& processor)
    : proc (processor),
      laneParamPanel (processor),
      volumeCurveEditor (processor, stutter::ModTarget::Volume, Palette::accent),
      filterCurveEditor (processor, stutter::ModTarget::Filter, Palette::laneColours[6]),
      panCurveEditor (processor, stutter::ModTarget::Pan, Palette::laneColours[3]),
      modRoutePanel (processor, processor.getSceneDocument())
{
    auto setupTab = [this] (juce::TextButton& b, Tab t)
    {
        b.setClickingTogglesState (false);
        b.onClick = [this, t] { selectTab (t); };
        addAndMakeVisible (b);
    };

    updateLaneTabLabel();
    setupTab (laneTabButton, Tab::Lane);
    setupTab (volumeTabButton, Tab::Volume);
    setupTab (filterTabButton, Tab::Filter);
    setupTab (panTabButton, Tab::Pan);
    setupTab (modTabButton, Tab::Mod);

    addChildComponent (laneParamPanel);
    addChildComponent (volumeCurveEditor);
    addChildComponent (filterCurveEditor);
    addChildComponent (panCurveEditor);
    addChildComponent (modRoutePanel);

    selectTab (Tab::Lane);

    // 8Hz is plenty: this only has to catch a curve being drawn or a preset changing one, and
    // both are human-speed events.
    startTimerHz (8);
    refreshCurveTabIndicators();
}

void BottomTabs::timerCallback()
{
    refreshCurveTabIndicators();
}

void BottomTabs::refreshCurveTabIndicators()
{
    struct Entry { ModTarget target; const char* name; CurveTabButton* button; };
    const Entry entries[] = {
        { ModTarget::Volume, ID::curveNameVolume.toRawUTF8(), &volumeTabButton },
        { ModTarget::Filter, ID::curveNameFilter.toRawUTF8(), &filterTabButton },
        { ModTarget::Pan,    ID::curveNamePan.toRawUTF8(),    &panTabButton },
    };

    for (const auto& e : entries)
    {
        const auto& curve = proc.getCurve (e.target);
        const float neutral = ID::neutralValueForCurve (e.name);

        // "Doing something" means the shape departs from its own neutral value somewhere.
        // Sampling a handful of phases is enough to catch any curve a person would draw, and
        // avoids walking the whole baked table eight times a second.
        bool active = false;
        for (int i = 0; i < 16 && ! active; ++i)
            active = std::abs (curve.getValueAtPhase ((float) i / 16.0f) - neutral) > 1.0e-3f;

        // Recorded for paint() to draw a lamp beside the label. Tinting the tab was tried
        // first and read as a second, dimmer version of "selected" -- two states drawn in the
        // same visual language, distinguishable only by shade. A lamp says something else
        // entirely, and matches the dots already used beside the lane names.
        e.button->setLampOn (active);
    }
}

void BottomTabs::updateLaneTabLabel()
{
    laneTabButton.setButtonText (juce::String ("LANE: ") + BlockGrid::getLaneName (selectedLane));
}

void BottomTabs::setSelectedLane (int laneIndex)
{
    selectedLane = laneIndex;
    laneParamPanel.setLane (laneIndex);
    updateLaneTabLabel();

    if (currentTab == Tab::Lane)
        selectTab (Tab::Lane); // switch to it / refresh highlight if not already focused

    repaint();
}

void BottomTabs::setSceneIndex (int sceneIndex)
{
    modRoutePanel.setSceneIndex (sceneIndex);
}

void BottomTabs::refreshAfterPresetLoad()
{
    // Re-sync the lane panel title/knobs are already handled by attachments; just repaint
    // everything (including hidden tabs, cheaply, since JUCE skips painting invisible components
    // anyway) so that whichever curve/lane view the user is looking at picks up the new state.
    laneParamPanel.repaint();
    volumeCurveEditor.refreshAfterPresetLoad();
    filterCurveEditor.refreshAfterPresetLoad();
    panCurveEditor.refreshAfterPresetLoad();

    // The route table is rows of child components built from the scene's curves, not something
    // paint() derives, so a repaint alone would keep showing the previous preset's routes.
    modRoutePanel.refresh();

    repaint();
}

void BottomTabs::selectTab (Tab t)
{
    currentTab = t;

    laneTabButton.setToggleState (t == Tab::Lane, juce::dontSendNotification);
    volumeTabButton.setToggleState (t == Tab::Volume, juce::dontSendNotification);
    filterTabButton.setToggleState (t == Tab::Filter, juce::dontSendNotification);
    panTabButton.setToggleState (t == Tab::Pan, juce::dontSendNotification);
    modTabButton.setToggleState (t == Tab::Mod, juce::dontSendNotification);

    laneParamPanel.setVisible (t == Tab::Lane);
    volumeCurveEditor.setVisible (t == Tab::Volume);
    filterCurveEditor.setVisible (t == Tab::Filter);
    panCurveEditor.setVisible (t == Tab::Pan);
    modRoutePanel.setVisible (t == Tab::Mod);

    repaint();
}

void BottomTabs::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (Palette::bg1);
    g.fillRect (bounds);

    auto tabStrip = bounds.removeFromTop (30.0f);
    g.setColour (Palette::bg0.withAlpha (0.5f));
    g.fillRect (tabStrip);
    g.setColour (Palette::bg3.withAlpha (0.6f));
    g.fillRect (juce::Rectangle<float> (0.0f, tabStrip.getBottom() - 1.0f, bounds.getWidth(), 1.0f));

}

void BottomTabs::resized()
{
    auto r = getLocalBounds();
    auto tabStrip = r.removeFromTop (30);

    laneTabButton.setBounds (tabStrip.removeFromLeft (180).reduced (2));
    volumeTabButton.setBounds (tabStrip.removeFromLeft (90).reduced (2));
    filterTabButton.setBounds (tabStrip.removeFromLeft (90).reduced (2));
    panTabButton.setBounds (tabStrip.removeFromLeft (90).reduced (2));
    modTabButton.setBounds (tabStrip.removeFromLeft (90).reduced (2));

    laneParamPanel.setBounds (r);
    volumeCurveEditor.setBounds (r);
    filterCurveEditor.setBounds (r);
    panCurveEditor.setBounds (r);
    modRoutePanel.setBounds (r);
}

} // namespace stutter::ui
