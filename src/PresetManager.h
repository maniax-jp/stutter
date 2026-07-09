#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>
#include <functional>
#include <vector>

class StutterAudioProcessor;

namespace stutter
{

/** One entry in the preset browser list. */
struct PresetEntry
{
    juce::String name;
    juce::String category;   // "Stutter", "Tape", "Gate & Sidechain", "Glitch", "Filter & Texture", "User"
    bool isFactory = true;
    juce::File userFile;     // valid only when isFactory == false
};

/**
    Owns the full preset lifecycle: factory presets (built in code as ValueTrees, matching
    exactly the same XML shape that StutterAudioProcessor::getStateInformation/setStateInformation
    use), user presets (read/write as .xml files under
    ~/Library/Audio/Presets/Maniax/Stutter/), current-preset tracking, dirty flag, and
    prev/next navigation in category+name order.

    All mutation happens on the message thread (this mirrors setStateInformation's contract:
    applying a preset calls the exact same processor->setStateInformation() path, so the audio
    thread only ever observes fully-formed state swaps that were already safe under the
    existing state-load mechanism).
*/
class PresetManager : private juce::ValueTree::Listener
{
public:
    explicit PresetManager (StutterAudioProcessor& processor);

    /** Rebuilds the full sorted list (factory presets + whatever user presets are currently
        on disk). Call after saving/deleting a user preset. */
    void refreshUserPresets();

    const std::vector<PresetEntry>& getPresets() const noexcept { return presets; }

    int getCurrentIndex() const noexcept { return currentIndex; }
    juce::String getCurrentPresetName() const noexcept;
    bool isDirty() const noexcept { return dirty; }

    /** Marks the current preset as modified (called by the UI whenever any parameter/step/
        curve edit happens after a preset was loaded). */
    void markDirty() noexcept { dirty = true; }

    /** Loads the preset at the given index in getPresets() and applies it to the processor
        via the same code path as setStateInformation. Safe to call from the message thread only. */
    void loadPreset (int index);

    void loadNext();
    void loadPrevious();

    /** Saves the processor's current full state as a new user preset with the given name
        (overwrites if a user preset with that name already exists), then refreshes the list
        and selects it as current. */
    void saveUserPreset (const juce::String& presetName);

    /** Deletes the user preset at the given index (must be a non-factory entry; a no-op
        otherwise) from disk, then refreshes the list. If the deleted preset was the current
        one, the current name is preserved (via currentNameOverride) so the browser keeps
        showing its name -- but marked dirty, since that named preset no longer exists on disk
        and the current in-memory state is now unsaved. Returns true if a file was deleted. */
    bool deleteUserPreset (int index);

    static juce::File getUserPresetDirectory();

    /** Called (on the message thread) whenever a preset finishes loading, so the editor can
        force StepGrid/CurveEditor/LaneParamPanel to repaint with the new state (parameter-bound
        controls already refresh themselves via APVTS listener callbacks; this covers the
        non-APVTS structural data -- step grid + curve breakpoints -- which UI components only
        pull on repaint, not via a listener). */
    std::function<void()> onPresetLoaded;

private:
    void rebuildPresetList();
    juce::ValueTree loadEntryState (const PresetEntry& entry) const;

    // juce::ValueTree::Listener: used purely to detect "user touched a parameter after a preset
    // was loaded" (marks dirty). Any property/child change on the live APVTS tree qualifies;
    // changes made while applyingPreset is true (i.e. our own setStateInformation() call while
    // loading a preset) are ignored so loading a preset doesn't immediately mark itself dirty.
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeRedirected (juce::ValueTree&) override;

    StutterAudioProcessor& proc;
    std::vector<PresetEntry> presets; // sorted: factory categories in fixed order, then User
    int currentIndex = 0;
    bool dirty = false;
    bool applyingPreset = false;
    juce::String currentNameOverride; // used right after saving, before rebuildPresetList() catches up

    // Set when the *current* user preset is deleted: its name no longer appears anywhere in
    // `presets`, so getCurrentPresetName() can't find it by index lookup any more. Keeps the
    // browser showing the just-deleted name (now unsaved/dirty) until the user loads/saves a
    // different preset. Cleared by loadPreset()/saveUserPreset().
    juce::String deletedCurrentPresetName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

} // namespace stutter
