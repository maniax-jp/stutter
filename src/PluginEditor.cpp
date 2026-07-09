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

    // Structural preset data (step grid + curve breakpoints) isn't APVTS-parameter-bound, so it
    // doesn't auto-refresh via attachment listeners the way sliders/combo boxes do -- force a
    // repaint of everything that reads it directly whenever a preset finishes loading.
    processorRef.getPresetManager().onPresetLoaded = [this]
    {
        headerBar.refreshPresetLabel();
        stepGrid.repaint();
        bottomTabs.refreshAfterPresetLoad();
    };

    setResizable (true, true);
    setResizeLimits (defaultWidth / 2, defaultHeight / 2, defaultWidth * 2, defaultHeight * 2);
    getConstrainer()->setFixedAspectRatio ((double) defaultWidth / (double) defaultHeight);

    setSize (defaultWidth, defaultHeight);
}

StutterAudioProcessorEditor::~StutterAudioProcessorEditor()
{
    // The processor (and its PresetManager) can outlive this editor -- the host is free to
    // destroy/recreate the editor at any time while the processor stays alive. Clear the
    // callback so a preset load that happens after this editor is gone never invokes a
    // dangling `this` (see docs/ISSUES.md 3.3).
    processorRef.getPresetManager().onPresetLoaded = nullptr;

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
