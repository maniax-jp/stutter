#include "SceneBrowser.h"
#include "../PluginProcessor.h"

namespace stutter::ui
{

SceneBrowser::SceneBrowser (StutterAudioProcessor& processor, SceneDocument& document)
    : proc (processor), doc (document)
{
    setWantsKeyboardFocus (false);
    // 30Hz: automation can move the scene on any block boundary, and the highlight has to
    // read as instant when it does.
    startTimerHz (30);
}

SceneBrowser::~SceneBrowser() { stopTimer(); }

juce::Rectangle<float> SceneBrowser::getCellBounds (int sceneIndex) const
{
    const int offset = sceneIndex - firstVisibleScene;
    if (offset < 0 || offset >= visibleScenes)
        return {};

    const auto b = getLocalBounds().toFloat();
    const float w = b.getWidth() / (float) visibleScenes;
    return { b.getX() + (float) offset * w, b.getY(), w, b.getHeight() };
}

int SceneBrowser::sceneAtPoint (juce::Point<int> p) const
{
    if (! getLocalBounds().contains (p))
        return -1;

    const float w = (float) getWidth() / (float) visibleScenes;
    const int offset = (int) ((float) p.x / juce::jmax (1.0f, w));
    const int scene = firstVisibleScene + juce::jlimit (0, visibleScenes - 1, offset);
    return juce::jlimit (firstSceneIndex, lastSceneIndex, scene);
}

bool SceneBrowser::sceneHasContent (int sceneIndex) const
{
    // Read the document rather than the baked bank: an empty scene the user has just started
    // editing should light up immediately, without waiting for a publish.
    auto& mutableDoc = const_cast<SceneDocument&> (doc);
    auto scene = mutableDoc.ensureScene (sceneIndex);
    if (! scene.isValid())
        return false;

    auto blocks = scene.getChildWithName (SceneIDs::blocksNode);
    return blocks.isValid() && blocks.getNumChildren() > 0;
}

void SceneBrowser::mouseDown (const juce::MouseEvent& e)
{
    const int scene = sceneAtPoint (e.getPosition());
    if (scene < 0)
        return;

    selectedScene = scene;
    if (onSceneSelected)
        onSceneSelected (selectedScene);

    // Selecting a scene in the browser also makes it the one that plays, so the user hears
    // what they are editing. Routed through the parameter rather than straight into the
    // engine: sceneSelect is the single source of truth for which scene plays, and writing
    // both would leave the UI and the host disagreeing about the current value. The gesture
    // pair is what lets a host in automation-write mode record this click.
    if (auto* p = proc.getAPVTS().getParameter (ID::sceneSelect))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) selectedScene));
        p->endChangeGesture();
    }

    repaint();
}

void SceneBrowser::mouseMove (const juce::MouseEvent& e)
{
    const int scene = sceneAtPoint (e.getPosition());
    if (scene != hoveredScene)
    {
        hoveredScene = scene;
        repaint();
    }
}

void SceneBrowser::mouseExit (const juce::MouseEvent&)
{
    if (hoveredScene != -1)
    {
        hoveredScene = -1;
        repaint();
    }
}

void SceneBrowser::timerCallback()
{
    // Read the engine rather than the parameter: this highlight means "the scene you are
    // hearing", and the engine is what the audio path actually rendered. The two agree
    // within a block, but only the engine is right during that block.
    const int active = proc.getSceneSelector().getActiveScene();
    if (active != lastActiveScene)
    {
        lastActiveScene = active;
        repaint();
    }
}

void SceneBrowser::resized() {}

void SceneBrowser::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bg0);

    const int active = lastActiveScene;

    for (int i = 0; i < visibleScenes; ++i)
    {
        const int scene = firstVisibleScene + i;
        const auto r = getCellBounds (scene).reduced (1.0f, 2.0f);
        if (r.isEmpty())
            continue;

        const bool occupied = sceneHasContent (scene);
        const bool selected = scene == selectedScene;
        const bool playing = scene == active;
        const bool hovered = scene == hoveredScene;

        // Fill carries "has content": scanning for somewhere to build is the most common
        // reason to look at this strip.
        juce::Colour base = occupied ? Palette::accent.withAlpha (0.45f) : Palette::bg2;
        if (hovered)
            base = base.brighter (0.15f);

        g.setColour (base);
        g.fillRoundedRectangle (r, 2.0f);

        // Playing outranks selected visually: while the timeline runs, which scene you are
        // hearing matters more than which one is open for editing.
        if (playing)
        {
            g.setColour (Palette::accent);
            g.drawRoundedRectangle (r, 2.0f, 2.0f);
            g.setColour (Palette::accent.withAlpha (0.25f));
            g.drawRoundedRectangle (r.expanded (1.5f), 3.0f, 1.5f);
        }
        else if (selected)
        {
            g.setColour (Palette::textHi.withAlpha (0.7f));
            g.drawRoundedRectangle (r, 2.0f, 1.0f);
        }

        // Every slot is numbered. This number is what the user types into an automation lane,
        // so having to count along from a marked slot to work one out would make the strip
        // useless for the job it exists to do.
        //
        // Occupied cells are filled with the accent, which is light enough that low-contrast
        // text disappears on it -- so the label follows the fill rather than being one colour
        // throughout.
        g.setColour (occupied ? Palette::bg0.withAlpha (0.75f)
                              : (playing || selected ? Palette::textHi : Palette::textLo));
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.drawText (juce::String (scene), r.reduced (1.0f),
                    juce::Justification::centredBottom, false);
    }
}

} // namespace stutter::ui
