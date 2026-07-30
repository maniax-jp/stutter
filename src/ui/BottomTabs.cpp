#include "BottomTabs.h"
#include "BlockGrid.h"
#include "../PluginProcessor.h"
#include "../dsp/ParamIndex.h"

namespace stutter::ui
{

namespace
{
/** One accent per shaping curve, used for both its editor (ON button, plotted line) and its
    tab lamp. Named here rather than written at each use so the strip and the panel cannot end
    up disagreeing about what colour a given curve is. */
inline juce::Colour volumeAccent() { return Palette::accent; }
inline juce::Colour filterAccent() { return Palette::laneColour (6); }
inline juce::Colour panAccent()    { return Palette::laneColour (3); }
}

BottomTabs::BottomTabs (StutterAudioProcessor& processor)
    : proc (processor),
      laneParamPanel (processor),
      volumeCurveEditor (processor, stutter::ModTarget::Volume, volumeAccent()),
      filterCurveEditor (processor, stutter::ModTarget::Filter, filterAccent()),
      panCurveEditor (processor, stutter::ModTarget::Pan, panAccent()),
      modRoutePanel (processor, processor.getSceneDocument())
{
    volumeTabButton.setLampColour (volumeAccent());
    filterTabButton.setLampColour (filterAccent());
    panTabButton.setLampColour (panAccent());
    // MOD has no curve of its own; the neutral accent keeps it from impersonating one of the
    // three shapers.
    modTabButton.setLampColour (Palette::accent);

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
    struct Entry { ModTarget target; CurveTabButton* button; };
    const Entry entries[] = {
        { ModTarget::Volume, &volumeTabButton },
        { ModTarget::Filter, &filterTabButton },
        { ModTarget::Pan,    &panTabButton },
    };

    // The lamp mirrors the tab's own ON switch, nothing else.
    //
    // It used to light on "the shape departs from neutral somewhere" instead, which is a
    // different proposition and disagreed with the switch in both directions: Init has all
    // three curves ON but flat, so no lamp lit even though every switch said ON, and a curve
    // switched OFF with a shape still drawn in it lit a lamp while contributing nothing. The
    // ON switch is the thing the user set and the thing the audio path actually tests
    // (applyGlobalModulators short-circuits on isEnabled), so it is what the lamp reports.
    for (const auto& e : entries)
        e.button->setLampOn (proc.getCurve (e.target).isEnabled());

    // MOD earns a lamp on the same terms: at least one route in this scene is enabled and
    // pointed somewhere. Without it the only way to know a scene carries modulation was to
    // open the tab, so a patch could be audibly modulated with nothing on screen saying so.
    modTabButton.setLampOn (countActiveModRoutes() > 0);
}

int BottomTabs::countActiveModRoutes() const
{
    // Read-only throughout: this runs eight times a second off a timer, and ensureScene()
    // here would materialise a scene for every slot the user merely visits -- the same
    // mistake documented on SceneDocument::findScene.
    const auto scene = proc.getSceneDocument().findScene (modScene);
    if (! scene.isValid())
        return 0;

    const auto curves = scene.getChildWithName (SceneIDs::curvesNode);
    if (! curves.isValid())
        return 0;

    int active = 0;
    for (int i = 0; i < curves.getNumChildren(); ++i)
    {
        const auto c = curves.getChild (i);
        if ((bool) c.getProperty (SceneIDs::enabled, false)
            && isValidParamIndex ((int) c.getProperty (SceneIDs::target, -1)))
            ++active;
    }
    return active;
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
    modScene = sceneIndex;
    modRoutePanel.setSceneIndex (sceneIndex);
    refreshCurveTabIndicators(); // the MOD lamp is per-scene, so it has to follow immediately
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
