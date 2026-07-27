#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "dsp/effects/StutterEffect.h"
#include "dsp/effects/TapeStopEffect.h"
#include "dsp/effects/TapeStartEffect.h"
#include "dsp/effects/ReverseEffect.h"
#include "dsp/effects/RepitchEffect.h"
#include "dsp/effects/GateEffect.h"
#include "dsp/effects/FilterEffect.h"
#include "dsp/effects/CrushEffect.h"
#include "dsp/effects/StretcherEffect.h"
#include "dsp/effects/ShufflerEffect.h"
#include "dsp/effects/DelayEffect.h"
#include "dsp/effects/DistortionEffect.h"

#include "dsp/ParameterLayout.h"
#include "state/SceneSchema.h"
#include <array>

using namespace stutter;


//==============================================================================
StutterAudioProcessor::StutterAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    // The block sequencer owns the audio path. It carries all twelve lanes; StepSequencer
    // above is the v1 grid and has no representation for the four v2 additions.
    blockSequencer.setLaneEffect (laneStutter,   std::make_unique<StutterEffect>());
    blockSequencer.setLaneEffect (laneTapeStop,  std::make_unique<TapeStopEffect>());
    blockSequencer.setLaneEffect (laneTapeStart, std::make_unique<TapeStartEffect>());
    blockSequencer.setLaneEffect (laneReverse,   std::make_unique<ReverseEffect>());
    blockSequencer.setLaneEffect (laneRepitch,   std::make_unique<RepitchEffect>());
    blockSequencer.setLaneEffect (laneGate,      std::make_unique<GateEffect>());
    blockSequencer.setLaneEffect (laneFilter,    std::make_unique<FilterEffect>());
    blockSequencer.setLaneEffect (laneCrush,     std::make_unique<CrushEffect>());
    blockSequencer.setLaneEffect (laneStretcher, std::make_unique<StretcherEffect>());
    blockSequencer.setLaneEffect (laneShuffler,  std::make_unique<ShufflerEffect>());
    blockSequencer.setLaneEffect (laneDelay,     std::make_unique<DelayEffect>());
    blockSequencer.setLaneEffect (laneDistort,   std::make_unique<DistortionEffect>());

    // Register each lane's declared defaults with the schema BEFORE the document is built,
    // so the first bake already has them. A scene that omits a lane would otherwise get
    // all-zero parameters -- a Filter at cutoff 0 and a Gate at duty 0 are both silent, so
    // an unconfigured lane would produce nothing rather than its neutral sound.
    for (int lane = 0; lane < stutter::maxLanes; ++lane)
    {
        if (auto* effect = blockSequencer.getLaneEffect (lane))
        {
            const auto set = effect->getParamDescriptors();
            std::array<float, stutter::maxParamsPerLane> defs {};
            for (int i = 0; i < set.count && i < stutter::maxParamsPerLane; ++i)
                defs[(size_t) i] = set[i].defaultValue;
            stutter::SceneSchema::setLaneDefaults (lane, defs.data(),
                                                  juce::jmin (set.count, stutter::maxParamsPerLane));
        }
    }

    sceneDocument = std::make_unique<stutter::SceneDocument> (sceneStore, undoManager);
    presetManager = std::make_unique<stutter::PresetManager> (*this);
    startTimerHz (2);
}

StutterAudioProcessor::~StutterAudioProcessor() = default;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout StutterAudioProcessor::createParameterLayout()
{
    return stutter::createParameterLayout();
}

//==============================================================================
const juce::String StutterAudioProcessor::getName() const { return JucePlugin_Name; }
bool StutterAudioProcessor::acceptsMidi() const { return true; }
bool StutterAudioProcessor::producesMidi() const { return false; }
bool StutterAudioProcessor::isMidiEffect() const { return false; }
double StutterAudioProcessor::getTailLengthSeconds() const { return 0.0; }

