#include "PerformanceBar.h"
#include "../PluginProcessor.h"
#include "../state/SceneSchema.h"

namespace stutter::ui
{

namespace
{
void styleCaption (juce::Label& l)
{
    // Same size and spacing as the header's knob captions (DRY/WET, OUTPUT). These used to
    // carry extra kerning, which widened the text past its fixed-width label and got it
    // elided to "..." as soon as the window was scaled up.
    l.setJustificationType (juce::Justification::centredLeft);
    l.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::plain)));
    l.setColour (juce::Label::textColourId, Palette::textLo);
}

} // namespace

void ActiveIndicator::paintButton (juce::Graphics& g, bool shouldDrawHighlighted, bool shouldDrawDown)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f, 3.0f);
    const bool on = getToggleState();

    // On and off have to be distinguishable at a glance from across a room, because during
    // playback this is the only thing telling the user whether the bar they are hearing is
    // glitched. Colour alone is not enough at this size, so the lit state also gets a filled
    // body and an outer glow while the dark state gets neither.
    juce::Colour body = on ? Palette::accent.withAlpha (0.85f) : Palette::bg1;
    if (shouldDrawHighlighted)
        body = body.brighter (0.12f);
    if (shouldDrawDown)
        body = body.darker (0.15f);

    g.setColour (body);
    g.fillRoundedRectangle (r, 3.0f);

    if (on)
    {
        g.setColour (Palette::accent);
        g.drawRoundedRectangle (r, 3.0f, 1.5f);
        g.setColour (Palette::accent.withAlpha (0.28f));
        g.drawRoundedRectangle (r.expanded (1.5f), 4.0f, 1.5f);
    }
    else
    {
        g.setColour (Palette::bg3);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }

    g.setColour (on ? Palette::bg0 : Palette::textLo);
    g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)).withExtraKerningFactor (0.10f));
    g.drawText ("ACTIVE", r, juce::Justification::centred, false);
}

PerformanceBar::PerformanceBar (StutterAudioProcessor& processor, SceneDocument& document)
    : proc (processor), doc (document)
{
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

    activeToggle.setTooltip ("Whether the effect is heard. Automate this to glitch only the bars "
                             "you want; everywhere else the source passes through untouched.");
    // The attachment owns the value in both directions, so there is no onClick here: writing
    // the parameter from a callback as well would race the attachment's own write.
    activeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.getAPVTS(), ID::active, activeToggle);
    addAndMakeVisible (activeToggle);

    refresh();

    // 30Hz: the ACTIVE indicator has to track automation, which can flip it on any block
    // boundary, and a slower poll would show the change late enough to read as a glitch in
    // the UI rather than in the audio. Each tick is a couple of atomic loads when idle.
    startTimerHz (30);
}

PerformanceBar::~PerformanceBar() { stopTimer(); }

void PerformanceBar::setSceneIndex (int newSceneIndex)
{
    if (newSceneIndex == sceneIndex)
        return;
    sceneIndex = newSceneIndex;
    refresh();
}

void PerformanceBar::refresh()
{
    const juce::ScopedValueSetter<bool> guard (updatingFromModel, true);

    // Read-only: pulling values back from the model must not create the node.
    auto scene = doc.findScene (sceneIndex);

    beatsBox.setSelectedId (juce::jlimit (1, 8, doc.getBeats (sceneIndex)),
                            juce::dontSendNotification);
    divisionsBox.setSelectedId (juce::jlimit (2, 8, doc.getDivisions (sceneIndex)),
                                juce::dontSendNotification);

    const float swing = scene.isValid() ? (float) scene.getProperty (SceneIDs::swing, 0.0f) : 0.0f;
    swingSlider.setValue (juce::jlimit (-1.0f, 1.0f, swing), juce::dontSendNotification);
}

void PerformanceBar::timerCallback()
{
    // Repaint on the parameter, not on the button's toggle state. The attachment updates that
    // state through an AsyncUpdater, so a fast automation edge can be coalesced away before it
    // ever reaches the button -- reading the parameter is what makes the indicator honest.
    const bool nowActive = proc.getAPVTS().getRawParameterValue (ID::active)->load() > 0.5f;
    if (nowActive != lastActiveState)
    {
        lastActiveState = nowActive;
        activeToggle.setToggleState (nowActive, juce::dontSendNotification);
        activeToggle.repaint();
    }
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

    // ACTIVE leads the bar: it is the control the user reaches for most, and during playback
    // it is the one they are reading rather than operating.
    activeToggle.setBounds (r.removeFromLeft (72));
    r.removeFromLeft (18);

    // Grid: beats x divisions reads as one control, so the two boxes stay tight around the "x".
    // The caption widths carry a few pixels of slack past the text so nothing elides.
    gridLabel.setBounds (r.removeFromLeft (38));
    beatsBox.setBounds (r.removeFromLeft (46).reduced (0, 3));
    gridTimesLabel.setBounds (r.removeFromLeft (16));
    divisionsBox.setBounds (r.removeFromLeft (46).reduced (0, 3));
    r.removeFromLeft (12);

    swingLabel.setBounds (r.removeFromLeft (48));
    swingSlider.setBounds (r.removeFromLeft (150).reduced (0, 2));
}

} // namespace stutter::ui
