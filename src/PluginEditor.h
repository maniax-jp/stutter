#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

/**
    Phase-1 placeholder editor. Wraps JUCE's GenericAudioProcessorEditor so every
    APVTS parameter is inspectable/automatable during DSP development.

    Phase 2 will replace this with the full custom UI (step grid, curve editors,
    custom LookAndFeel) described in SPEC.md, reading/writing the same
    StepSequencer/CurveModulator ValueTree data model exposed via
    StutterAudioProcessor::getSequencer() / getCurve().
*/
class StutterAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit StutterAudioProcessorEditor (StutterAudioProcessor&);
    ~StutterAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // Kept for phase 2 (custom UI will read/write processorRef.getSequencer() / getCurve()).
    [[maybe_unused]] StutterAudioProcessor& processorRef;
    juce::GenericAudioProcessorEditor genericEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StutterAudioProcessorEditor)
};