//==============================================================================
void StutterAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    const int numCh = juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels());

    captureBuffer.prepare (sampleRate, numCh, 2.5);
    gestureEngine.prepare (sampleRate);
    blockSequencer.prepare (sampleRate, numCh);
    modulationEngine.prepare (sampleRate);
    lastChainOrderScene = -1;
    gestureEngine.setIdentityMapping();

    dryWetSmoothed.reset (sampleRate, 0.02);
    outputGainSmoothed.reset (sampleRate, 0.02);
    dryWetSmoothed.setCurrentAndTargetValue (apvts.getRawParameterValue (ID::dryWet)->load());
    outputGainSmoothed.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (apvts.getRawParameterValue (ID::outputGain)->load()));

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, (juce::uint32) numCh };
    globalFilter.prepare (spec);
    globalFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    globalFilter.setCutoffFrequency (20000.0f);
    globalFilter.setResonance (0.3f);

    // Smooth over ~10ms; coefficients are only re-pushed into globalFilter every
    // filterCutoffUpdateInterval samples (see applyGlobalModulators), not every sample.
    filterCutoffSmoothed.reset (sampleRate, 0.01);
    filterCutoffSmoothed.setCurrentAndTargetValue (20000.0f);
    filterCutoffUpdateCounter = 0;
    // Cutoff starts at the fully-open ceiling (matches globalFilter's initial 20000Hz cutoff
    // above), so the bypass state starts "on" too -- consistent with the engage/disengage
    // hysteresis in applyGlobalModulators.
    globalFilterBypassed = true;

    // Allocate generously so processBlock never needs to resize on the audio thread, even if
    // the host later calls processBlock with a larger buffer than it declared in prepareToPlay.
    dryScratchMaxChannels = numCh;
    dryScratchMaxSamples = samplesPerBlock;
    dryScratchBuffer.setSize (dryScratchMaxChannels, dryScratchMaxSamples, false, true, true);

    internalClockPpq = 0.0;
}

void StutterAudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool StutterAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;

    return true;
}
#endif

//==============================================================================
void StutterAudioProcessor::updateTransportAndSequence (juce::AudioBuffer<float>& buffer)
{
    const bool hostSyncEnabled = apvts.getRawParameterValue (ID::hostSync)->load() > 0.5f;

    blockSequencer.setEnabled (apvts.getRawParameterValue (ID::sequencerOn)->load() > 0.5f);

    double bpm = apvts.getRawParameterValue (ID::internalBpm)->load();
    double ppqAtBlockStart = internalClockPpq;
    bool usingHostSync = false;

    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            const bool hostIsPlaying = position->getIsPlaying();

            if (hostSyncEnabled && hostIsPlaying)
            {
                if (auto bpmOpt = position->getBpm())
                    if (*bpmOpt > 0.0)
                        bpm = *bpmOpt;

                if (auto ppqOpt = position->getPpqPosition())
                {
                    ppqAtBlockStart = *ppqOpt;
                    usingHostSync = true;
                }
            }
        }
    }

    const double ppqPerSample = (bpm / 60.0) / currentSampleRate;

    // Free-running internal clock: when host sync is off (or the host isn't playing / doesn't
    // report a usable position), keep advancing independently from where we left off, regardless
    // of host transport state changes.
    if (! usingHostSync)
        ppqAtBlockStart = internalClockPpq;

    // v2: the block sequencer drives the audio. It reads the scene the gesture layer has
    // selected, so a MIDI note changes what is playing rather than merely what is displayed.
    //
    // A null scene (nothing published, or an index with no data) leaves the buffer untouched
    // rather than silencing it -- an unmapped note should be inert, not a dropout.
    if (const auto* scene = sceneStore.get (gestureEngine.getActiveScene()))
    {
        if (scene->populated)
        {
            // Chain order can change with the scene, and sorting per sample would be waste;
            // doing it here costs one pass per block.
            if (lastChainOrderScene != gestureEngine.getActiveScene())
            {
                blockSequencer.updateChainOrder (*scene);
                lastChainOrderScene = gestureEngine.getActiveScene();
            }

            blockSequencer.processBlock (buffer, captureBuffer, *scene,
                                         ppqAtBlockStart, ppqPerSample, &modulationEngine);
        }
    }

    // Advance internal free-running clock for next block regardless (so it stays live when host stops)
    internalClockPpq = ppqAtBlockStart + ppqPerSample * (double) buffer.getNumSamples();

    displayBpm.store (bpm, std::memory_order_relaxed);
    displayHostSynced.store (usingHostSync, std::memory_order_relaxed);

    // Store phase advance rate for global modulators (reuse ppqPerSample-derived rate)
    lastKnownPpq = ppqAtBlockStart;
    lastPpqPerSample = ppqPerSample;
}

