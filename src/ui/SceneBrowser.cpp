#include "SceneBrowser.h"
#include "../PluginProcessor.h"

namespace stutter::ui
{

SceneBrowser::SceneBrowser (StutterAudioProcessor& processor, SceneDocument& document)
    : proc (processor), doc (document)
{
    setWantsKeyboardFocus (false);
    startTimerHz (20);
}

SceneBrowser::~SceneBrowser() { stopTimer(); }

bool SceneBrowser::isBlackKey (int note) const
{
    const int n = ((note % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

juce::Rectangle<float> SceneBrowser::getKeyBounds (int note) const
{
    const int offset = note - firstVisibleNote;
    if (offset < 0 || offset >= visibleNotes)
        return {};

    const auto b = getLocalBounds().toFloat();
    const float w = b.getWidth() / (float) visibleNotes;
    return { b.getX() + (float) offset * w, b.getY(), w, b.getHeight() };
}

int SceneBrowser::noteAtPoint (juce::Point<int> p) const
{
    if (! getLocalBounds().contains (p))
        return -1;

    const float w = (float) getWidth() / (float) visibleNotes;
    const int offset = (int) ((float) p.x / juce::jmax (1.0f, w));
    const int note = firstVisibleNote + juce::jlimit (0, visibleNotes - 1, offset);
    return juce::jlimit (0, 127, note);
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
    const int note = noteAtPoint (e.getPosition());
    if (note < 0)
        return;

    selectedScene = note;
    if (onSceneSelected)
        onSceneSelected (selectedScene);

    // Selecting a scene in the browser also makes it the one that plays, so the user hears
    // what they are editing without having to send a note.
    proc.getGestureEngine().setActiveScene (selectedScene);
    repaint();
}

void SceneBrowser::mouseMove (const juce::MouseEvent& e)
{
    const int note = noteAtPoint (e.getPosition());
    if (note != hoveredNote)
    {
        hoveredNote = note;
        repaint();
    }
}

void SceneBrowser::mouseExit (const juce::MouseEvent&)
{
    if (hoveredNote != -1)
    {
        hoveredNote = -1;
        repaint();
    }
}

void SceneBrowser::timerCallback()
{
    const int active = proc.getGestureEngine().getActiveScene();
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

    for (int i = 0; i < visibleNotes; ++i)
    {
        const int note = firstVisibleNote + i;
        const auto r = getKeyBounds (note).reduced (1.0f, 2.0f);
        if (r.isEmpty())
            continue;

        const bool black = isBlackKey (note);
        const bool occupied = sceneHasContent (note);
        const bool selected = note == selectedScene;
        const bool playing = note == active;
        const bool hovered = note == hoveredNote;

        // Base key. Black keys sit darker so the octave layout stays readable even when
        // every scene is occupied.
        juce::Colour base = black ? Palette::bg1 : Palette::bg2;
        if (occupied)
            base = Palette::accent.withAlpha (black ? 0.30f : 0.45f);
        if (hovered)
            base = base.brighter (0.15f);

        g.setColour (base);
        g.fillRoundedRectangle (r, 2.0f);

        // The playing scene outranks the selected one visually: during performance, which
        // scene you are hearing matters more than which one is open for editing.
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

        // Label C notes only; anything more is unreadable at this key width.
        if ((note % 12) == 0)
        {
            g.setColour (Palette::textLo);
            g.setFont (juce::Font (juce::FontOptions (9.0f)));
            g.drawText ("C" + juce::String (note / 12 - 1), r.reduced (1.0f),
                        juce::Justification::centredBottom, false);
        }
    }
}

} // namespace stutter::ui
