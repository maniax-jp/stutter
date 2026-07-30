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

    // These knobs have no text box -- the caption beneath them is the only space available, so
    // hovering swaps the name for the value and leaving puts the name back. Showing the number
    // permanently would leave the header reading as a row of anonymous digits.
    dryWetKnob.onValueChange = [this] { refreshKnobCaptions(); };
    outputKnob.onValueChange = [this] { refreshKnobCaptions(); };

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

void HeaderBar::refreshKnobCaptions()
{
    // isMouseOver(true) includes the knob's children, so the whole control counts as hovered
    // rather than just the pixels the rotary happens to paint. isMouseButtonDown covers the
    // rest of a drag: the pointer leaves the knob's bounds almost immediately once you start
    // moving, and losing the readout mid-drag is exactly when it is being read.
    const bool overDryWet = dryWetKnob.isMouseOver (true) || dryWetKnob.isMouseButtonDown (true);
    const bool overOutput = outputKnob.isMouseOver (true) || outputKnob.isMouseButtonDown (true);

    // Percent for a 0..1 mix, dB for a gain: the unit a user would say out loud.
    dryWetLabel.setText (overDryWet
                             ? juce::String (juce::roundToInt (dryWetKnob.getValue() * 100.0)) + "%"
                             : "DRY/WET",
                         juce::dontSendNotification);

    // Explicit sign so "+3.0 dB" cannot be misread as a cut.
    const double gain = outputKnob.getValue();
    outputLabel.setText (overOutput
                             ? (gain >= 0.0 ? "+" : "") + juce::String (gain, 1) + " dB"
                             : "OUTPUT",
                         juce::dontSendNotification);
}

