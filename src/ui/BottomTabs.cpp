#include "BottomTabs.h"
#include "StepGrid.h"
#include "../PluginProcessor.h"

namespace stutter::ui
{

BottomTabs::BottomTabs (StutterAudioProcessor& processor)
    : proc (processor),
      laneParamPanel (processor),
      volumeCurveEditor (processor, stutter::ModTarget::Volume, Palette::accent),
      filterCurveEditor (processor, stutter::ModTarget::Filter, Palette::laneColours[6]),
      panCurveEditor (processor, stutter::ModTarget::Pan, Palette::laneColours[3])
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

    addChildComponent (laneParamPanel);
    addChildComponent (volumeCurveEditor);
    addChildComponent (filterCurveEditor);
    addChildComponent (panCurveEditor);

    selectTab (Tab::Lane);
}

void BottomTabs::updateLaneTabLabel()
{
    laneTabButton.setButtonText (juce::String ("LANE: ") + StepGrid::getLaneName (selectedLane));
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

void BottomTabs::selectTab (Tab t)
{
    currentTab = t;

    laneTabButton.setToggleState (t == Tab::Lane, juce::dontSendNotification);
    volumeTabButton.setToggleState (t == Tab::Volume, juce::dontSendNotification);
    filterTabButton.setToggleState (t == Tab::Filter, juce::dontSendNotification);
    panTabButton.setToggleState (t == Tab::Pan, juce::dontSendNotification);

    laneParamPanel.setVisible (t == Tab::Lane);
    volumeCurveEditor.setVisible (t == Tab::Volume);
    filterCurveEditor.setVisible (t == Tab::Filter);
    panCurveEditor.setVisible (t == Tab::Pan);

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

    laneParamPanel.setBounds (r);
    volumeCurveEditor.setBounds (r);
    filterCurveEditor.setBounds (r);
    panCurveEditor.setBounds (r);
}

} // namespace stutter::ui