void StutterAudioProcessor::applyGlobalModulators (juce::AudioBuffer<float>& buffer)
{
    // Control-rate interval (in samples) at which the global filter's cutoff coefficients are
    // actually recalculated; see the comment at the filter modulator block below.
    constexpr int filterCutoffUpdateInterval = 32;

    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    if (numSamples <= 0)
        return;

    auto& volumeCurve = curves[(size_t) ModTarget::Volume];
    auto& filterCurve = curves[(size_t) ModTarget::Filter];
    auto& panCurve = curves[(size_t) ModTarget::Pan];

    auto cyclesPerPpqQuarter = [] (int syncIndex) -> double
    {
        // syncIndex maps 1/1 .. 1/16 bar-length cycles (in quarter notes: 1 bar = 4 quarter notes)
        static const double barFractionTable[] = { 4.0, 2.0, 1.0, 0.5, 0.25 }; // 1/1,1/2,1/4,1/8,1/16 (in quarter notes per cycle)
        constexpr int n = (int) (sizeof (barFractionTable) / sizeof (double));
        syncIndex = juce::jlimit (0, n - 1, syncIndex);
        return 1.0 / barFractionTable[syncIndex];
    };

    for (int n = 0; n < numSamples; ++n)
    {
        const double ppq = lastKnownPpq + lastPpqPerSample * (double) n;

        float* samples[8] = {};
        for (int c = 0; c < numCh && c < 8; ++c)
            samples[c] = buffer.getWritePointer (c) + n;

        // Volume modulator
        if (volumeCurve.isEnabled())
        {
            const double cyclesPerQuarter = cyclesPerPpqQuarter (volumeCurve.getSyncDivision());
            const float phase = (float) std::fmod (ppq * cyclesPerQuarter, 1.0);
            const float modValue = volumeCurve.getValueAtPhase (phase); // 0..1
            const float gain = modValue * 2.0f; // 0..1 maps to 0..2x gain, 0.5 = unity
            for (int c = 0; c < numCh && c < 8; ++c)
                samples[c][0] *= gain;
        }

        // Pan modulator (only meaningful for stereo)
        if (panCurve.isEnabled() && numCh >= 2)
        {
            const double cyclesPerQuarter = cyclesPerPpqQuarter (panCurve.getSyncDivision());
            const float phase = (float) std::fmod (ppq * cyclesPerQuarter, 1.0);
            const float modValue = panCurve.getValueAtPhase (phase); // 0..1, 0.5 = center
            const float panPos = (modValue - 0.5f) * 2.0f; // -1..1
            const float leftGain = panPos <= 0.0f ? 1.0f : 1.0f - panPos;
            const float rightGain = panPos >= 0.0f ? 1.0f : 1.0f + panPos;
            samples[0][0] *= leftGain;
            samples[1][0] *= rightGain;
        }

        // Filter modulator: sweeps global filter cutoff. The target cutoff is recomputed every
        // sample (cheap: just curve lookup + pow), but it is only pushed into the SVF's
        // setCutoffFrequency() (which recalculates internal coefficients) once every
        // filterCutoffUpdateInterval samples. In between, filterCutoffSmoothed interpolates the
        // *value* linearly, and we re-push it every interval so the filter's actual coefficients
        // step smoothly rather than recomputing every sample (removes zipper noise + CPU cost of
        // per-sample coefficient recalculation).
        if (filterCurve.isEnabled())
        {
            const double cyclesPerQuarter = cyclesPerPpqQuarter (filterCurve.getSyncDivision());
            const float phase = (float) std::fmod (ppq * cyclesPerQuarter, 1.0);
            const float modValue = filterCurve.getValueAtPhase (phase); // 0..1, 1.0 = neutral/no-op
            const float cutoffHz = 200.0f * std::pow (100.0f, modValue); // 200Hz .. 20kHz exponential
            filterCutoffSmoothed.setTargetValue (juce::jlimit (20.0f, 20000.0f, cutoffHz));

            if (filterCutoffUpdateCounter <= 0)
            {
                globalFilter.setCutoffFrequency (filterCutoffSmoothed.getCurrentValue());
                filterCutoffUpdateCounter = filterCutoffUpdateInterval;
            }
            --filterCutoffUpdateCounter;
            filterCutoffSmoothed.skip (1);

            // At/near the fully-open cutoff (modValue ~1.0, i.e. Init/neutral state, or any
            // curve that momentarily reaches the top of its range) the SVF's cutoff sits right
            // at the 20kHz ceiling -- mathematically near-transparent, but close enough to
            // Nyquist at common sample rates that its resonance (0.3) can still leave a faint
            // high-frequency response ripple. Since the whole point of "neutral" is zero audible
            // effect, bypass the filter outright once its target cutoff is within a hair of the
            // ceiling rather than actually running the near-transparent-but-not-quite coefficients.
            //
            // Two separate thresholds (rather than one) give this hysteresis: once bypassed, the
            // filter only re-engages when the cutoff drops meaningfully below the ceiling
            // (engageCutoffHz), and once engaged, it only bypasses again once it's very close to
            // the ceiling (disengageCutoffHz). Without this gap, a smoothed cutoff hovering right
            // at a single threshold could flip the bypass on/off every sample (chattering).
            constexpr float engageCutoffHz = 19950.0f;    // below this: filter turns ON
            constexpr float disengageCutoffHz = 19999.0f; // at/above this: filter is bypassed
            const float currentCutoff = filterCutoffSmoothed.getCurrentValue();

            if (globalFilterBypassed)
            {
                if (currentCutoff < engageCutoffHz)
                {
                    // Bypass -> engage transition: the SVF's internal integrator state (s1/s2)
                    // was never updated while bypassed, so it's stale relative to the signal
                    // that's about to start flowing through it again. Resetting here (state
                    // clear only, RT-safe per JUCE's StateVariableTPTFilter::reset()) avoids
                    // resuming from old state and producing a transient click.
                    globalFilter.reset();
                    globalFilterBypassed = false;
                }
            }
            else
            {
                if (currentCutoff >= disengageCutoffHz)
                    globalFilterBypassed = true;
            }

            if (! globalFilterBypassed)
            {
                for (int c = 0; c < numCh && c < 8; ++c)
                    samples[c][0] = globalFilter.processSample (c, samples[c][0]);
            }
        }
    }
}

