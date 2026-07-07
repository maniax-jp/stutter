#include "PluginEditor.h"

namespace
{
constexpr int defaultWidth = 900;
constexpr int defaultHeight = 620;
}

StutterAudioProcessorEditor::StutterAudioProcessorEditor (StutterAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      headerBar (p),
      stepGrid (p),
      bottomTabs (p)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (headerBar);
    addAndMakeVisible (stepGrid);
    addAndMakeVisible (bottomTabs);

    stepGrid.onLaneSelected = [this] (int lane) { bottomTabs.setSelectedLane (lane); };
    bottomTabs.setSelectedLane (stepGrid.getSelectedLane());

    setResizable (true, true);
    setResizeLimits (defaultWidth / 2, defaultHeight / 2, defaultWidth * 2, defaultHeight * 2);
    getConstrainer()->setFixedAspectRatio ((double) defaultWidth / (double) defaultHeight);

    setSize (defaultWidth, defaultHeight);
}

StutterAudioProcessorEditor::~StutterAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void StutterAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (stutter::ui::Palette::bg1);
}

void StutterAudioProcessorEditor::resized()
{
    auto r = getLocalBounds();

    const float scale = (float) getWidth() / (float) defaultWidth;

    headerBar.setBounds (r.removeFromTop ((int) (72.0f * scale)));

    auto bottomHeight = (int) (200.0f * scale);
    bottomTabs.setBounds (r.removeFromBottom (bottomHeight));

    stepGrid.setBounds (r);
}
