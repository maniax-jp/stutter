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

    placeField (playModeLabel, playModeBox, 34, 74);
    placeField (quantizeLabel, quantizeBox, 44, 74);
    placeField (releaseLabel, releaseBox, 52, 82);

    sceneLockToggle.setBounds (r.removeFromLeft (74));
}

} // namespace stutter::ui
