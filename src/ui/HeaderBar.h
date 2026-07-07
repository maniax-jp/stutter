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

private:
    void timerCallback() override;

    StutterAudioProcessor& proc;

    // Logo
    juce::Label titleLabel;
    juce::Label subtitleLabel;

    // Preset browser placeholder (phase 3 will wire this to a real preset manager)
    juce::Component presetArea;
    juce::TextButton presetPrevButton { "<" };
    juce::TextButton presetNextButton { ">" };
    juce::TextButton presetNameButton { "Init" };
    juce::TextButton presetSaveButton { "Save" };

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

    // BPM / sync status
    juce::Label bpmLabel;
    double lastShownBpm = -1.0;
    bool lastShownSynced = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBar)
};

} // namespace stutter::ui
