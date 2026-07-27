#include "PerformanceBar.h"
#include "../PluginProcessor.h"
#include "../state/SceneSchema.h"

namespace stutter::ui
{

namespace
{
void styleCaption (juce::Label& l)
{
    l.setJustificationType (juce::Justification::centredLeft);
    l.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::plain)).withExtraKerningFactor (0.12f));
    l.setColour (juce::Label::textColourId, Palette::textLo);
}

/** Quantize grid choices, in quarter notes. 0 means "fire immediately". */
struct QuantizeChoice { const char* name; double ppq; };

const QuantizeChoice quantizeChoices[] = {
    { "Off",  0.0  },
    { "1/16", 0.25 },
    { "1/8",  0.5  },
    { "1/4",  1.0  },
    { "1/2",  2.0  },
    { "1 Bar", 4.0 },
};

constexpr int numQuantizeChoices = (int) (sizeof (quantizeChoices) / sizeof (quantizeChoices[0]));
} // namespace

PerformanceBar::PerformanceBar (StutterAudioProcessor& processor, SceneDocument& document)
    : proc (processor), doc (document)
{
    styleCaption (playModeLabel);
    addAndMakeVisible (playModeLabel);

    playModeBox.addItem ("Auto", 1);
    playModeBox.addItem ("MIDI", 2);
    playModeBox.setTooltip ("Auto: the scene runs continuously and notes only pick which one.\n"
                            "MIDI: you hear the effect only while a note is held.");
    playModeBox.onChange = [this]
    {
        if (updatingFromModel)
            return;
        proc.getGestureEngine().setPlayMode (playModeBox.getSelectedId() == 2 ? PlayMode::Midi
                                                                              : PlayMode::Auto);
        proc.getPresetManager().markDirty();
    };
    addAndMakeVisible (playModeBox);

    styleCaption (quantizeLabel);
    addAndMakeVisible (quantizeLabel);

    for (int i = 0; i < numQuantizeChoices; ++i)
        quantizeBox.addItem (quantizeChoices[i].name, i + 1);
    quantizeBox.setTooltip ("Delay an incoming note to the next musical boundary.\n"
                            "A note played slightly early still lands on the boundary it was aiming for.");
    quantizeBox.onChange = [this]
    {
        if (updatingFromModel)
            return;
        const int idx = juce::jlimit (0, numQuantizeChoices - 1, quantizeBox.getSelectedId() - 1);
        proc.getGestureEngine().setTriggerQuantize (quantizeChoices[idx].ppq);
        proc.getPresetManager().markDirty();
    };
    addAndMakeVisible (quantizeBox);

    styleCaption (releaseLabel);
    addAndMakeVisible (releaseLabel);

    // Order must match ReleaseMode's enumerators; the item id is the enum value + 1.
    releaseBox.addItem ("On Grid", 1);
    releaseBox.addItem ("Full", 2);
    releaseBox.addItem ("Latch", 3);
    releaseBox.addItem ("Instant", 4);
    releaseBox.addItem ("Stick", 5);
    releaseBox.setTooltip ("What happens when you let go of a note.\n"
                           "Stored per scene, so each key can behave differently.");
    releaseBox.onChange = [this]
    {
        if (updatingFromModel)
            return;
        pushReleaseModeToScene();
    };
    addAndMakeVisible (releaseBox);

    styleCaption (gridLabel);
    addAndMakeVisible (gridLabel);

    for (int b = 1; b <= 8; ++b)
        beatsBox.addItem (juce::String (b), b);
    beatsBox.setTooltip ("Beats in the pattern. The grid gets longer; the blocks already on it "
                         "keep their positions.");
    beatsBox.onChange = [this]
    {
        if (updatingFromModel)
            return;
        doc.getUndoManager().beginNewTransaction();
        doc.setBeats (sceneIndex, beatsBox.getSelectedId());
        doc.publish();
        proc.getPresetManager().markDirty();
        if (onGridGeometryChanged) onGridGeometryChanged();
    };
    addAndMakeVisible (beatsBox);

    styleCaption (gridTimesLabel);
    gridTimesLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (gridTimesLabel);

    // 2..8: 3 and 6 give triplets and 5/7 give the odd grids, without a separate rate table.
    for (int d = 2; d <= 8; ++d)
        divisionsBox.addItem (juce::String (d), d);
    divisionsBox.setTooltip ("Steps per beat. 3 or 6 gives triplets, 5 or 7 gives odd grids.");
    divisionsBox.onChange = [this]
    {
        if (updatingFromModel)
            return;
        doc.getUndoManager().beginNewTransaction();
        doc.setDivisions (sceneIndex, divisionsBox.getSelectedId());
        doc.publish();
        proc.getPresetManager().markDirty();
        if (onGridGeometryChanged) onGridGeometryChanged();
    };
    addAndMakeVisible (divisionsBox);

    styleCaption (swingLabel);
    addAndMakeVisible (swingLabel);

    swingSlider.setRange (-1.0, 1.0, 0.01);
    swingSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 18);
    swingSlider.setTooltip ("Pushes every other step late (or early, below zero). The pattern "
                            "keeps its length -- this is groove, not a tempo change.");
    swingSlider.onValueChange = [this]
    {
        if (updatingFromModel)
            return;
        doc.setSwing (sceneIndex, (float) swingSlider.getValue());
        doc.publish();
        proc.getPresetManager().markDirty();
        if (onGridGeometryChanged) onGridGeometryChanged();
    };
    // One undo entry per drag, not one per pixel.
    swingSlider.onDragStart = [this] { doc.getUndoManager().beginNewTransaction(); };
    addAndMakeVisible (swingSlider);

    sceneLockToggle.setTooltip ("Notes stop choosing scenes and only gate, so you can edit one "
                                "scene while a track plays notes that would otherwise steal focus.");
    sceneLockToggle.onClick = [this]
    {
        if (updatingFromModel)
            return;
        proc.getGestureEngine().setSceneLock (sceneLockToggle.getToggleState());
        proc.getPresetManager().markDirty();
    };
    addAndMakeVisible (sceneLockToggle);

    refresh();

    // Play Mode and Scene Lock can also be changed by a state load, so poll rather than
    // assuming this bar is the only writer.
    startTimerHz (8);
}

