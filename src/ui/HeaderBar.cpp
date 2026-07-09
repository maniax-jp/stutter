#include "HeaderBar.h"
#include "../PluginProcessor.h"
#include "../PresetManager.h"
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

    // ---- Preset browser ----
    addAndMakeVisible (presetArea);
    presetPrevButton.onClick = [this] { proc.getPresetManager().loadPrevious(); };
    presetNextButton.onClick = [this] { proc.getPresetManager().loadNext(); };
    for (auto* b : { &presetPrevButton, &presetNextButton })
        addAndMakeVisible (*b);

    presetNameButton.onClick = [this] { showPresetMenu(); };
    addAndMakeVisible (presetNameButton);

    presetSaveButton.onClick = [this] { showSaveDialog(); };
    addAndMakeVisible (presetSaveButton);

    refreshPresetLabel();

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

    // ---- Host sync toggle ----
    syncToggle.setColour (juce::ToggleButton::textColourId, Palette::textLo);
    addAndMakeVisible (syncToggle);
    syncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.getAPVTS(), ID::hostSync, syncToggle);

    // ---- BPM readout / internal-BPM editor ----
    bpmLabel.setJustificationType (juce::Justification::centredRight);
    bpmLabel.setFont (StutterLookAndFeel::monoFont (13.0f));
    bpmLabel.setColour (juce::Label::textColourId, Palette::textLo);
    bpmLabel.setColour (juce::Label::textWhenEditingColourId, Palette::textHi);
    // Editable via double-click; while host-synced this is toggled off in
    // updateBpmEditableState() so there's nothing to edit (the readout is host-driven then).
    bpmLabel.setEditable (false, true, true);
    bpmLabel.onEditorShow = [this]
    {
        bpmLabelBeingEdited = true;
        if (auto* ed = bpmLabel.getCurrentTextEditor())
        {
            // Editing should operate on the raw BPM number, not the "120.0 BPM  ○ FREE" display.
            ed->setText (juce::String (proc.getAPVTS().getRawParameterValue (ID::internalBpm)->load(), 1),
                         juce::dontSendNotification);
            ed->setInputRestrictions (6, "0123456789.");
            ed->selectAll();
        }
    };
    bpmLabel.onEditorHide = [this]
    {
        bpmLabelBeingEdited = false;
        bpmLabelTextEdited();
    };
    addAndMakeVisible (bpmLabel);
    updateBpmEditableState (proc.isDisplayHostSynced());

    startTimerHz (15);
    timerCallback();
}

HeaderBar::~HeaderBar() { stopTimer(); }

void HeaderBar::timerCallback()
{
    const double bpm = proc.getDisplayBpm();
    const bool synced = proc.isDisplayHostSynced();

    // Don't clobber the label's text while the user is actively typing into it.
    if (! bpmLabelBeingEdited && (std::abs (bpm - lastShownBpm) > 0.05 || synced != lastShownSynced))
    {
        lastShownBpm = bpm;
        lastShownSynced = synced;

        juce::String text = juce::String (bpm, 1) + " BPM";
        text << (synced ? "  ● SYNC" : "  ○ FREE");
        bpmLabel.setText (text, juce::dontSendNotification);
        bpmLabel.setColour (juce::Label::textColourId, synced ? Palette::accent.withAlpha (0.9f) : Palette::textLo);

        updateBpmEditableState (synced);
    }

    // Cheap poll for the dirty flag (set by StepGrid/CurveEditor/parameter edits) so the "*"
    // modified-indicator appears promptly without needing every edit site to reach into HeaderBar.
    const bool dirtyNow = proc.getPresetManager().isDirty();
    if (dirtyNow != lastShownDirty)
    {
        lastShownDirty = dirtyNow;
        refreshPresetLabel();
    }
}

void HeaderBar::updateBpmEditableState (bool hostSynced)
{
    // FREE (internal clock) -> editable (double-click to type a BPM), bound to internalBpm.
    // Host-synced -> not editable (host owns the tempo) and visually dimmed to signal that.
    bpmLabel.setEditable (false, ! hostSynced, true);
    bpmLabel.setAlpha (hostSynced ? 0.75f : 1.0f);
    bpmLabel.setMouseCursor (hostSynced ? juce::MouseCursor::NormalCursor : juce::MouseCursor::IBeamCursor);
}

void HeaderBar::bpmLabelTextEdited()
{
    const double typed = bpmLabel.getText().getDoubleValue();
    if (typed <= 0.0)
        return; // unparsable / empty -> ignore, next timer tick redraws the live readout

    auto* param = proc.getAPVTS().getParameter (ID::internalBpm);
    if (param == nullptr)
        return;

    const auto range = proc.getAPVTS().getParameterRange (ID::internalBpm);
    const float clamped = juce::jlimit (range.start, range.end, (float) typed);

    param->beginChangeGesture();
    param->setValueNotifyingHost (range.convertTo0to1 (clamped));
    param->endChangeGesture();

    // Force an immediate redraw with the committed value rather than waiting for the next timer
    // tick (avoids a one-frame flash of the raw typed text against the "NNN BPM  ○ FREE" format).
    lastShownBpm = -1.0;
    timerCallback();
}

void HeaderBar::refreshPresetLabel()
{
    auto& pm = proc.getPresetManager();
    juce::String text = pm.getCurrentPresetName();
    if (pm.isDirty())
        text << " *";
    presetNameButton.setButtonText (text);
    lastShownDirty = pm.isDirty();
}

