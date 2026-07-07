#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "ui/StutterLookAndFeel.h"
#include "ui/HeaderBar.h"
#include "ui/StepGrid.h"
#include "ui/BottomTabs.h"

/**
    Phase-2 custom editor: dark-themed, 900x620 (resizable, fixed aspect ratio).

    Layout:
      - HeaderBar   (logo, preset browser placeholder, dry/wet + output knobs, seq toggle, BPM)
      - StepGrid    (8 lanes x 16 steps, live playhead, drag-paint, lane selection)
      - BottomTabs  (LANE params for the selected lane / VOLUME / FILTER / PAN curve editors)
*/
class StutterAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit StutterAudioProcessorEditor (StutterAudioProcessor&);
    ~StutterAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    StutterAudioProcessor& processorRef;

    stutter::ui::StutterLookAndFeel lookAndFeel;

    stutter::ui::HeaderBar headerBar;
    stutter::ui::StepGrid stepGrid;
    stutter::ui::BottomTabs bottomTabs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StutterAudioProcessorEditor)
};
