#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/CaptureBuffer.h"
#include "dsp/CurveModulator.h"
#include "dsp/SceneSelector.h"
#include "dsp/ParameterIDs.h"
#include "PresetManager.h"
#include "state/SceneStore.h"
#include "state/LiveParamOverlay.h"
#include "state/SceneDocument.h"
#include "dsp/BlockSequencer.h"
#include "dsp/ModulationEngine.h"
#include "state/SceneSchema.h"

namespace stutter
{
enum class ModTarget { Volume, Filter, Pan, Count };
}

class StutterAudioProcessor : public juce::AudioProcessor, public juce::Timer
{
public:
    StutterAudioProcessor();
    ~StutterAudioProcessor() override;

    // ---- AudioProcessor ----
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- Public access for the (phase-2) editor ----
    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts; }
    stutter::CurveModulator& getCurve (stutter::ModTarget target) noexcept
    {
        return curves[(size_t) target];
    }

    stutter::PresetManager& getPresetManager() noexcept { return *presetManager; }

    /** For UI: current effective BPM/PPQ/playhead-step, updated once per block. Lock-free reads. */
    double getDisplayBpm() const noexcept { return displayBpm.load (std::memory_order_relaxed); }
    bool isDisplayHostSynced() const noexcept { return displayHostSynced.load (std::memory_order_relaxed); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Lane construction order matches SPEC's 8 lanes:
    // 0 Stutter, 1 TapeStop, 2 TapeStart, 3 Reverse, 4 Repitch, 5 Gate, 6 Filter, 7 Crush
    static constexpr int laneStutter   = stutter::lanes::stutterLane;
    static constexpr int laneTapeStop  = stutter::lanes::tapeStop;
    static constexpr int laneTapeStart = stutter::lanes::tapeStart;
    static constexpr int laneReverse   = stutter::lanes::reverse;
    static constexpr int laneRepitch   = stutter::lanes::repitch;
    static constexpr int laneGate      = stutter::lanes::gate;
    static constexpr int laneFilter    = stutter::lanes::filter;
    static constexpr int laneCrush     = stutter::lanes::crush;
    static constexpr int laneStretcher = stutter::lanes::stretcher;
    static constexpr int laneShuffler  = stutter::lanes::shuffler;
    static constexpr int laneDelay     = stutter::lanes::delay;
    static constexpr int laneDistort   = stutter::lanes::distort;
private:
    void updateTransportAndSequence (juce::AudioBuffer<float>& buffer);
    void applyGlobalModulators (juce::AudioBuffer<float>& buffer);
    void applyDryWetAndGain (const juce::AudioBuffer<float>& dryBuffer, juce::AudioBuffer<float>& wetBuffer);

    void timerCallback() override;
    void loadInitState();

    /** Push the active scene's parameter values into APVTS so the UI and the host see what
        is actually playing. Message thread only; see the implementation for why the
        write-back listener must be suppressed while it runs. */
    void mirrorActiveSceneToApvts();

    /** Flush parameter edits that arrived since the last call into their scene.
        Message thread only; driven by the processor's timer. */
    void flushPendingLaneParamWrites();

    /** Processes one chunk (<= dryScratchBuffer's capacity) through the full transport/sequencer/
        modulator/dry-wet chain. See processBlock() for why blocks larger than that capacity are
        split into successive calls to this. */
    void processChunk (juce::AudioBuffer<float>& chunk);

    juce::AudioProcessorValueTreeState apvts;

    stutter::CaptureBuffer captureBuffer;

    // Order matches ModTarget: Volume, Filter, Pan. Each starts enabled + flat at its own
    // neutral value (see stutter::ID::neutralValueForCurve, the single source of truth: 0.5 =
    // unity/center for Volume/Pan; 1.0 = fully-open 20kHz cutoff for Filter, since Filter's 0..1
    // range maps exponentially to 200Hz..20kHz and 0.5 would be an audible ~2kHz cut) so a
    // freshly-instantiated plugin is acoustically transparent, matching the Init preset exactly.
    std::array<stutter::CurveModulator, (size_t) stutter::ModTarget::Count> curves {
        stutter::CurveModulator (stutter::ID::neutralValueForCurve (stutter::ID::curveNameVolume)),
        stutter::CurveModulator (stutter::ID::neutralValueForCurve (stutter::ID::curveNameFilter)),
        stutter::CurveModulator (stutter::ID::neutralValueForCurve (stutter::ID::curveNamePan)),
    };

    // Constructed last (after apvts/sequencer/curves exist) since it reads them when building
    // factory preset states; declared last so member destruction order doesn't matter either way.
    std::unique_ptr<stutter::PresetManager> presetManager;

    // v2 scene store and scalar param overlay
public:
    /** Scene selection and the wet gate. Exposed so the editor can show which scene is
        actually playing, and so tests can assert on it end to end. */
    stutter::SceneSelector& getSceneSelector() noexcept { return sceneSelector; }

    /** The editable scene tree the UI mutates. Baked into SceneStore on publish(). */
    stutter::SceneDocument& getSceneDocument() noexcept { return *sceneDocument; }

    /** The baked bank the audio thread reads. Exposed read-only so tests can assert on what
        actually reached the audio path, rather than on the document that was supposed to
        produce it -- the two disagreeing is precisely the failure worth catching. */
    const stutter::SceneStore& getSceneStore() const noexcept { return sceneStore; }

    juce::UndoManager& getUndoManager() noexcept { return undoManager; }

    /** Run one mirror pass immediately. The shipping path is the processor's timer; this is
        exposed so the offline harness can drive it deterministically rather than depending
        on message-loop scheduling. */
    void pumpSceneMirror() { flushPendingLaneParamWrites(); mirrorActiveSceneToApvts(); }

    /** Current division for the block grid's playhead, or -1 when idle. */
    int getBlockPlayheadDivision() const noexcept { return blockSequencer.getPlayheadDivision(); }

    /** A v2 lane's effect, for the UI to read its parameter descriptors. */
    stutter::LaneEffect* getBlockSequencerEffect (int lane) noexcept
    {
        return blockSequencer.getLaneEffect (lane);
    }

private:
    stutter::SceneStore sceneStore;
    stutter::LiveParamOverlay paramOverlay;

    // Holds the automated scene selection and produces the wet-path gate. Polled ahead of the
    // sequencer in processChunk so a change takes effect on the chunk it belongs to.
    stutter::SceneSelector sceneSelector;

    // The sequencer. Drives the audio path and owns the playhead the block grid renders.
    stutter::BlockSequencer blockSequencer;
    stutter::ModulationEngine modulationEngine;

    /** Which scene the chain order was last sorted for; -1 forces a re-sort. */
    int lastChainOrderScene = -1;

    /** Set while mirrorActiveSceneToApvts is writing, so the parameter listener can tell a
        mirror write from a genuine user edit. Message thread only. */
    bool suppressParamWriteback = false;

    /** Which scene APVTS currently reflects; -1 before the first mirror. */
    int mirroredScene = -1;

    /** One per lane parameter, so the callback already knows which lane and slot it is for
        rather than parsing "lane3_decay" back apart on every knob move.

        parameterValueChanged fires on whatever thread set the parameter -- the audio thread
        for host automation, and several threads at once under a validator. So it only records
        the value; the scene write happens on the timer. Doing the write here mutated a
        ValueTree and rebuilt the whole bank from arbitrary threads, which deadlocked
        pluginval's parameter thread-safety test.

        Only *gestured* changes are recorded. APVTS mirrors the active scene's lane values, so
        a host replaying an automation lane writes the same parameters a knob does, and without
        this distinction that playback would be baked into the scene document and made
        permanent by the next project save. JUCE's Slider and ComboBox attachments always
        bracket a drag in begin/endChangeGesture; automation playback never does. This listens
        to the parameter rather than to APVTS because only the parameter reports gestures. */
    struct LaneParamWriteback : juce::AudioProcessorParameter::Listener
    {
        LaneParamWriteback (StutterAudioProcessor& p, int l, int idx, juce::String id,
                            juce::RangedAudioParameter& parameter)
            : owner (p), lane (l), paramIndex (idx), paramID (std::move (id)), param (parameter) {}

        void parameterValueChanged (int, float newNormalisedValue) override
        {
            if (! inGesture.load (std::memory_order_acquire))
                return;

            // Denormalise here so the timer does not have to reach back into the parameter
            // from the message thread while the audio thread may be writing it.
            pending.store (param.convertFrom0to1 (newNormalisedValue), std::memory_order_relaxed);
            dirty.store (true, std::memory_order_release);
            owner.laneParamsDirty.store (true, std::memory_order_release);
        }

        void parameterGestureChanged (int, bool gestureIsStarting) override
        {
            inGesture.store (gestureIsStarting, std::memory_order_release);

            // Catch the final value on release. A drag's last move can arrive before the
            // end-gesture, but a click that jumps straight to a value emits no move at all --
            // without this, such an edit would be dropped.
            if (! gestureIsStarting)
            {
                pending.store (param.convertFrom0to1 (param.getValue()), std::memory_order_relaxed);
                dirty.store (true, std::memory_order_release);
                owner.laneParamsDirty.store (true, std::memory_order_release);
            }
        }

        StutterAudioProcessor& owner;
        int lane, paramIndex;
        juce::String paramID;   // kept so detaching does not have to re-derive it
        juce::RangedAudioParameter& param;

        std::atomic<float> pending { 0.0f };
        std::atomic<bool> dirty { false };
        std::atomic<bool> inGesture { false };
    };

    std::vector<std::unique_ptr<LaneParamWriteback>> laneParamWritebacks;

    /** Set by any writeback listener, cleared by the timer. Lets the common case -- nothing
        touched a parameter since the last tick -- cost one atomic load instead of a scan. */
    std::atomic<bool> laneParamsDirty { false };

    juce::UndoManager undoManager;
    std::unique_ptr<stutter::SceneDocument> sceneDocument;

    // Smoothed globals (audio-rate, click-free)
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> dryWetSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputGainSmoothed;

    // Global modulator state
    juce::dsp::StateVariableTPTFilter<float> globalFilter;
    // Cutoff is smoothed and only pushed into globalFilter once per control-rate block
    // (see filterCutoffUpdateInterval in .cpp) rather than every sample: calling
    // setCutoffFrequency() per-sample recomputes internal filter coefficients on every
    // sample, which is both a CPU hit and a source of zipper noise on fast LFO sweeps.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> filterCutoffSmoothed;
    int filterCutoffUpdateCounter = 0;

    // Tracks whether globalFilter is currently being bypassed (cutoff pinned at/near the fully-
    // open ceiling -- see applyGlobalModulators). Hysteresis avoids on/off chattering when the
    // smoothed cutoff hovers around the threshold; on the bypass->engage transition we reset()
    // the filter so it doesn't resume processing from stale internal state (see High-severity
    // fix in applyGlobalModulators).
    bool globalFilterBypassed = true;

    juce::AudioBuffer<float> dryScratchBuffer;
    int dryScratchMaxChannels = 2;
    int dryScratchMaxSamples = 0;

    double currentSampleRate = 44100.0;

    // Host transport tracking / internal free-running clock fallback
    double lastKnownPpq = 0.0;
    double lastPpqPerSample = 0.0;
    double internalClockPpq = 0.0;

    std::atomic<double> displayBpm { 120.0 };
    std::atomic<bool> displayHostSynced { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StutterAudioProcessor)
};
