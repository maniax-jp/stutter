#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"
#include "../state/SceneDocument.h"

class StutterAudioProcessor;

namespace stutter::ui
{

/**
    Strip of scene slots: which are built, which is being edited, which is being heard.

    Two highlights, and they mean different things. *Playing* follows the Scene parameter, so
    it moves on its own while automation runs; *selected* is what the editor below is showing
    and only moves when clicked. Keeping them separate is what lets someone edit one scene
    while the timeline plays another, and playing outranks selected visually because during
    playback the scene you are hearing is the one you need to find.

    Only 24 slots are visible at a time. All 128 exist, but a strip showing them all would give
    each about four pixels, and in practice a bank occupies a handful of adjacent slots.
*/
class SceneBrowser : public juce::Component, private juce::Timer
{
public:
    /** The scene the browser opens on. Shared with the sceneSelect parameter's default so the
        browser cannot open showing a scene other than the one being heard. */
    static constexpr int defaultScene = defaultSceneIndex;

    SceneBrowser (StutterAudioProcessor& processor, SceneDocument& document);
    ~SceneBrowser() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    /** Fired when the scene being edited changes, however it changed. */
    std::function<void (int sceneIndex)> onSceneSelected;

    int getSelectedScene() const noexcept { return selectedScene; }

    /** Point the editor at a scene without a click -- used when automation or a preset load
        moves the scene, so the grid and knobs below follow what is actually being heard
        instead of staying on whatever was last clicked. Fires onSceneSelected. */
    void setSelectedScene (int sceneIndex);

private:
    void timerCallback() override;

    /** Scene under a point, or -1. */
    int sceneAtPoint (juce::Point<int> p) const;
    juce::Rectangle<float> getCellBounds (int sceneIndex) const;
    bool sceneHasContent (int sceneIndex) const;

    StutterAudioProcessor& proc;
    SceneDocument& doc;

    int selectedScene = defaultScene;
    int hoveredScene = -1;
    int lastActiveScene = -1;

    /** Leftmost visible slot. Starts at the default scene so a freshly loaded factory bank,
        which targets that slot and the ones just above it, opens in view. */
    int firstVisibleScene = defaultScene;
    static constexpr int visibleScenes = 24;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SceneBrowser)
};

} // namespace stutter::ui