void StutterAudioProcessor::applyDryWetAndGain (const juce::AudioBuffer<float>& dryBuffer, juce::AudioBuffer<float>& wetBuffer)
{
    // Guard against dryBuffer being shorter than wetBuffer (can only happen in the degraded
    // path where the host sent a bigger block than prepareToPlay declared).
    const int numSamples = juce::jmin (wetBuffer.getNumSamples(), dryBuffer.getNumSamples());
    const int numCh = wetBuffer.getNumChannels();

    dryWetSmoothed.setTargetValue (apvts.getRawParameterValue (ID::dryWet)->load());
    outputGainSmoothed.setTargetValue (
        juce::Decibels::decibelsToGain (apvts.getRawParameterValue (ID::outputGain)->load()));

    for (int n = 0; n < numSamples; ++n)
    {
        const float mix = dryWetSmoothed.getNextValue();
        const float gain = outputGainSmoothed.getNextValue();

        // The gesture gate collapses the WET signal toward dry rather than toward silence,
        // and it is applied here rather than by muting the sequencer. Both choices matter:
        // gating to dry means releasing a note leaves the source audible instead of dropping
        // a hole in the track, and gating with a ramp on the mix means every transition is
        // click-free by construction rather than by care. In Auto mode this sits at 1.0 and
        // the expression below reduces to exactly what v1 computed.
        const float gate = gestureEngine.nextGateGain();
        const float effectiveMix = mix * gate;

        for (int c = 0; c < numCh; ++c)
        {
            const float dry = dryBuffer.getReadPointer (juce::jmin (c, dryBuffer.getNumChannels() - 1))[n];
            const float wet = wetBuffer.getReadPointer (c)[n];
            const float mixed = dry + effectiveMix * (wet - dry);
            wetBuffer.getWritePointer (c)[n] = mixed * gain;
        }
    }
}

