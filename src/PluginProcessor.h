#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/CaptureBuffer.h"
#include "dsp/StepSequencer.h"
#include "dsp/CurveModulator.h"
#include "dsp/ParameterIDs.h"
#include "PresetManager.h"

namespace stutter
{
enum class ModTarget { Volume, Filter, Pan, Count };
}

class StutterAudioProcessor : public juce::AudioProcessor
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
    stutter::StepSequencer& getSequencer() noexcept { return sequencer; }
    stutter::CurveModulator& getCurve (stutter::ModTarget target) noexcept
    {
        return curves[(size_t) target];
    }

    stutter::PresetManager& getPresetManager() noexcept { return *presetManager; }

    /** Current internal free-running clock BPM (used when host transport is stopped/not available). */
    double getInternalBpm() const noexcept { return internalBpm.load (std::memory_order_relaxed); }
    void setInternalBpm (double bpm) noexcept { internalBpm.store (bpm, std::memory_order_relaxed); }

    /** For UI: current effective BPM/PPQ/playhead-step, updated once per block. Lock-free reads. */
    double getDisplayBpm() const noexcept { return displayBpm.load (std::memory_order_relaxed); }
    bool isDisplayHostSynced() const noexcept { return displayHostSynced.load (std::memory_order_relaxed); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Lane construction order matches SPEC's 8 lanes:
    // 0 Stutter, 1 TapeStop, 2 TapeStart, 3 Reverse, 4 Repitch, 5 Gate, 6 Filter, 7 Crush
    static constexpr int laneStutter   = 0;
    static constexpr int laneTapeStop  = 1;
    static constexpr int laneTapeStart = 2;
    static constexpr int laneReverse   = 3;
    static constexpr int laneRepitch   = 4;
    static constexpr int laneGate      = 5;
    static constexpr int laneFilter    = 6;
    static constexpr int laneCrush     = 7;

private:
    void updateTransportAndSequence (juce::AudioBuffer<float>& buffer);
    void applyGlobalModulators (juce::AudioBuffer<float>& buffer);
    void applyDryWetAndGain (const juce::AudioBuffer<float>& dryBuffer, juce::AudioBuffer<float>& wetBuffer);

    juce::AudioProcessorValueTreeState apvts;

    stutter::CaptureBuffer captureBuffer;
    stutter::StepSequencer sequencer;

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
    bool wasHostPlaying = false;
    std::atomic<double> internalBpm { 120.0 };

    std::atomic<double> displayBpm { 120.0 };
    std::atomic<bool> displayHostSynced { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StutterAudioProcessor)
};
