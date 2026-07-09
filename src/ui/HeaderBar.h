#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"

class StutterAudioProcessor;

namespace stutter::ui
{

/**
    Top bar: logo/title, preset browser placeholder (wired up in phase 3),
    Dry/Wet + Output knobs, sequencer on/off toggle, BPM/sync status readout.
*/
class HeaderBar : public juce::Component, private juce::Timer
{
public:
    explicit HeaderBar (StutterAudioProcessor& processor);
    ~HeaderBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Public so the editor can force an immediate label refresh right after a preset load
        (the timer-driven poll would otherwise catch up within one tick anyway, but this avoids
        even a single-frame stale name flash). */
    void refreshPresetLabel();

private:
    void timerCallback() override;

    void showPresetMenu();
    void showSaveDialog();
    void confirmDeleteUserPreset (int presetIndex);

    StutterAudioProcessor& proc;

    // Logo
    juce::Label titleLabel;
    juce::Label subtitleLabel;

    // Preset browser
    juce::Component presetArea;
    juce::TextButton presetPrevButton { "<" };
    juce::TextButton presetNextButton { ">" };
    juce::TextButton presetNameButton { "Init" };
    juce::TextButton presetSaveButton { "Save" };

    std::unique_ptr<juce::AlertWindow> saveDialog;

    // Knobs
    juce::Slider dryWetKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label dryWetLabel { {}, "DRY/WET" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> dryWetAttachment;

    juce::Slider outputKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label outputLabel { {}, "OUTPUT" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;

    // Sequencer on/off
    juce::ToggleButton seqToggle { "SEQ" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> seqAttachment;

    // Host sync on/off
    juce::ToggleButton syncToggle { "SYNC" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> syncAttachment;

    // BPM / sync status. When host sync is off (FREE / internal clock), the label becomes an
    // editable text field bound to ID::internalBpm (double-click to type a new value, per
    // juce::Label's built-in edit-on-double-click); while host-synced it just shows the
    // live host BPM read-only and dimmed-uneditable.
    juce::Label bpmLabel;
    double lastShownBpm = -1.0;
    bool lastShownSynced = false;
    bool lastShownDirty = false;
    bool bpmLabelBeingEdited = false;

    void updateBpmEditableState (bool hostSynced);
    void bpmLabelTextEdited();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBar)
};

} // namespace stutter::ui
