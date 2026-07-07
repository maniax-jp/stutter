#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StutterLookAndFeel.h"

class StutterAudioProcessor;

namespace stutter::ui
{

/**
    Bottom "LANE" tab content: shows the parameters for whichever lane is
    currently selected in the StepGrid. Rebuilds its knob set when the
    selected lane changes (each lane has a different parameter set).
*/
class LaneParamPanel : public juce::Component
{
public:
    explicit LaneParamPanel (StutterAudioProcessor& processor);
    ~LaneParamPanel() override;

    void setLane (int laneIndex);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct KnobControl
    {
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
        std::unique_ptr<juce::ComboBox> comboBox;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
    };

    void rebuildForLane (int laneIndex);
    void addKnob (const juce::String& paramId, const juce::String& displayName, juce::Colour accent);
    void addChoice (const juce::String& paramId, const juce::String& displayName, juce::Colour accent);

    StutterAudioProcessor& proc;
    int currentLane = -1;
    juce::Label laneTitle;

    std::vector<std::unique_ptr<KnobControl>> controls;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LaneParamPanel)
};

} // namespace stutter::ui