void StutterAudioProcessor::processChunk (juce::AudioBuffer<float>& chunk, const juce::MidiBuffer& chunkMidi)
{
    // Capture the (dry) input into the always-on ring buffer. This must happen per-chunk (not
    // once for the whole host block) because StepSequencer::processBlock() anchors its reads to
    // CaptureBuffer::getTotalWritten() *immediately after* writing exactly this chunk's samples
    // -- see the comment there. Writing the whole block up front and then processing chunks
    // against it would leave every chunk but the last reading a stale/incorrect anchor.
    captureBuffer.write (chunk);

    // Keep an untouched copy of the dry signal for the final dry/wet mix. dryScratchBuffer is
    // sized generously in prepareToPlay() and is never resized here (zero heap activity on the
    // audio thread); processBlock() guarantees chunk never exceeds dryScratchBuffer's capacity
    // (see the chunking loop there), so this copy always covers the chunk in full -- no sample
    // is ever silently dropped, even when the host sends a block larger than it declared in
    // prepareToPlay.
    const int chunkChannels = chunk.getNumChannels();
    const int chunkSamples = chunk.getNumSamples();
    jassert (chunkChannels <= dryScratchMaxChannels && chunkSamples <= dryScratchMaxSamples);

    for (int c = 0; c < chunkChannels; ++c)
        dryScratchBuffer.copyFrom (c, 0, chunk, c, 0, chunkSamples);

    juce::AudioBuffer<float> dryView (dryScratchBuffer.getArrayOfWritePointers(), chunkChannels, chunkSamples);

    // 0. Gesture layer. Runs BEFORE the sequencer so a note arriving in this chunk can change
    //    the active scene before the chunk is rendered, rather than one chunk late.
    {
        const auto* scene = sceneStore.get (gestureEngine.getActiveScene());
        gestureEngine.currentPatternBeats = scene != nullptr ? scene->beats : 4;
        const auto releaseMode = scene != nullptr ? scene->releaseMode : stutter::ReleaseMode::OnGrid;
        gestureEngine.processMidi (chunkMidi, chunkSamples, lastKnownPpq, lastPpqPerSample, releaseMode);
    }

    // 1. Transport sync + step sequencer (lane effects read from captureBuffer, write into `chunk`)
    updateTransportAndSequence (chunk);

    // 2. Global modulators (Volume / Filter / Pan curves)
    applyGlobalModulators (chunk);

    // 3. Dry/Wet mix + output gain
    applyDryWetAndGain (dryView, chunk);
}

void StutterAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // dryScratchMaxSamples is only set (non-zero) by prepareToPlay(); dryScratchBuffer is sized
    // from it, and processChunk()/the chunking loop below both assume it's a valid, non-zero
    // capacity. Guard against a host calling processBlock() before prepareToPlay() (or after
    // releaseResources() without a matching re-prepare) rather than dividing by / chunking into
    // a zero-sized buffer.
    if (dryScratchMaxSamples == 0)
        return;

    const int totalNumInputChannels = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();

    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // dryScratchBuffer is allocated in prepareToPlay() for up to dryScratchMaxSamples and is
    // never resized here (zero heap activity on the audio thread). If the host sends a block
    // larger than that (blockSize increased after prepareToPlay, or a host that ignores the
    // declared maximum), split it into dryScratchMaxSamples-sized chunks and run each one
    // through the full chain (capture write -> transport/sequencer -> modulators -> dry/wet) in
    // turn -- every sample still gets processed (no audio dropped), and each chunk's
    // transport/PPQ advance continues exactly where the previous chunk left off
    // (updateTransportAndSequence() advances the shared internalClockPpq member once per chunk,
    // and lastKnownPpq/lastPpqPerSample carry the host-synced position forward the same way), so
    // splitting a block into chunks is transparent to the sequencer/curve timeline.
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const int chunkCapacity = juce::jmax (1, dryScratchMaxSamples);

    int offset = 0;
    while (offset < numSamples)
    {
        const int n = juce::jmin (chunkCapacity, numSamples - offset);

        juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), numChannels, offset, n);

        // Slice this chunk's MIDI, rebasing event offsets to the chunk. Without the rebase a
        // note landing late in a large host block would appear at the same offset in every
        // chunk, so quantization would compute the wrong target position.
        juce::MidiBuffer chunkMidi;
        for (const auto meta : midiMessages)
            if (meta.samplePosition >= offset && meta.samplePosition < offset + n)
                chunkMidi.addEvent (meta.getMessage(), meta.samplePosition - offset);

        processChunk (chunk, chunkMidi);

        offset += n;
    }

    // Consume the MIDI rather than forwarding it: this is an audio effect, and a host that
    // received these notes back would double-trigger anything chained after it.
    midiMessages.clear();
}

//==============================================================================
juce::AudioProcessorEditor* StutterAudioProcessor::createEditor()
{
    return new StutterAudioProcessorEditor (*this);
}

