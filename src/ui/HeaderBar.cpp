#include "HeaderBar.h"
#include "../PluginProcessor.h"
#include "../dsp/ParameterIDs.h"

namespace stutter::ui
{

namespace
{
void styleKnobLabel (juce::Label& l)
{
    l.setJustificationType (juce::Justification::centred);
    l.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::plain)));
    l.setColour (juce::Label::textColourId, Palette::textLo);
}
}

HeaderBar::HeaderBar (StutterAudioProcessor& processor) : proc (processor)
{
    // ---- Logo ----
    titleLabel.setText ("STUTTER", juce::dontSendNotification);
    titleLabel.setFont (StutterLookAndFeel::titleFont (26.0f));
    titleLabel.setColour (juce::Label::textColourId, Palette::textHi);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel);

    subtitleLabel.setText ("MULTI-FX GLITCH SEQUENCER", juce::dontSendNotification);
    subtitleLabel.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::plain)).withExtraKerningFactor (0.18f));
    subtitleLabel.setColour (juce::Label::textColourId, Palette::textLo);
    subtitleLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (subtitleLabel);

    // ---- Preset browser placeholder (phase 3 wires real preset switching) ----
    addAndMakeVisible (presetArea);
    for (auto* b : { &presetPrevButton, &presetNextButton })
    {
        b->setEnabled (false);
        addAndMakeVisible (*b);
    }
    presetNameButton.setEnabled (false);
    presetNameButton.setButtonText ("Init Patch");
    addAndMakeVisible (presetNameButton);
    presetSaveButton.setEnabled (false);
    addAndMakeVisible (presetSaveButton);

    // ---- Dry/Wet knob ----
    dryWetKnob.setColour (juce::Slider::rotarySliderFillColourId, Palette::accent);
    addAndMakeVisible (dryWetKnob);
    styleKnobLabel (dryWetLabel);
    addAndMakeVisible (dryWetLabel);
    dryWetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.getAPVTS(), ID::dryWet, dryWetKnob);

    // ---- Output knob ----
    outputKnob.setColour (juce::Slider::rotarySliderFillColourId, Palette::laneColours[6]);
    addAndMakeVisible (outputKnob);
    styleKnobLabel (outputLabel);
    addAndMakeVisible (outputLabel);
    outputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.getAPVTS(), ID::outputGain, outputKnob);

    // ---- Sequencer toggle ----
    seqToggle.setColour (juce::ToggleButton::textColourId, Palette::textLo);
    addAndMakeVisible (seqToggle);
    seqAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.getAPVTS(), ID::sequencerOn, seqToggle);

    // ---- BPM readout ----
    bpmLabel.setJustificationType (juce::Justification::centredRight);
    bpmLabel.setFont (StutterLookAndFeel::monoFont (13.0f));
    bpmLabel.setColour (juce::Label::textColourId, Palette::textLo);
    addAndMakeVisible (bpmLabel);

    startTimerHz (15);
    timerCallback();
}

HeaderBar::~HeaderBar() { stopTimer(); }

void HeaderBar::timerCallback()
{
    const double bpm = proc.getDisplayBpm();
    const bool synced = proc.isDisplayHostSynced();

    if (std::abs (bpm - lastShownBpm) > 0.05 || synced != lastShownSynced)
    {
        lastShownBpm = bpm;
        lastShownSynced = synced;

        juce::String text = juce::String (bpm, 1) + " BPM";
        text << (synced ? "  ● SYNC" : "  ○ FREE");
        bpmLabel.setText (text, juce::dontSendNotification);
        bpmLabel.setColour (juce::Label::textColourId, synced ? Palette::accent.withAlpha (0.9f) : Palette::textLo);
    }
}

void HeaderBar::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (Palette::bg0);
    g.fillRect (bounds);

    g.setColour (Palette::bg3.withAlpha (0.6f));
    g.fillRect (bounds.removeFromBottom (1.0f));

    // preset area background
    auto pa = presetArea.getBounds().toFloat();
    g.setColour (Palette::bg2);
    g.fillRoundedRectangle (pa, 6.0f);
    g.setColour (Palette::bg3);
    g.drawRoundedRectangle (pa, 6.0f, 1.0f);
}

void HeaderBar::resized()
{
    auto r = getLocalBounds().reduced (14, 8);

    // Logo block on the left
    auto logoArea = r.removeFromLeft (190);
    titleLabel.setBounds (logoArea.removeFromTop (30));
    subtitleLabel.setBounds (logoArea.removeFromTop (14));

    r.removeFromLeft (18);

    // BPM readout + sequencer toggle on the far right
    auto rightArea = r.removeFromRight (150);
    bpmLabel.setBounds (rightArea.removeFromTop (18));
    seqToggle.setBounds (rightArea.withSizeKeepingCentre (rightArea.getWidth(), 22).translated (0, 6));

    r.removeFromRight (14);

    // Output knob
    auto outputArea = r.removeFromRight (60);
    outputLabel.setBounds (outputArea.removeFromBottom (13));
    outputKnob.setBounds (outputArea.withSizeKeepingCentre (52, 52));

    r.removeFromRight (10);

    // Dry/Wet knob
    auto dwArea = r.removeFromRight (60);
    dryWetLabel.setBounds (dwArea.removeFromBottom (13));
    dryWetKnob.setBounds (dwArea.withSizeKeepingCentre (52, 52));

    r.removeFromRight (18);

    // Remaining central space -> preset browser placeholder
    presetArea.setBounds (r);
    auto pr = r.reduced (4);
    presetPrevButton.setBounds (pr.removeFromLeft (26));
    presetSaveButton.setBounds (pr.removeFromRight (52));
    pr.removeFromRight (6);
    presetNextButton.setBounds (pr.removeFromRight (26));
    pr.removeFromLeft (4);
    pr.removeFromRight (4);
    presetNameButton.setBounds (pr);
}

} // namespace stutter::ui
