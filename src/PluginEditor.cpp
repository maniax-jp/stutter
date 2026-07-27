#include "PluginEditor.h"

namespace
{
constexpr int defaultWidth = 1200;
constexpr int defaultHeight = 800;
}

StutterAudioProcessorEditor::StutterAudioProcessorEditor (StutterAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      headerBar (p),
      sceneBrowser (p, p.getSceneDocument()),
      blockGrid (p, p.getSceneDocument()),
      bottomTabs (p)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (headerBar);
    addAndMakeVisible (sceneBrowser);
    addAndMakeVisible (blockGrid);
    addAndMakeVisible (bottomTabs);

    blockGrid.onLaneSelected = [this] (int lane) { bottomTabs.setSelectedLane (lane); };

    // Picking a scene in the browser points the grid at it, so the two panes always show the
    // same scene rather than drifting apart.
    sceneBrowser.onSceneSelected = [this] (int sceneIndex)
    {
        blockGrid.setSceneIndex (sceneIndex);
        bottomTabs.setSceneIndex (sceneIndex);
    };
    blockGrid.setSceneIndex (sceneBrowser.getSelectedScene());
    bottomTabs.setSceneIndex (sceneBrowser.getSelectedScene());
    bottomTabs.setSelectedLane (blockGrid.getSelectedLane());

    // Structural preset data (step grid + curve breakpoints) isn't APVTS-parameter-bound, so it
    // doesn't auto-refresh via attachment listeners the way sliders/combo boxes do -- force a
    // repaint of everything that reads it directly whenever a preset finishes loading.
    processorRef.getPresetManager().onPresetLoaded = [this]
    {
        headerBar.refreshPresetLabel();
        blockGrid.repaint();
        sceneBrowser.repaint();
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
    // dangling `this`.
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
    sceneBrowser.setBounds (r.removeFromTop ((int) (56.0f * scale)));

    auto bottomHeight = (int) (220.0f * scale);
    bottomTabs.setBounds (r.removeFromBottom (bottomHeight));

    blockGrid.setBounds (r);
}
