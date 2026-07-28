#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"
#include "../state/SceneDocument.h"

class StutterAudioProcessor;

namespace stutter::ui
{

/**
    ACTIVE, and the per-scene grid geometry: beats x divisions and swing.

    ACTIVE leads the bar because it is both a control and a readout. The user clicks it to
    audition, and the host's automation moves it while they watch -- so it is drawn from the
    parameter rather than from its own toggle state, and repainted on a timer so an automated
    change is visible the moment it happens.

    The grid controls sit here rather than in the header because changing beats or divisions
    re-times the pattern under the blocks the user is looking at.
*/
/**
    The ACTIVE indicator: a button that is also a live readout.

    It has to work in both directions at once. The user clicks it to audition, and the host's
    automation lane moves it while they watch -- so it draws from the parameter every frame
    rather than from its own toggle state. A plain ToggleButton would only show what the last
    click did, and during playback that is exactly the wrong thing to trust.
*/
class ActiveIndicator : public juce::Button
{
public:
    ActiveIndicator() : juce::Button ("ACTIVE") { setClickingTogglesState (true); }

    void paintButton (juce::Graphics&, bool shouldDrawHighlighted, bool shouldDrawDown) override;
};

class PerformanceBar : public juce::Component, private juce::Timer
{
public:
    PerformanceBar (StutterAudioProcessor& processor, SceneDocument& document);
    ~PerformanceBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Point the Release control at the scene the rest of the editor is showing. */
    void setSceneIndex (int sceneIndex);

    /** Pull every control back from the model, after a preset load replaces it wholesale. */
    void refresh();

    /** Fired when beats/divisions/swing change, so the grid can redraw against the new
        geometry -- it caches nothing, but it only repaints when told to. */
    std::function<void()> onGridGeometryChanged;

private:
    void timerCallback() override;

    StutterAudioProcessor& proc;
    SceneDocument& doc;
    int sceneIndex = 0;

    // Where the effect is heard. The attachment carries clicks to the host and host moves
    // back to the button; the timer below repaints so an automated change is visible the
    // moment it happens rather than whenever the attachment's async update lands.
    ActiveIndicator activeToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> activeAttachment;
    bool lastActiveState = true;

    // Per-scene grid geometry. Changing these re-times the pattern rather than moving blocks:
    // block positions are stored in divisions, so the same arrangement re-reads against the
    // new grid instead of being rewritten.
    juce::Label gridLabel { {}, "GRID" };
    juce::ComboBox beatsBox;
    juce::Label gridTimesLabel { {}, "x" };
    juce::ComboBox divisionsBox;

    juce::Label swingLabel { {}, "SWING" };
    juce::Slider swingSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    // Guards the combo callbacks while refresh() writes into them, so restoring the UI from
    // the model is not mistaken for the user editing it and written straight back.
    bool updatingFromModel = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerformanceBar)
};

} // namespace stutter::ui