//==============================================================================
void StutterAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    // Write v2 schema version so setStateInformation can distinguish from v1 state.
    state.setProperty (stutter::SceneIDs::version, stutter::stateSchemaVersion, nullptr);

    // Attach structural (non-parameter) data: scenes + curves.
    state.removeChild (state.getChildWithName (ID::curvesNode), nullptr);
    state.removeChild (state.getChildWithName (stutter::SceneIDs::scenesNode), nullptr);

    // The scene document is the authority here, not the store: the store holds the last
    // *published* bank, so saving from it would silently drop edits made since the last
    // publish. setStateInformation feeds this same node straight back to the store.
    if (sceneDocument != nullptr)
        state.appendChild (sceneDocument->getState().createCopy(), nullptr);

    // Which scene was live, so reopening a project returns to it rather than to whichever
    // scene happens to come first in the bank.
    state.setProperty (stutter::SceneIDs::activeScene, gestureEngine.getActiveScene(), nullptr);


    juce::ValueTree curvesTree (ID::curvesNode);
    static const juce::Identifier curveNames[] = { { "Volume" }, { "Filter" }, { "Pan" } };
    for (size_t i = 0; i < curves.size(); ++i)
    {
        auto curveTree = curves[i].toValueTree();
        curveTree.setProperty (ID::propName, curveNames[i].toString(), nullptr);
        curvesTree.appendChild (curveTree, nullptr);
    }
    state.appendChild (curvesTree, nullptr);

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void StutterAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState == nullptr)
        return;

    auto newState = juce::ValueTree::fromXml (*xmlState);
    if (! newState.isValid())
        return;

    // Version guard: v1 state has no version property, so getProperty yields 1.
    // Reject older state rather than half-applying an incompatible tree.
    const int version = (int) newState.getProperty (stutter::SceneIDs::version, 1);
    if (version < stutter::stateSchemaVersion)
    {
        loadInitState();
        return;
    }

    auto scenesNode = newState.getChildWithName (stutter::SceneIDs::scenesNode);

    if (scenesNode.isValid())
    {
        // Restore through the document, which republishes to the store. Loading the store
        // directly would leave the editor's tree empty, so the grid would show nothing while
        // the audio path played the restored scenes, and the next UI edit would overwrite
        // them from that empty tree.
        if (sceneDocument != nullptr)
            sceneDocument->replaceState (scenesNode);
        else
            sceneStore.rebuildFromTree (scenesNode);

        // Land on a scene that actually exists. The factory banks map their scenes to MIDI
        // notes from C4 up, so a freshly loaded bank would otherwise sit on empty scene 0 and
        // make no sound at all until the user happened to play the right note -- the plugin
        // looking broken on first contact. An explicitly saved activeScene wins, so reopening
        // a project still returns to whatever the user was on.
        const int savedActive = (int) newState.getProperty (stutter::SceneIDs::activeScene, -1);
        int target = savedActive;

        if (target < 0 || sceneStore.get (target) == nullptr || ! sceneStore.get (target)->populated)
        {
            target = -1;
            for (int i = 0; i < stutter::maxScenes && target < 0; ++i)
                if (const auto* s = sceneStore.get (i))
                    if (s->populated && s->hasAnyBlocks())
                        target = i;
        }

        if (target >= 0)
            gestureEngine.setActiveScene (target);
    }

    // The v1 Curves node is still read: the three global curve modulators
    // (Volume/Filter/Pan) predate the routable matrix and remain the way those three
    // targets are shaped, so presets carrying them must still load. The v1 Sequencer node
    // is looked up only to be stripped below -- nothing consumes it since the block
    // sequencer took over.
    auto sequencerTree = newState.getChildWithName (ID::sequencerNode);
    auto curvesTree = newState.getChildWithName (ID::curvesNode);
    juce::ignoreUnused (sequencerTree);

    // Strip structural nodes before handing off to APVTS (it only expects parameter children)
    auto paramsOnlyState = newState.createCopy();
    paramsOnlyState.removeChild (paramsOnlyState.getChildWithName (ID::sequencerNode), nullptr);
    paramsOnlyState.removeChild (paramsOnlyState.getChildWithName (ID::curvesNode), nullptr);

    apvts.replaceState (paramsOnlyState);

    // Always route through fromValueTree(), even when sequencerTree/curvesTree (or an individual
    // curve within it) is missing -- StepSequencer::fromValueTree() clears the grid up front, and
    // CurveModulator::fromValueTree() resets to its neutral default on an invalid tree, so a
    // preset that omits this structural data (old/hand-edited user presets, presets that don't
    // touch a given curve, etc.) always yields a full reset rather than leaving residue from
    // whatever was previously loaded.

    static const juce::Identifier curveNames[] = { { "Volume" }, { "Filter" }, { "Pan" } };
    for (size_t i = 0; i < curves.size(); ++i)
    {
        juce::ValueTree matchedCurve; // invalid by default -> fromValueTree() resets to neutral
        if (curvesTree.isValid())
        {
            for (int c = 0; c < curvesTree.getNumChildren(); ++c)
            {
                auto child = curvesTree.getChild (c);
                if (child.hasType (ID::curveNode)
                    && child.getProperty (ID::propName).toString() == curveNames[i].toString())
                {
                    matchedCurve = child;
                    break;
                }
            }
        }
        curves[i].fromValueTree (matchedCurve);
    }
}

