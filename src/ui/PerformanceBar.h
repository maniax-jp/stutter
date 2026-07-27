#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"
#include "../state/SceneDocument.h"

class StutterAudioProcessor;

namespace stutter::ui
{

/**
    The MIDI performance controls: Play Mode, Scene Lock, trigger quantize, and the active
    scene's Release mode.

    These sit beside the keyboard strip rather than in the header because they change what
    playing a note *does*, and the keyboard is what the user is looking at while deciding.

    Play Mode / Scene Lock / Quantize are global and live in the state tree, not in APVTS.
    They are modal rather than continuous -- switching Play Mode mid-phrase changes whether
    notes gate at all -- so exposing them as automatable parameters would invite hosts to
    produce note handling no one asked for. Release mode is per-scene, so it is written to
    the scene document and follows whichever scene the editor is showing.
*/
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

private:
    void timerCallback() override;
    void pushReleaseModeToScene();

    StutterAudioProcessor& proc;
    SceneDocument& doc;
    int sceneIndex = 0;

    juce::Label playModeLabel { {}, "PLAY" };
    juce::ComboBox playModeBox;

    juce::Label quantizeLabel { {}, "QUANT" };
    juce::ComboBox quantizeBox;

    juce::Label releaseLabel { {}, "RELEASE" };
    juce::ComboBox releaseBox;

    juce::ToggleButton sceneLockToggle { "LOCK" };

    // Guards the combo callbacks while refresh() writes into them, so restoring the UI from
    // the model is not mistaken for the user editing it and written straight back.
    bool updatingFromModel = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerformanceBar)
};

} // namespace stutter::ui