PerformanceBar::~PerformanceBar() { stopTimer(); }

void PerformanceBar::setSceneIndex (int newSceneIndex)
{
    if (newSceneIndex == sceneIndex)
        return;
    sceneIndex = newSceneIndex;
    refresh();
}

void PerformanceBar::pushReleaseModeToScene()
{
    auto scene = doc.ensureScene (sceneIndex);
    if (! scene.isValid())
        return;

    const int mode = juce::jlimit (0, 4, releaseBox.getSelectedId() - 1);
    scene.setProperty (SceneIDs::releaseMode, mode, &doc.getUndoManager());
    doc.publish();
    proc.getPresetManager().markDirty();
}

void PerformanceBar::refresh()
{
    const juce::ScopedValueSetter<bool> guard (updatingFromModel, true);

    auto& engine = proc.getGestureEngine();

    playModeBox.setSelectedId (engine.getPlayMode() == PlayMode::Midi ? 2 : 1,
                               juce::dontSendNotification);
    sceneLockToggle.setToggleState (engine.isSceneLocked(), juce::dontSendNotification);

    // Nearest match rather than exact: the stored value is a double, and a preset written by
    // a future build could carry a grid this list does not offer.
    const double q = engine.getTriggerQuantize();
    int best = 0;
    for (int i = 1; i < numQuantizeChoices; ++i)
        if (std::abs (quantizeChoices[i].ppq - q) < std::abs (quantizeChoices[best].ppq - q))
            best = i;
    quantizeBox.setSelectedId (best + 1, juce::dontSendNotification);

    auto scene = doc.ensureScene (sceneIndex);
    const int mode = scene.isValid()
                       ? juce::jlimit (0, 4, (int) scene.getProperty (SceneIDs::releaseMode, 0))
                       : 0;
    releaseBox.setSelectedId (mode + 1, juce::dontSendNotification);

    beatsBox.setSelectedId (juce::jlimit (1, 8, doc.getBeats (sceneIndex)),
                            juce::dontSendNotification);
    divisionsBox.setSelectedId (juce::jlimit (2, 8, doc.getDivisions (sceneIndex)),
                                juce::dontSendNotification);

    const float swing = scene.isValid() ? (float) scene.getProperty (SceneIDs::swing, 0.0f) : 0.0f;
    swingSlider.setValue (juce::jlimit (-1.0f, 1.0f, swing), juce::dontSendNotification);
}

void PerformanceBar::timerCallback()
{
    auto& engine = proc.getGestureEngine();

    const bool modeChanged = (playModeBox.getSelectedId() == 2)
                              != (engine.getPlayMode() == PlayMode::Midi);
    const bool lockChanged = sceneLockToggle.getToggleState() != engine.isSceneLocked();

    if (modeChanged || lockChanged)
        refresh();
}

void PerformanceBar::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bg0);

    // Hairline against the keyboard strip above, so the two rows read as separate controls
    // rather than one crowded band.
    g.setColour (Palette::bg3);
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
}

void PerformanceBar::resized()
{
    auto r = getLocalBounds().reduced (10, 4);

    auto placeField = [&r] (juce::Label& caption, juce::ComboBox& box, int captionW, int boxW)
    {
        caption.setBounds (r.removeFromLeft (captionW));
        box.setBounds (r.removeFromLeft (boxW).reduced (0, 3));
        r.removeFromLeft (12);
    };

    placeField (playModeLabel, playModeBox, 34, 66);
    placeField (quantizeLabel, quantizeBox, 44, 68);
    placeField (releaseLabel, releaseBox, 52, 76);

    // Grid: beats x divisions reads as one control, so the two boxes stay tight around the "x".
    gridLabel.setBounds (r.removeFromLeft (30));
    beatsBox.setBounds (r.removeFromLeft (46).reduced (0, 3));
    gridTimesLabel.setBounds (r.removeFromLeft (16));
    divisionsBox.setBounds (r.removeFromLeft (46).reduced (0, 3));
    r.removeFromLeft (12);

    swingLabel.setBounds (r.removeFromLeft (38));
    swingSlider.setBounds (r.removeFromLeft (150).reduced (0, 2));
    r.removeFromLeft (12);

    sceneLockToggle.setBounds (r.removeFromLeft (70));
}

} // namespace stutter::ui
