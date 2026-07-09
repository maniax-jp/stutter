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

using namespace stutter;

namespace
{
juce::String laneParamId (int lane, const juce::String& name) { return ID::lanePrefix (lane) + name; }

juce::StringArray rateChoices()
{
    return { "1/4", "1/8", "1/16", "1/32", "1/64", "1/4T", "1/8T", "1/16T", "1/4.", "1/8.", "1/16." };
}
} // namespace

//==============================================================================
StutterAudioProcessor::StutterAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    sequencer.setLaneEffect (laneStutter,   std::make_unique<StutterEffect> (apvts, laneStutter));
    sequencer.setLaneEffect (laneTapeStop,  std::make_unique<TapeStopEffect> (apvts, laneTapeStop));
    sequencer.setLaneEffect (laneTapeStart, std::make_unique<TapeStartEffect> (apvts, laneTapeStart));
    sequencer.setLaneEffect (laneReverse,   std::make_unique<ReverseEffect> (apvts, laneReverse));
    sequencer.setLaneEffect (laneRepitch,   std::make_unique<RepitchEffect> (apvts, laneRepitch));
    sequencer.setLaneEffect (laneGate,      std::make_unique<GateEffect> (apvts, laneGate));
    sequencer.setLaneEffect (laneFilter,    std::make_unique<FilterEffect> (apvts, laneFilter));
    sequencer.setLaneEffect (laneCrush,     std::make_unique<CrushEffect> (apvts, laneCrush));

    presetManager = std::make_unique<stutter::PresetManager> (*this);
}

StutterAudioProcessor::~StutterAudioProcessor() = default;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout StutterAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // ---- Global ----
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ID::dryWet, 1 }, "Dry/Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ID::outputGain, 1 }, "Output Gain",
        juce::NormalisableRange<float> (-24.0f, 24.0f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ID::sequencerOn, 1 }, "Sequencer On", true));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ID::hostSync, 1 }, "Host Sync", true));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ID::internalBpm, 1 }, "Internal BPM",
        juce::NormalisableRange<float> (40.0f, 240.0f), 120.0f));

    auto addRateChoice = [&params] (int lane, const juce::String& name, const juce::String& label, int defaultIndex)
    {
        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { laneParamId (lane, name), 1 }, label, rateChoices(), defaultIndex));
    };

    auto addFloat = [&params] (int lane, const juce::String& name, const juce::String& label,
                                juce::NormalisableRange<float> range, float def, const juce::String& unit = {})
    {
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { laneParamId (lane, name), 1 }, label, range, def,
            juce::AudioParameterFloatAttributes().withLabel (unit)));
    };

    // ---- Lane 0: Stutter ----
    addRateChoice (StutterAudioProcessor::laneStutter, ID::stutterRate, "Stutter Rate", 2);
    addFloat (StutterAudioProcessor::laneStutter, ID::stutterDecay, "Stutter Decay",
              { 0.0f, 1.0f }, 0.0f);
    addFloat (StutterAudioProcessor::laneStutter, ID::stutterPitchSlide, "Stutter Pitch Slide",
              { -24.0f, 24.0f }, 0.0f, "st");

    // ---- Lane 1: TapeStop ----
    addFloat (StutterAudioProcessor::laneTapeStop, ID::tapeStopCurve, "TapeStop Curve", { 0.0f, 1.0f }, 0.5f);
    addFloat (StutterAudioProcessor::laneTapeStop, ID::tapeStopTime, "TapeStop Time", { 0.0f, 1.0f }, 0.5f);

    // ---- Lane 2: TapeStart ----
    addFloat (StutterAudioProcessor::laneTapeStart, ID::tapeStartCurve, "TapeStart Curve", { 0.0f, 1.0f }, 0.5f);
    addFloat (StutterAudioProcessor::laneTapeStart, ID::tapeStartTime, "TapeStart Time", { 0.0f, 1.0f }, 0.5f);

    // ---- Lane 3: Reverse ----
    addRateChoice (StutterAudioProcessor::laneReverse, ID::reverseSliceLen, "Reverse Slice Length", 2);

    // ---- Lane 4: Repitch ----
    addFloat (StutterAudioProcessor::laneRepitch, ID::repitchSemitones, "Repitch Semitones",
              { -24.0f, 24.0f }, -12.0f, "st");
    addFloat (StutterAudioProcessor::laneRepitch, ID::repitchSlide, "Repitch Slide", { 0.0f, 1.0f }, 0.0f);

    // ---- Lane 5: Gate ----
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { laneParamId (StutterAudioProcessor::laneGate, ID::gateRate), 1 }, "Gate Rate",
        juce::StringArray { "1/1", "1/2", "1/3", "1/4", "1/6", "1/8", "1/12", "1/16" }, 3));
    addFloat (StutterAudioProcessor::laneGate, ID::gateDuty, "Gate Duty", { 0.01f, 0.99f }, 0.5f);
    addFloat (StutterAudioProcessor::laneGate, ID::gateShape, "Gate Shape", { 0.0f, 1.0f }, 0.0f);

    // ---- Lane 6: Filter ----
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { laneParamId (StutterAudioProcessor::laneFilter, ID::filterType), 1 }, "Filter Type",
        juce::StringArray { "Low Pass", "Band Pass", "High Pass" }, 0));
    addFloat (StutterAudioProcessor::laneFilter, ID::filterCutoff, "Filter Cutoff",
              juce::NormalisableRange<float> (20.0f, 20000.0f, 1.0f, 0.3f), 1000.0f, "Hz");
    addFloat (StutterAudioProcessor::laneFilter, ID::filterResonance, "Filter Resonance", { 0.0f, 0.99f }, 0.2f);
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { laneParamId (StutterAudioProcessor::laneFilter, ID::filterLfoRate), 1 }, "Filter LFO Rate",
        juce::StringArray { "1/4", "1/2", "1/1", "2/1", "4/1", "8/1" }, 2));
    addFloat (StutterAudioProcessor::laneFilter, ID::filterLfoDepth, "Filter LFO Depth", { 0.0f, 1.0f }, 0.0f);

    // ---- Lane 7: Crush ----
    addFloat (StutterAudioProcessor::laneCrush, ID::crushBitDepth, "Crush Bit Depth", { 1.0f, 16.0f, 1.0f }, 16.0f, "bit");
    addFloat (StutterAudioProcessor::laneCrush, ID::crushRateDiv, "Crush Rate Div", { 0.0f, 1.0f }, 0.0f);

    return { params.begin(), params.end() };
}

