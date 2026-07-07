#include "LaneParamPanel.h"
#include "StepGrid.h"
#include "../PluginProcessor.h"
#include "../dsp/ParameterIDs.h"

namespace stutter::ui
{

LaneParamPanel::LaneParamPanel (StutterAudioProcessor& processor) : proc (processor)
{
    laneTitle.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    laneTitle.setColour (juce::Label::textColourId, Palette::textHi);
    addAndMakeVisible (laneTitle);

    rebuildForLane (0);
}

LaneParamPanel::~LaneParamPanel() = default;

void LaneParamPanel::setLane (int laneIndex)
{
    if (laneIndex == currentLane)
        return;
    rebuildForLane (laneIndex);
}

void LaneParamPanel::addKnob (const juce::String& paramId, const juce::String& displayName, juce::Colour accent)
{
    auto ctrl = std::make_unique<KnobControl>();
    ctrl->slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
    ctrl->slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 16);
    ctrl->slider->setColour (juce::Slider::rotarySliderFillColourId, accent);
    ctrl->slider->setColour (juce::Slider::textBoxTextColourId, Palette::textLo);
    addAndMakeVisible (*ctrl->slider);

    ctrl->label = std::make_unique<juce::Label>();
    ctrl->label->setText (displayName, juce::dontSendNotification);
    ctrl->label->setJustificationType (juce::Justification::centred);
    ctrl->label->setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::plain)));
    ctrl->label->setColour (juce::Label::textColourId, Palette::textLo);
    addAndMakeVisible (*ctrl->label);

    ctrl->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.getAPVTS(), paramId, *ctrl->slider);

    // The attachment installs its own textFromValueFunction (via RangedAudioParameter::getText),
    // which overrides Slider's numDecimalPlacesToDisplay. Re-install our own formatting on top so
    // knob readouts show a sane, consistent number of decimals instead of raw 7-digit floats.
    if (auto* p = proc.getAPVTS().getParameter (paramId))
    {
        const auto range = p->getNormalisableRange();
        const float span = range.end - range.start;
        const int decimals = span > 50.0f ? 0 : (span > 4.0f ? 1 : 2);
        auto* slider = ctrl->slider.get();
        slider->textFromValueFunction = [decimals] (double v) { return juce::String (v, decimals); };
        slider->valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue(); };
        slider->updateText();
    }

    controls.push_back (std::move (ctrl));
}

void LaneParamPanel::addChoice (const juce::String& paramId, const juce::String& displayName, juce::Colour accent)
{
    auto ctrl = std::make_unique<KnobControl>();
    ctrl->comboBox = std::make_unique<juce::ComboBox>();
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (proc.getAPVTS().getParameter (paramId)))
    {
        int idx = 1;
        for (auto& choice : p->choices)
            ctrl->comboBox->addItem (choice, idx++);
    }
    ctrl->comboBox->setColour (juce::ComboBox::textColourId, accent);
    addAndMakeVisible (*ctrl->comboBox);

    ctrl->label = std::make_unique<juce::Label>();
    ctrl->label->setText (displayName, juce::dontSendNotification);
    ctrl->label->setJustificationType (juce::Justification::centred);
    ctrl->label->setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::plain)));
    ctrl->label->setColour (juce::Label::textColourId, Palette::textLo);
    addAndMakeVisible (*ctrl->label);

    ctrl->comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.getAPVTS(), paramId, *ctrl->comboBox);

    controls.push_back (std::move (ctrl));
}

void LaneParamPanel::rebuildForLane (int laneIndex)
{
    currentLane = laneIndex;
    controls.clear();

    laneTitle.setText (juce::String (StepGrid::getLaneName (laneIndex)), juce::dontSendNotification);
    const auto accent = Palette::laneColours[(size_t) juce::jlimit (0, 7, laneIndex)];
    laneTitle.setColour (juce::Label::textColourId, accent);

    const auto lp = [laneIndex] (const juce::String& name) { return ID::lanePrefix (laneIndex) + name; };

    switch (laneIndex)
    {
        case 0: // Stutter
            addChoice (lp (ID::stutterRate), "RATE", accent);
            addKnob (lp (ID::stutterDecay), "DECAY", accent);
            addKnob (lp (ID::stutterPitchSlide), "PITCH SLIDE", accent);
            break;
        case 1: // TapeStop
            addKnob (lp (ID::tapeStopCurve), "CURVE", accent);
            addKnob (lp (ID::tapeStopTime), "TIME", accent);
            break;
        case 2: // TapeStart
            addKnob (lp (ID::tapeStartCurve), "CURVE", accent);
            addKnob (lp (ID::tapeStartTime), "TIME", accent);
            break;
        case 3: // Reverse
            addChoice (lp (ID::reverseSliceLen), "SLICE LEN", accent);
            break;
        case 4: // Repitch
            addKnob (lp (ID::repitchSemitones), "SEMITONES", accent);
            addKnob (lp (ID::repitchSlide), "SLIDE", accent);
            break;
        case 5: // Gate
            addChoice (lp (ID::gateRate), "RATE", accent);
            addKnob (lp (ID::gateDuty), "DUTY", accent);
            addKnob (lp (ID::gateShape), "SHAPE", accent);
            break;
        case 6: // Filter
            addChoice (lp (ID::filterType), "TYPE", accent);
            addKnob (lp (ID::filterCutoff), "CUTOFF", accent);
            addKnob (lp (ID::filterResonance), "RESONANCE", accent);
            addChoice (lp (ID::filterLfoRate), "LFO RATE", accent);
            addKnob (lp (ID::filterLfoDepth), "LFO DEPTH", accent);
            break;
        case 7: // Crush
            addKnob (lp (ID::crushBitDepth), "BIT DEPTH", accent);
            addKnob (lp (ID::crushRateDiv), "RATE DIV", accent);
            break;
        default:
            break;
    }

    resized();
    repaint();
}

void LaneParamPanel::paint (juce::Graphics& g)
{
    g.setColour (Palette::bg1);
    g.fillAll();
}

void LaneParamPanel::resized()
{
    auto r = getLocalBounds().reduced (16, 10);
    auto titleArea = r.removeFromLeft (140);
    laneTitle.setBounds (titleArea.withSizeKeepingCentre (titleArea.getWidth(), 20));
    r.removeFromLeft (10);

    const int slotW = 96;
    for (auto& ctrl : controls)
    {
        auto slot = r.removeFromLeft (slotW);
        auto labelSlot = slot.removeFromBottom (16);

        if (ctrl->slider != nullptr)
            ctrl->slider->setBounds (slot.reduced (6, 0));
        else if (ctrl->comboBox != nullptr)
            ctrl->comboBox->setBounds (slot.withSizeKeepingCentre (slot.getWidth() - 12, 24));

        if (ctrl->label != nullptr)
            ctrl->label->setBounds (labelSlot);

        r.removeFromLeft (6);
    }
}

} // namespace stutter::ui