void HeaderBar::timerCallback()
{
    refreshKnobCaptions();

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

namespace
{
/**
    A preset row that can be right-clicked.

    juce::PopupMenu has no notion of a secondary click on an item, so a user preset that wants
    "left-click loads, right-click offers Delete" has to supply its own component. Everything
    else about the row -- the tick, the highlight, the text position -- is drawn by the menu's
    own item renderer so it stays indistinguishable from the factory rows around it.
*/
/** Row height for the preset menu. Compact enough that the whole list fits one column --
    see showPresetMenu for why that matters -- and shared so custom rows match plain ones. */
constexpr int standardItemHeight = 20;

class UserPresetMenuItem : public juce::PopupMenu::CustomComponent
{
public:
    UserPresetMenuItem (juce::String presetName, bool isTicked, HeaderBar::PresetRowClickState& clickState)
        // autoTrigger=false: with the default (true) the menu window triggers the item on
        // mouse-up by itself and never calls mouseUp here, so a right-click could not be
        // distinguished from a left one -- it just loaded the preset.
        : juce::PopupMenu::CustomComponent (false),
          name (std::move (presetName)), ticked (isTicked), state (&clickState)
    {
    }

    void getIdealSize (int& idealWidth, int& idealHeight) override
    {
        // Pass the same standard height the menu was shown with, so this row lines up with the
        // plain ones around it. Asking with -1 makes the look-and-feel pick its own default,
        // which is taller than the compact height the preset list uses and left user rows
        // standing proud of their neighbours.
        getLookAndFeel().getIdealPopupMenuItemSize (name, false, standardItemHeight,
                                                    idealWidth, idealHeight);
    }

    void paint (juce::Graphics& g) override
    {
        getLookAndFeel().drawPopupMenuItem (g, getLocalBounds(),
                                            false,              // isSeparator
                                            true,               // isActive
                                            isItemHighlighted() && ! showingDelete,
                                            ticked,
                                            false,              // hasSubMenu
                                            name, {}, nullptr, nullptr);

        if (! showingDelete)
            return;

        // Drawn over the row rather than in a menu of its own. A second PopupMenu dismisses
        // the first, so the list vanished and "Delete..." floated with nothing to say which
        // preset it belonged to. Painted in place, it stays anchored to its row.
        const auto box = deleteBounds().toFloat();

        g.setColour (Palette::bg0.withAlpha (0.96f));
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (Palette::accent.withAlpha (0.8f));
        g.drawRoundedRectangle (box, 3.0f, 1.0f);

        g.setColour (deleteHot ? Palette::textHi : Palette::textLo);
        g.setFont (juce::Font (juce::FontOptions ((float) standardItemHeight * 0.55f)));
        g.drawText ("Delete...", box, juce::Justification::centred, false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (showingDelete)
        {
            // Inside the box commits; anywhere else on the row just puts it away, so a stray
            // click cannot delete a preset.
            const bool hit = deleteBounds().contains (e.getPosition());
            showingDelete = false;

            if (hit && state != nullptr)
            {
                state->wasRightClick = true;
                triggerMenuItem();
                return;
            }

            repaint();
            return;
        }

        if (e.mods.isPopupMenu())
        {
            // Offer the action, do not perform it: a right-click reveals Delete on this row
            // and leaves the list open so the user can see what they are about to remove.
            showingDelete = true;
            deleteHot = false;
            repaint();
            return;
        }

        if (state != nullptr)
            state->wasRightClick = false;

        triggerMenuItem();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (! showingDelete)
            return;

        const bool hot = deleteBounds().contains (e.getPosition());
        if (hot != deleteHot)
        {
            deleteHot = hot;
            repaint();
        }
    }

    /** True while this row is offering Delete, so the menu knows not to treat a move onto
        another row as a plain highlight change. */
    bool isOfferingDelete() const noexcept { return showingDelete; }

private:
    juce::Rectangle<int> deleteBounds() const
    {
        // Right-aligned, inset a little: it sits over the row's own text without hiding the
        // start of the name, which is what identifies the preset.
        const int w = juce::jmin (getWidth() - 8, 78);
        return getLocalBounds().removeFromRight (w + 6).reduced (3, 2);
    }

    juce::String name;
    bool ticked;
    HeaderBar::PresetRowClickState* state = nullptr;
    bool showingDelete = false;
    bool deleteHot = false;
};

} // namespace

void HeaderBar::showPresetMenu()
{
    auto& pm = proc.getPresetManager();
    const auto& presets = pm.getPresets();

    // One ID range: item (i + 1) loads preset i. PopupMenu reserves 0 for "nothing chosen".
    // Deleting is offered by the user rows themselves -- see UserPresetMenuItem.
    juce::PopupMenu menu;
    juce::String currentCategory;
    int itemId = 1; // PopupMenu item IDs must be non-zero

    // The list is taller than any sane window, so it will be split across columns. Left to
    // itself JUCE divides by item count and thinks nothing of the section headers -- a column
    // began right below "Gate & Sidechain", orphaning the heading from the presets it names.
    // Breaking at category boundaries keeps each heading with its contents.
    constexpr int rowsPerColumn = 26;
    int rowsInColumn = 0;

    for (int i = 0; i < (int) presets.size(); ++i)
    {
        const auto& entry = presets[(size_t) i];
        if (entry.category != currentCategory)
        {
            // Count how many rows this category needs, and start a new column if it will not
            // fit -- rather than letting the split land wherever the arithmetic falls.
            int categoryRows = 1;   // the heading
            for (int j = i; j < (int) presets.size()
                            && presets[(size_t) j].category == entry.category; ++j)
                ++categoryRows;

            if (rowsInColumn > 0 && rowsInColumn + categoryRows > rowsPerColumn)
            {
                menu.addColumnBreak();
                rowsInColumn = 0;
            }

            currentCategory = entry.category;
            menu.addSectionHeader (currentCategory);
            ++rowsInColumn;
        }

        ++rowsInColumn;

        const bool isCurrent = (i == pm.getCurrentIndex());

        if (entry.isFactory)
        {
            menu.addItem (itemId, entry.name, true, isCurrent);
        }
        else
        {
            // User rows look and load exactly like factory rows, but also accept a right-click
            // for Delete. A submenu was tried first and cost the common action (load) an extra
            // step to shelter the rare one -- and it hid the tick, so there was no way to see
            // which user preset was loaded.
            auto item = std::make_unique<UserPresetMenuItem> (entry.name, isCurrent,
                                                              presetRowClick);
            menu.addCustomItem (itemId, std::move (item), nullptr, entry.name);
        }

        ++itemId;
    }

    // Cleared before every showing; a user row fills it in on the way out.
    presetRowClick = {};

    // showMenuAsync's callback can likewise fire after this HeaderBar has been destroyed; guard
    // with a SafePointer the same way as the dialogs above.
    juce::Component::SafePointer<HeaderBar> safeThis (this);
    // Parent the menu to the editor rather than letting it float as its own desktop window.
    // A free-floating menu keeps its screen position, so dragging the plugin window out from
    // under it left the list stranded where the window used to be.
    //
    // A tighter item height keeps the whole list in one column. JUCE splits an over-long menu
    // into columns by dividing the item count evenly, with no regard for the section headers,
    // so a second column started immediately below a heading and orphaned it from the presets
    // it names. Fitting the list vertically avoids the split entirely.
    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (presetNameButton)
                            .withParentComponent (getTopLevelComponent())
                            .withStandardItemHeight (standardItemHeight),
        [safeThis] (int result)
        {
            if (result <= 0 || safeThis == nullptr)
                return;

            const int presetIndex = result - 1;

            // The row itself already offered Delete in place and the user committed to it --
            // see UserPresetMenuItem. Nothing more to ask.
            if (safeThis->presetRowClick.wasRightClick)
            {
                safeThis->presetRowClick = {};
                safeThis->confirmDeleteUserPreset (presetIndex);
                return;
            }

            safeThis->proc.getPresetManager().loadPreset (presetIndex);
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
