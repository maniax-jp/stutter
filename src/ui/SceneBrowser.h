#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"
#include "../state/SceneDocument.h"

class StutterAudioProcessor;

namespace stutter::ui
{

/**
    Keyboard strip showing which notes map to which scenes.

    Drawn rather than built on juce::MidiKeyboardComponent, because the thing being shown is
    scene occupancy, not a playable keyboard: each key needs an occupied/empty state, the
    active scene highlighted live from the audio thread, and a click that selects for editing
    rather than sounds a note.

    Only two octaves are visible at a time. All 128 scenes exist, but a strip showing them
    all would give each key about four pixels, and in practice a bank occupies a handful of
    adjacent notes -- Stutter Edit 2 has the same problem and solves it the same way, with a
    scrollable window over the full range.
*/
class SceneBrowser : public juce::Component, private juce::Timer
{
public:
    SceneBrowser (StutterAudioProcessor& processor, SceneDocument& document);
    ~SceneBrowser() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** Fired when the user picks a scene to edit. */
    std::function<void (int sceneIndex)> onSceneSelected;

    int getSelectedScene() const noexcept { return selectedScene; }

private:
    void timerCallback() override;

    /** Note under a point, or -1. */
    int noteAtPoint (juce::Point<int> p) const;
    juce::Rectangle<float> getKeyBounds (int note) const;
    bool isBlackKey (int note) const;
    bool sceneHasContent (int sceneIndex) const;

    StutterAudioProcessor& proc;
    SceneDocument& doc;

    int selectedScene = 60;
    int hoveredNote = -1;
    int lastActiveScene = -1;

    /** Lowest visible note. C4 by default, so the default 1:1 mapping puts middle C at the
        left edge and the factory Playable Set lands in view. */
    int firstVisibleNote = 60;
    static constexpr int visibleNotes = 24;   // two octaves

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SceneBrowser)
};

} // namespace stutter::ui
