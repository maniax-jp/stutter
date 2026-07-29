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
      performanceBar (p, p.getSceneDocument()),
      blockGrid (p, p.getSceneDocument()),
      bottomTabs (p)
{
    setLookAndFeel (&lookAndFeel);

    // Everything lives inside `content`, which is always laid out at defaultWidth x
    // defaultHeight. resized() then scales that one component, so type and controls grow with
    // the window instead of staying put while the boxes around them change size.
    addAndMakeVisible (content);
    content.addAndMakeVisible (headerBar);
    content.addAndMakeVisible (sceneBrowser);
    content.addAndMakeVisible (performanceBar);
    content.addAndMakeVisible (blockGrid);
    content.addAndMakeVisible (bottomTabs);

    blockGrid.onLaneSelected = [this] (int lane) { bottomTabs.setSelectedLane (lane); };

    // Beats/divisions/swing change the grid's shape, so it has to redraw against the new
    // geometry rather than keep painting the old division count.
    performanceBar.onGridGeometryChanged = [this] { blockGrid.repaint(); };

    // Picking a scene in the browser points the grid at it, so the two panes always show the
    // same scene rather than drifting apart.
    sceneBrowser.onSceneSelected = [this] (int sceneIndex)
    {
        blockGrid.setSceneIndex (sceneIndex);
        bottomTabs.setSceneIndex (sceneIndex);
        performanceBar.setSceneIndex (sceneIndex);
    };
    blockGrid.setSceneIndex (sceneBrowser.getSelectedScene());
    bottomTabs.setSceneIndex (sceneBrowser.getSelectedScene());
    performanceBar.setSceneIndex (sceneBrowser.getSelectedScene());
    bottomTabs.setSelectedLane (blockGrid.getSelectedLane());

    // Structural preset data (step grid + curve breakpoints) isn't APVTS-parameter-bound, so it
    // doesn't auto-refresh via attachment listeners the way sliders/combo boxes do -- force a
    // repaint of everything that reads it directly whenever a preset finishes loading.
    processorRef.getPresetManager().onPresetLoaded = [this]
    {
        headerBar.refreshPresetLabel();

        // Point the editor at the scene the preset actually landed on before repainting
        // anything. Reusing the previously selected scene left the grid and knobs showing the
        // old preset until the user happened to click a scene cell.
        sceneBrowser.setSelectedScene (processorRef.getSceneSelector().getActiveScene());

        blockGrid.repaint();
        sceneBrowser.repaint();
        bottomTabs.refreshAfterPresetLoad();

        // Grid geometry and swing are per-scene, so these have to be re-read rather than
        // repainted -- combo boxes show their own state, not the model's.
        performanceBar.setSceneIndex (sceneBrowser.getSelectedScene());
        performanceBar.refresh();
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
    // Lay the panels out at the design size, then scale the whole thing to fit. The aspect
    // ratio is pinned by the constrainer, so one factor covers both axes.
    content.setBounds (0, 0, defaultWidth, defaultHeight);

    const float scale = (float) getWidth() / (float) defaultWidth;
    content.setTransform (juce::AffineTransform::scale (scale));

    auto r = content.getLocalBounds();

    headerBar.setBounds (r.removeFromTop (72));
    sceneBrowser.setBounds (r.removeFromTop (56));
    performanceBar.setBounds (r.removeFromTop (30));
    bottomTabs.setBounds (r.removeFromBottom (220));

    blockGrid.setBounds (r);
}