void HeaderBar::showPresetMenu()
{
    auto& pm = proc.getPresetManager();
    const auto& presets = pm.getPresets();

    // Two disjoint ID ranges packed into one PopupMenu, since showMenuAsync's callback only gets
    // a single result ID back for the whole menu (including submenu items): [1, N] load a preset
    // at (id - 1); [deleteIdBase, deleteIdBase + N) delete the user preset at (id - deleteIdBase).
    // deleteIdBase is chosen comfortably above any realistic preset count.
    constexpr int deleteIdBase = 100000;

    juce::PopupMenu menu;
    juce::String currentCategory;
    int itemId = 1; // PopupMenu item IDs must be non-zero

    for (int i = 0; i < (int) presets.size(); ++i)
    {
        const auto& entry = presets[(size_t) i];
        if (entry.category != currentCategory)
        {
            currentCategory = entry.category;
            menu.addSectionHeader (currentCategory);
        }

        const bool isCurrent = (i == pm.getCurrentIndex());

        if (entry.isFactory)
        {
            menu.addItem (itemId, entry.name, true, isCurrent);
        }
        else
        {
            // User preset: clicking the row loads it (as before); a submenu carries the
            // destructive "Delete..." action so it isn't a stray click away from loading.
            juce::PopupMenu userItemMenu;
            userItemMenu.addItem (itemId, "Load", true, isCurrent);
            userItemMenu.addSeparator();
            userItemMenu.addItem (deleteIdBase + i, "Delete...");
            menu.addSubMenu (entry.name + (isCurrent ? "  (current)" : ""), userItemMenu);
        }

        ++itemId;
    }

    // showMenuAsync's callback can likewise fire after this HeaderBar has been destroyed; guard
    // with a SafePointer the same way as the dialogs above.
    juce::Component::SafePointer<HeaderBar> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (presetNameButton),
        [safeThis] (int result)
        {
            if (result <= 0)
                return;
            if (safeThis == nullptr)
                return;

            if (result >= deleteIdBase)
            {
                safeThis->confirmDeleteUserPreset (result - deleteIdBase);
                return;
            }

            safeThis->proc.getPresetManager().loadPreset (result - 1);
        });
}

void HeaderBar::confirmDeleteUserPreset (int presetIndex)
{
    auto& pm = proc.getPresetManager();
    const auto& presets = pm.getPresets();
    if (presetIndex < 0 || presetIndex >= (int) presets.size())
        return;

    const juce::String name = presets[(size_t) presetIndex].name;

    auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::WarningIcon,
        "Delete Preset",
        "Delete the user preset \"" + name + "\"? This can't be undone.",
        "Delete", "Cancel", this);

    // showAsync: non-blocking, returns immediately; the callback fires later on the message
    // thread when the user dismisses the dialog. Never use the modal/blocking AlertWindow API.
    // StutterLookAndFeel doesn't opt into native alert windows, so this routes through
    // LookAndFeel_V2::createAlertWindow's 2-button numbering: button1 ("Delete") -> 1,
    // button2 ("Cancel") -> 0 (see juce_LookAndFeel_V2.cpp's createAlertWindow()).
    //
    // The callback can fire after this HeaderBar has been destroyed (e.g. plugin editor closed
    // while the dialog is still up), so capture a SafePointer rather than `this` and bail out if
    // it's gone by the time the callback runs.
    juce::Component::SafePointer<HeaderBar> safeThis (this);
    juce::AlertWindow::showAsync (options, [safeThis, presetIndex, name] (int result)
    {
        if (result != 1)
            return;
        if (safeThis == nullptr)
            return;

        auto& pmInner = safeThis->proc.getPresetManager();

        // Re-resolve the index by name: the menu may have stayed open across an async round
        // trip long enough for the list to change underneath (e.g. another save/delete), so
        // don't trust the captured index blindly if it no longer matches.
        int idx = presetIndex;
        const auto& list = pmInner.getPresets();
        if (idx < 0 || idx >= (int) list.size() || list[(size_t) idx].name != name || list[(size_t) idx].isFactory)
        {
            idx = -1;
            for (int i = 0; i < (int) list.size(); ++i)
                if (! list[(size_t) i].isFactory && list[(size_t) i].name == name)
                {
                    idx = i;
                    break;
                }
        }

        if (idx >= 0)
            pmInner.deleteUserPreset (idx);

        safeThis->refreshPresetLabel();
    });
}

void HeaderBar::showSaveDialog()
{
    saveDialog = std::make_unique<juce::AlertWindow> ("Save Preset",
        "Enter a name for the user preset:", juce::MessageBoxIconType::NoIcon);
    saveDialog->addTextEditor ("name", proc.getPresetManager().getCurrentPresetName(), "Name:");
    saveDialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    saveDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    // enterModalState's callback can fire after this HeaderBar has been destroyed (e.g. plugin
    // editor closed while the dialog is still up), so capture a SafePointer rather than `this`
    // and bail out if it's gone by the time the callback runs.
    juce::Component::SafePointer<HeaderBar> safeThis (this);
    saveDialog->enterModalState (true, juce::ModalCallbackFunction::create (
        [safeThis] (int result)
        {
            if (safeThis == nullptr)
                return;

            if (result == 1 && safeThis->saveDialog != nullptr)
            {
                const auto name = safeThis->saveDialog->getTextEditorContents ("name");
                safeThis->proc.getPresetManager().saveUserPreset (name);
                safeThis->refreshPresetLabel();
            }
            safeThis->saveDialog.reset();
        }));
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

    // BPM readout + sequencer/sync toggles on the far right
    auto rightArea = r.removeFromRight (196);
    bpmLabel.setBounds (rightArea.removeFromTop (18));
    auto toggleRow = rightArea.withSizeKeepingCentre (rightArea.getWidth(), 22).translated (0, 6);
    syncToggle.setBounds (toggleRow.removeFromRight (toggleRow.getWidth() / 2));
    toggleRow.removeFromRight (4);
    seqToggle.setBounds (toggleRow);

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
