#include "PluginEditor.h"

StutterAudioProcessorEditor::StutterAudioProcessorEditor (StutterAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p), genericEditor (p)
{
    addAndMakeVisible (genericEditor);
    setResizable (true, true);
    setSize (900, 620);
}

StutterAudioProcessorEditor::~StutterAudioProcessorEditor() = default;

void StutterAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff14161c));
}

void StutterAudioProcessorEditor::resized()
{
    genericEditor.setBounds (getLocalBounds());
}