void StutterAudioProcessor::timerCallback()
{
    sceneStore.collectGarbage();
    mirrorActiveSceneToApvts();
}

void StutterAudioProcessor::mirrorActiveSceneToApvts()
{
    // The audio thread only ever flags which scene became active; it never touches APVTS.
    // This runs on the message thread and does the actual parameter writes, which is what
    // keeps a MIDI-triggered scene change off the audio thread's critical path.
    const int scene = gestureEngine.consumePendingMirror();
    if (scene < 0 || sceneDocument == nullptr)
        return;

    auto sceneTree = sceneDocument->ensureScene (scene);
    if (! sceneTree.isValid())
        return;

    auto laneParams = sceneTree.getChildWithName (stutter::SceneIDs::laneParams);

    // Suppress the write-back listener for the duration. Without this the parameter changes
    // below would be read as user edits and written straight back into the scene we are
    // mirroring FROM -- a feedback loop that would also mark every scene switch as a
    // document edit.
    const juce::ScopedValueSetter<bool> guard (suppressParamWriteback, true);

    for (int lane = 0; lane < stutter::maxLanes; ++lane)
    {
        auto* effect = blockSequencer.getLaneEffect (lane);
        if (effect == nullptr)
            continue;

        const auto set = effect->getParamDescriptors();

        // Locate this lane's stored values, if the scene has any. A scene that omits the
        // lane mirrors the descriptor defaults instead, matching what the audio thread is
        // actually rendering (see SceneSchema::setLaneDefaults).
        juce::ValueTree laneNode;
        if (laneParams.isValid())
            for (int i = 0; i < laneParams.getNumChildren(); ++i)
                if ((int) laneParams.getChild (i).getProperty (stutter::SceneIDs::index, -1) == lane)
                    laneNode = laneParams.getChild (i);

        for (int p = 0; p < set.count && p < stutter::maxParamsPerLane; ++p)
        {
            float value = set[p].defaultValue;

            if (laneNode.isValid())
                for (int c = 0; c < laneNode.getNumChildren(); ++c)
                {
                    const auto pt = laneNode.getChild (c);
                    if (pt.hasType (stutter::SceneIDs::param)
                        && (int) pt.getProperty (stutter::SceneIDs::paramIndexProp, -1) == p)
                        value = (float) pt.getProperty (stutter::SceneIDs::value, value);
                }

            if (auto* param = apvts.getParameter (ID::lanePrefix (lane) + set[p].id))
            {
                const auto& range = apvts.getParameterRange (ID::lanePrefix (lane) + set[p].id);
                param->setValueNotifyingHost (range.convertTo0to1 (value));
            }
        }
    }

    mirroredScene = scene;
}

void StutterAudioProcessor::loadInitState()
{
    // Reset every parameter to the value declared in the layout, then clear the structural
    // data. Copying the state and replacing it with itself would be a no-op -- the
    // parameters have to be driven back to their defaults explicitly, or a rejected load
    // would leave whatever the user last had dialled in.
    for (auto* param : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            ranged->setValueNotifyingHost (ranged->getDefaultValue());

    // An invalid tree makes each curve fall back to its own neutral value (see
    // ID::neutralValueForCurve) -- the same path Test C exercises for malformed state.
    for (size_t i = 0; i < curves.size(); ++i)
        curves[i].fromValueTree (juce::ValueTree {});

    // Scenes are structural, so resetting parameters does not touch them. Without this an
    // Init -- including the fallback taken when a state load is rejected -- would leave the
    // previous session's blocks playing under a patch claiming to be empty.
    if (sceneDocument != nullptr)
    {
        juce::ValueTree empty (stutter::SceneIDs::scenesNode);
        sceneDocument->replaceState (empty);
        sceneDocument->ensureScene (0);
        sceneDocument->publish();
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StutterAudioProcessor();
}