//==============================================================================
const juce::String StutterAudioProcessor::getName() const { return JucePlugin_Name; }
bool StutterAudioProcessor::acceptsMidi() const { return false; }
bool StutterAudioProcessor::producesMidi() const { return false; }
bool StutterAudioProcessor::isMidiEffect() const { return false; }
double StutterAudioProcessor::getTailLengthSeconds() const { return 0.0; }

//==============================================================================
void StutterAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    const int numCh = juce::jmax (getTotalNumInputChannels(), getTotalNumOutputChannels());

    captureBuffer.prepare (sampleRate, numCh, 2.5);
    sequencer.prepare (sampleRate, numCh);

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

    sequencer.setEnabled (apvts.getRawParameterValue (ID::sequencerOn)->load() > 0.5f);

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

    sequencer.processBlock (buffer, captureBuffer, ppqAtBlockStart, ppqPerSample);

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

        for (int c = 0; c < numCh; ++c)
        {
            const float dry = dryBuffer.getReadPointer (juce::jmin (c, dryBuffer.getNumChannels() - 1))[n];
            const float wet = wetBuffer.getReadPointer (c)[n];
            const float mixed = dry + mix * (wet - dry);
            wetBuffer.getWritePointer (c)[n] = mixed * gain;
        }
    }
}

void StutterAudioProcessor::processChunk (juce::AudioBuffer<float>& chunk)
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
    midiMessages.clear();

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
        processChunk (chunk);

        offset += n;
    }
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

    // Attach structural (non-parameter) data: step grid + curves
    state.removeChild (state.getChildWithName (ID::sequencerNode), nullptr);
    state.removeChild (state.getChildWithName (ID::curvesNode), nullptr);

    state.appendChild (sequencer.toValueTree(), nullptr);

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

    auto sequencerTree = newState.getChildWithName (ID::sequencerNode);
    auto curvesTree = newState.getChildWithName (ID::curvesNode);

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
    sequencer.fromValueTree (sequencerTree);

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

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StutterAudioProcessor();
}
