// Offline verification harness for the Stutter noise-fix work.
//
// Instantiates StutterAudioProcessor directly (no plugin wrapper/host involved), drives it
// with a synthetic "loud/quiet alternating" test signal, and renders each lane soloed (all 16
// steps on) for 4 bars at 120 BPM / 48kHz / block=512, with hostSync OFF so the internal
// free-running clock is used. For each lane it writes a WAV file and reports discontinuity
// metrics (max adjacent-sample delta, count of deltas over a threshold, RMS) so the effect of
// the DSP fix can be measured numerically before/after.
//
// Also runs headless regression checks for the Init-preset / curve-residue bugs:
//  (a) a fresh instance's default output matches the dry signal (no audible coloration from
//      curve modulators that should be neutral out of the box)
//  (b) loading "Trance Gate 16th" (which sets a non-neutral Volume curve) and then loading
//      "Init" resets all three curves (Volume/Filter/Pan) to neutral flat + expected enabled
//      state -- i.e. no residue from the previous preset.
//  (c) malformed/incomplete curve-tree fixtures (missing curveNode, <2 Points, a Point missing
//      its "value" property) each fall back to that curve's neutral value rather than leaving
//      stale or garbage state.
//
// Usage: render_test <output-directory>

#include "PluginProcessor.h"
#include "PresetManager.h"
#include "dsp/CurveModulator.h"
#include "dsp/ParameterIDs.h"
#include "dsp/TimingMode.h"
#include "dsp/effects/StutterEffect.h"
#include "dsp/effects/ReverseEffect.h"
#include "dsp/effects/TapeStopEffect.h"
#include "dsp/effects/TapeStartEffect.h"
#include "dsp/effects/RepitchEffect.h"
#include "dsp/effects/GateEffect.h"
#include "dsp/effects/FilterEffect.h"
#include "dsp/effects/CrushEffect.h"
#include "dsp/effects/StretcherEffect.h"
#include "dsp/effects/ShufflerEffect.h"
#include "dsp/effects/DelayEffect.h"
#include "dsp/effects/DistortionEffect.h"
#include "dsp/BlockSequencer.h"
#include "dsp/GestureEngine.h"
#include "dsp/ModulationEngine.h"
#include "state/SceneSchema.h"
#include "state/SceneSnapshot.h"
#include "state/SceneStore.h"
#include "state/SceneDocument.h"
#include "FactoryScenes.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdio>
#include <cmath>
#include <memory>
#include <vector>

namespace
{

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr double kBpm = 120.0;
constexpr int kNumBars = 4;
constexpr double kClickThreshold = 0.3; // NOISE_FIX.md pass/fail bar: severe-click adjacent-sample delta

// A single continuous 220Hz sine at full amplitude has a maximum possible sample-to-sample
// delta of ~0.0288 (2*pi*f/sr). Any splice/discontinuity artifact shows up as a delta well
// above that clean baseline, well before it would ever reach the coarser 0.3 "severe click"
// bar above -- so this second, tighter threshold is what actually discriminates the
// moving-writePos-relative-read bug (which produces small-phase-jump buzz, not full-scale
// clicks, when the source material is a single bounded tone). Both metrics are reported.
constexpr double kCleanSineMaxDelta = juce::MathConstants<double>::twoPi * 220.0 / kSampleRate;
constexpr double kDiscontinuityThreshold = kCleanSineMaxDelta * 2.0; // 2x clean baseline, with margin

struct LaneSpec
{
    int laneIndex;
    const char* name;
};

// Lane construction order: 0 Stutter, 1 TapeStop, 2 TapeStart, 3 Reverse, 4 Repitch, 5 Gate, 6 Filter, 7 Crush
static const LaneSpec kLanes[] = {
    { StutterAudioProcessor::laneStutter,   "Stutter" },
    { StutterAudioProcessor::laneTapeStop,  "TapeStop" },
    { StutterAudioProcessor::laneTapeStart, "TapeStart" },
    { StutterAudioProcessor::laneReverse,   "Reverse" },
    { StutterAudioProcessor::laneRepitch,   "Repitch" },
    { StutterAudioProcessor::laneGate,      "Gate" },
    { StutterAudioProcessor::laneFilter,    "Filter" },
    { StutterAudioProcessor::laneCrush,     "Crush" },
};

struct Metrics
{
    float maxAdjacentDelta = 0.0f;
    int severeClickCount = 0;      // deltas > kClickThreshold (0.3, NOISE_FIX.md's literal bar)
    int discontinuityCount = 0;    // deltas > kDiscontinuityThreshold (tighter, discriminates splice noise)
    double rms = 0.0;
    int numSamples = 0;
};

Metrics analyze (const juce::AudioBuffer<float>& buf, double severeThreshold, double discontinuityThreshold)
{
    Metrics m;
    const int n = buf.getNumSamples();
    const int ch = buf.getNumChannels();
    m.numSamples = n;

    double sumSq = 0.0;
    for (int c = 0; c < ch; ++c)
    {
        const float* d = buf.getReadPointer (c);
        float prev = d[0];
        for (int i = 0; i < n; ++i)
        {
            const float v = d[i];
            sumSq += (double) v * (double) v;
            if (i > 0)
            {
                const float delta = std::abs (v - prev);
                if (delta > m.maxAdjacentDelta)
                    m.maxAdjacentDelta = delta;
                if (delta > (float) severeThreshold)
                    ++m.severeClickCount;
                if (delta > (float) discontinuityThreshold)
                    ++m.discontinuityCount;
            }
            prev = v;
        }
    }
    m.rms = std::sqrt (sumSq / (double) juce::jmax (1, n * ch));
    return m;
}

// 220Hz sine, amplitude alternating 1.0 / 0.05 every beat (quarter note) at 120bpm -- the
// "loud material spliced with quiet material" scenario from the user report.
void fillTestSignal (juce::AudioBuffer<float>& buf, double sampleRate, double bpm)
{
    constexpr double freqHz = 220.0;
    const double secondsPerBeat = 60.0 / bpm;
    const int samplesPerBeat = (int) std::round (secondsPerBeat * sampleRate);

    const int n = buf.getNumSamples();
    for (int c = 0; c < buf.getNumChannels(); ++c)
    {
        float* d = buf.getWritePointer (c);
        for (int i = 0; i < n; ++i)
        {
            const int beatIndex = i / juce::jmax (1, samplesPerBeat);
            const float amp = (beatIndex % 2 == 0) ? 1.0f : 0.05f;
            d[i] = amp * (float) std::sin (juce::MathConstants<double>::twoPi * freqHz * (double) i / sampleRate);
        }
    }
}

// internalBpm is APVTS-owned (see docs/ISSUES.md 2.2); processBlock reads it via
// getRawParameterValue(), so tests must set it the same way a host/preset would.
void setInternalBpm (StutterAudioProcessor& processor, double bpm)
{
    if (auto* param = processor.getAPVTS().getParameter (stutter::ID::internalBpm))
    {
        const auto& range = processor.getAPVTS().getParameterRange (stutter::ID::internalBpm);
        param->setValueNotifyingHost (range.convertTo0to1 ((float) bpm));
    }
}

double rmsOf (const juce::AudioBuffer<float>& buf)
{
    double sumSq = 0.0;
    const int n = buf.getNumSamples();
    const int ch = buf.getNumChannels();
    for (int c = 0; c < ch; ++c)
    {
        const float* d = buf.getReadPointer (c);
        for (int i = 0; i < n; ++i)
            sumSq += (double) d[i] * (double) d[i];
    }
    return std::sqrt (sumSq / (double) juce::jmax (1, n * ch));
}

// Checks that a curve is "neutral": enabled state as expected, flat value == expectedValue
// across the whole table (sampled), i.e. contributes no audible modulation.
bool isCurveNeutral (const stutter::CurveModulator& c, float expectedValue, const char* label)
{
    bool ok = true;
    for (int i = 0; i <= 16; ++i)
    {
        const float phase = (float) i / 16.0f;
        const float v = c.getValueAtPhase (phase);
        if (std::abs (v - expectedValue) > 1.0e-4f)
        {
            printf ("  FAIL: %s curve not flat at phase %.3f -> %.6f (expected %.6f)\n",
                    label, phase, v, expectedValue);
            ok = false;
        }
    }
    return ok;
}

// (a) Fresh-instance default output must match dry signal (RMS diff < 0.1dB-equivalent).
bool testFreshInstanceIsTransparent()
{
    printf ("\n[Test A] Fresh instance default output vs dry signal\n");

    StutterAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
    processor.prepareToPlay (kSampleRate, kBlockSize);

    auto& apvts = processor.getAPVTS();
    apvts.getParameter (stutter::ID::hostSync)->setValueNotifyingHost (0.0f);
    setInternalBpm (processor, kBpm);
    // Sequencer stays fully OFF (freshly constructed) -- we're isolating the global
    // Volume/Filter/Pan curve modulators, which run regardless of the step sequencer.

    const int totalSamples = (int) std::round (2.0 * kSampleRate); // 2 seconds is plenty
    juce::AudioBuffer<float> source (2, totalSamples);
    fillTestSignal (source, kSampleRate, kBpm);

    juce::AudioBuffer<float> rendered (2, totalSamples);
    rendered.clear();

    int pos = 0;
    juce::MidiBuffer midi;
    while (pos < totalSamples)
    {
        const int n = juce::jmin (kBlockSize, totalSamples - pos);
        juce::AudioBuffer<float> block (2, kBlockSize);
        block.clear();
        for (int c = 0; c < 2; ++c)
            block.copyFrom (c, 0, source, c, pos, n);

        midi.clear();
        processor.processBlock (block, midi);

        for (int c = 0; c < 2; ++c)
            rendered.copyFrom (c, pos, block, c, 0, n);
        pos += n;
    }

    // Skip the first block (filter/smoothing settling from cold state).
    const int analysisStart = juce::jmin (kBlockSize, totalSamples);
    juce::AudioBuffer<float> dryView (source.getArrayOfWritePointers(), 2, analysisStart, totalSamples - analysisStart);
    juce::AudioBuffer<float> wetView (rendered.getArrayOfWritePointers(), 2, analysisStart, totalSamples - analysisStart);

    const double dryRms = rmsOf (dryView);
    const double wetRms = rmsOf (wetView);
    const double dbDiff = 20.0 * std::log10 (juce::jmax (1.0e-12, wetRms) / juce::jmax (1.0e-12, dryRms));

    // Also check per-sample max abs diff, since RMS could mask e.g. a filter that changes
    // spectral content but not overall level.
    double maxAbsDiff = 0.0;
    for (int c = 0; c < 2; ++c)
    {
        const float* d = dryView.getReadPointer (c);
        const float* w = wetView.getReadPointer (c);
        for (int i = 0; i < dryView.getNumSamples(); ++i)
            maxAbsDiff = juce::jmax (maxAbsDiff, (double) std::abs (d[i] - w[i]));
    }

    printf ("  dry RMS=%.6f  wet RMS=%.6f  dB diff=%.4f dB  maxAbsSampleDiff=%.6f\n",
            dryRms, wetRms, dbDiff, maxAbsDiff);

    const bool pass = std::abs (dbDiff) < 0.1 && maxAbsDiff < 0.01;
    printf ("  %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// (b) "Trance Gate 16th" -> "Init" preset transition must reset all 3 curves to neutral.
bool testPresetTransitionResetsCurves()
{
    printf ("\n[Test B] Trance Gate 16th -> Init resets all curves to neutral\n");

    StutterAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
    processor.prepareToPlay (kSampleRate, kBlockSize);

    auto& pm = processor.getPresetManager();
    const auto& presets = pm.getPresets();

    int tranceIdx = -1, initIdx = -1;
    for (int i = 0; i < (int) presets.size(); ++i)
    {
        if (presets[(size_t) i].name == "Trance Gate 16th") tranceIdx = i;
        if (presets[(size_t) i].name == "Init") initIdx = i;
    }

    if (tranceIdx < 0 || initIdx < 0)
    {
        printf ("  FAIL: could not find required presets (Trance=%d, Init=%d)\n", tranceIdx, initIdx);
        return false;
    }

    pm.loadPreset (tranceIdx);
    pm.loadPreset (initIdx);

    bool pass = true;

    auto& volumeCurve = processor.getCurve (stutter::ModTarget::Volume);
    auto& filterCurve = processor.getCurve (stutter::ModTarget::Filter);
    auto& panCurve    = processor.getCurve (stutter::ModTarget::Pan);

    printf ("  Volume: enabled=%d points=%zu\n", volumeCurve.isEnabled(), volumeCurve.getPoints().size());
    printf ("  Filter: enabled=%d points=%zu\n", filterCurve.isEnabled(), filterCurve.getPoints().size());
    printf ("  Pan:    enabled=%d points=%zu\n", panCurve.isEnabled(), panCurve.getPoints().size());

    if (! isCurveNeutral (volumeCurve, stutter::ID::neutralValueForCurve (stutter::ID::curveNameVolume), "Volume")) pass = false;
    if (! isCurveNeutral (filterCurve, stutter::ID::neutralValueForCurve (stutter::ID::curveNameFilter), "Filter")) pass = false; // neutral = fully open (20kHz), not 0.5
    if (! isCurveNeutral (panCurve, stutter::ID::neutralValueForCurve (stutter::ID::curveNamePan), "Pan")) pass = false;

    // Filter must not be silently left "on" at a non-neutral value that audibly colors the
    // signal; since neutral flat value (1.0 = cutoff wide open) makes enabled state irrelevant
    // to the *sound*, we don't hard-require disabled here -- only that IF enabled, it's flat 1.0.
    if (filterCurve.isEnabled() && ! isCurveNeutral (filterCurve, stutter::ID::neutralValueForCurve (stutter::ID::curveNameFilter), "Filter(enabled-check)"))
        pass = false;

    printf ("  %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// (c) Malformed-state regression fixtures: CurveModulator::fromValueTree() must fall back to
// neutral (rather than leaving stale/garbage state) whenever the incoming tree is structurally
// incomplete. Covers three distinct shapes of "missing data" a hand-edited or older-version
// preset XML could plausibly contain:
//   1. Curves node present, but this particular curve's Curve node is entirely absent (invalid
//      tree passed straight through, same path as "Curves node missing altogether").
//   2. Curve node present but with only a single Point child (or none) -- not enough points to
//      define a curve.
//   3. Curve node with >=2 Point children, but a Point is missing its "value" property.
bool testMalformedCurveTreeFixtures()
{
    printf ("\n[Test C] Malformed curve-tree fixtures fall back to neutral\n");
    bool pass = true;

    const float volumeNeutral = stutter::ID::neutralValueForCurve (stutter::ID::curveNameVolume);
    const float filterNeutral = stutter::ID::neutralValueForCurve (stutter::ID::curveNameFilter);

    // Fixture 1: curveNode entirely missing for this curve (an invalid/default-constructed tree,
    // exactly what PluginProcessor::setStateInformation() passes when it can't find a matching
    // <Curve name="..."> child under <Curves>).
    {
        stutter::CurveModulator curve (filterNeutral);
        curve.setPoints ({ { 0.0f, 0.1f, 0.0f }, { 1.0f, 0.9f, 0.5f } }); // give it non-neutral state first
        juce::ValueTree missing; // default-constructed == invalid
        curve.fromValueTree (missing);
        if (! isCurveNeutral (curve, filterNeutral, "Fixture1-Filter(missing curveNode)"))
            pass = false;
        if (curve.getPoints().size() < 2)
        {
            printf ("  FAIL: Fixture1 left curve with <2 points\n");
            pass = false;
        }
    }

    // Fixture 2: Curve node exists but has only one Point child (not enough to define a curve).
    {
        stutter::CurveModulator curve (volumeNeutral);
        curve.setPoints ({ { 0.0f, 0.1f, 0.0f }, { 1.0f, 0.9f, 0.5f } });

        juce::ValueTree curveTree (stutter::ID::curveNode);
        curveTree.setProperty (stutter::ID::propEnabled, true, nullptr);
        curveTree.setProperty (stutter::ID::propSyncDiv, 4, nullptr);
        juce::ValueTree onlyPoint (stutter::ID::pointNode);
        onlyPoint.setProperty (stutter::ID::propPosition, 0.5f, nullptr);
        onlyPoint.setProperty (stutter::ID::propValue, 0.9f, nullptr);
        curveTree.appendChild (onlyPoint, nullptr);

        curve.fromValueTree (curveTree);
        if (! isCurveNeutral (curve, volumeNeutral, "Fixture2-Volume(1 point)"))
            pass = false;
        if (curve.getPoints().size() < 2)
        {
            printf ("  FAIL: Fixture2 left curve with <2 points\n");
            pass = false;
        }
    }

    // Fixture 2b: Curve node exists with zero Point children.
    {
        stutter::CurveModulator curve (volumeNeutral);
        curve.setPoints ({ { 0.0f, 0.1f, 0.0f }, { 1.0f, 0.9f, 0.5f } });

        juce::ValueTree curveTree (stutter::ID::curveNode);
        curveTree.setProperty (stutter::ID::propEnabled, true, nullptr);
        curveTree.setProperty (stutter::ID::propSyncDiv, 4, nullptr);

        curve.fromValueTree (curveTree);
        if (! isCurveNeutral (curve, volumeNeutral, "Fixture2b-Volume(0 points)"))
            pass = false;
    }

    // Fixture 3: Curve node with >=2 points, but one Point is missing its "value" property --
    // must fall back to this curve's neutral value for that point, not JUCE's ValueTree default
    // (0.0), which previously would have been a hardcoded 0.5f fallback baked into
    // CurveModulator::fromValueTree() regardless of which curve it was.
    {
        stutter::CurveModulator curve (filterNeutral);

        juce::ValueTree curveTree (stutter::ID::curveNode);
        curveTree.setProperty (stutter::ID::propEnabled, true, nullptr);
        curveTree.setProperty (stutter::ID::propSyncDiv, 4, nullptr);

        juce::ValueTree pt0 (stutter::ID::pointNode);
        pt0.setProperty (stutter::ID::propPosition, 0.0f, nullptr);
        // propValue deliberately omitted -- should fall back to filterNeutral (1.0), not 0.5.
        pt0.setProperty (stutter::ID::propCurvature, 0.0f, nullptr);
        curveTree.appendChild (pt0, nullptr);

        juce::ValueTree pt1 (stutter::ID::pointNode);
        pt1.setProperty (stutter::ID::propPosition, 1.0f, nullptr);
        pt1.setProperty (stutter::ID::propValue, 0.3f, nullptr);
        pt1.setProperty (stutter::ID::propCurvature, 0.0f, nullptr);
        curveTree.appendChild (pt1, nullptr);

        curve.fromValueTree (curveTree);
        const float v0 = curve.getValueAtPhase (0.0f);
        printf ("  Fixture3: point0 (missing propValue) resolved to %.6f (expected neutral %.6f)\n",
                v0, filterNeutral);
        if (std::abs (v0 - filterNeutral) > 1.0e-4f)
        {
            printf ("  FAIL: Fixture3 missing-propValue point did not fall back to curve's neutral value\n");
            pass = false;
        }
    }

    printf ("  %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// (d) sequencerOn == false must silence step effects entirely (output matches dry passthrough)
// while the global Volume/Filter/Pan curve modulators keep running -- this is the "bypass ==
// curve-only" contract from SPEC (docs/ISSUES.md 2.1). Drives a lane with all 16 steps ON (which
// would otherwise audibly alter the signal) plus a non-neutral, enabled Volume curve, with
// sequencerOn set to false via the APVTS parameter (the only supported way to reach the DSP, per
// 2.1/2.2's "APVTS is the single source of truth" fix).
bool testSequencerOffBypassesStepsButCurvesStillWork()
{
    printf ("\n[Test D] sequencerOn=false silences step lanes (dry match) while curves still modulate\n");
    bool pass = true;

    // --- D1: sequencerOn=false -> output must equal dry passthrough, even with a lane fully ON ---
    {
        StutterAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
        processor.prepareToPlay (kSampleRate, kBlockSize);

        auto& apvts = processor.getAPVTS();
        apvts.getParameter (stutter::ID::hostSync)->setValueNotifyingHost (0.0f);
        setInternalBpm (processor, kBpm);
        apvts.getParameter (stutter::ID::sequencerOn)->setValueNotifyingHost (0.0f); // OFF

        // Make sure the Volume curve is neutral for this sub-test, isolating the step-lane bypass.
        processor.getCurve (stutter::ModTarget::Volume).resetToDefault();
        processor.getCurve (stutter::ModTarget::Filter).resetToDefault();
        processor.getCurve (stutter::ModTarget::Pan).resetToDefault();

        // A fully-active lane, which would be very audible if the bypass did not work.
        auto& doc = processor.getSceneDocument();
        const int sceneIdx = processor.getGestureEngine().getActiveScene();
        for (int d = 0; d < stutter::numSteps; ++d)
            doc.addBlock (sceneIdx, StutterAudioProcessor::laneStutter, d, 1);
        doc.publish();

        const int totalSamples = (int) std::round (1.5 * kSampleRate);
        juce::AudioBuffer<float> source (2, totalSamples);
        fillTestSignal (source, kSampleRate, kBpm);

        juce::AudioBuffer<float> rendered (2, totalSamples);
        rendered.clear();

        int pos = 0;
        juce::MidiBuffer midi;
        while (pos < totalSamples)
        {
            const int n = juce::jmin (kBlockSize, totalSamples - pos);
            juce::AudioBuffer<float> block (2, kBlockSize);
            block.clear();
            for (int c = 0; c < 2; ++c)
                block.copyFrom (c, 0, source, c, pos, n);

            midi.clear();
            processor.processBlock (block, midi);

            for (int c = 0; c < 2; ++c)
                rendered.copyFrom (c, pos, block, c, 0, n);
            pos += n;
        }

        double maxAbsDiff = 0.0;
        for (int c = 0; c < 2; ++c)
        {
            const float* d = source.getReadPointer (c);
            const float* w = rendered.getReadPointer (c);
            for (int i = 0; i < totalSamples; ++i)
                maxAbsDiff = juce::jmax (maxAbsDiff, (double) std::abs (d[i] - w[i]));
        }

        printf ("  D1: sequencerOn=false, lane fully ON -> maxAbsDiff vs dry = %.6f\n", maxAbsDiff);
        if (maxAbsDiff > 1.0e-6)
        {
            printf ("  FAIL: expected exact dry passthrough (step lanes fully bypassed) when sequencerOn=false\n");
            pass = false;
        }
    }

    // --- D2: with sequencerOn=false, the Volume curve modulator must still audibly modulate ---
    {
        StutterAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
        processor.prepareToPlay (kSampleRate, kBlockSize);

        auto& apvts = processor.getAPVTS();
        apvts.getParameter (stutter::ID::hostSync)->setValueNotifyingHost (0.0f);
        setInternalBpm (processor, kBpm);
        apvts.getParameter (stutter::ID::sequencerOn)->setValueNotifyingHost (0.0f); // OFF

        // Non-neutral, enabled Volume curve: SidechainDuck dips well below unity gain.
        auto& volumeCurve = processor.getCurve (stutter::ModTarget::Volume);
        volumeCurve.setEnabled (true);
        volumeCurve.applyPreset ("SidechainDuck");
        volumeCurve.setSyncDivision (2); // 1/4 bar per cycle, several cycles within the render

        // No blocks are added: sequencerOn gates the lanes anyway, so the pattern is
        // irrelevant to what this check measures.

        const int totalSamples = (int) std::round (2.0 * kSampleRate);
        juce::AudioBuffer<float> source (2, totalSamples);
        fillTestSignal (source, kSampleRate, kBpm);

        juce::AudioBuffer<float> rendered (2, totalSamples);
        rendered.clear();

        int pos = 0;
        juce::MidiBuffer midi;
        while (pos < totalSamples)
        {
            const int n = juce::jmin (kBlockSize, totalSamples - pos);
            juce::AudioBuffer<float> block (2, kBlockSize);
            block.clear();
            for (int c = 0; c < 2; ++c)
                block.copyFrom (c, 0, source, c, pos, n);

            midi.clear();
            processor.processBlock (block, midi);

            for (int c = 0; c < 2; ++c)
                rendered.copyFrom (c, pos, block, c, 0, n);
            pos += n;
        }

        double maxAbsDiff = 0.0;
        for (int c = 0; c < 2; ++c)
        {
            const float* d = source.getReadPointer (c);
            const float* w = rendered.getReadPointer (c);
            for (int i = 0; i < totalSamples; ++i)
                maxAbsDiff = juce::jmax (maxAbsDiff, (double) std::abs (d[i] - w[i]));
        }

        printf ("  D2: sequencerOn=false, SidechainDuck Volume curve -> maxAbsDiff vs dry = %.6f\n", maxAbsDiff);
        if (maxAbsDiff < 0.05)
        {
            printf ("  FAIL: expected the Volume curve modulator to audibly duck the signal even with sequencerOn=false\n");
            pass = false;
        }
    }

    printf ("  %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// (e) internalBpm is APVTS-owned (2.2): changing it via the APVTS parameter must change the
// free-running playhead's advance speed. Renders a fixed number of samples at two different
// internalBpm values (hostSync off, transport free-running) and checks the sequencer's
// playhead-step count advances proportionally faster at the higher BPM.
bool testApvtsInternalBpmChangesFreeRunSpeed()
{
    printf ("\n[Test E] APVTS internalBpm change alters free-running playhead speed\n");

    auto renderAndCountStepAdvances = [] (double bpm) -> int
    {
        StutterAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
        processor.prepareToPlay (kSampleRate, kBlockSize);

        auto& apvts = processor.getAPVTS();
        apvts.getParameter (stutter::ID::hostSync)->setValueNotifyingHost (0.0f); // force free-run
        setInternalBpm (processor, bpm);

        // Reads the BLOCK sequencer's playhead: since the audio path switched to it, the v1
        // StepSequencer no longer advances and querying it would report a frozen transport
        // regardless of tempo. The scene needs no blocks -- only that the playhead moves.
        const int totalSamples = (int) std::round (2.0 * kSampleRate);
        juce::AudioBuffer<float> block (2, kBlockSize);
        juce::MidiBuffer midi;

        int lastStep = -1;
        int advances = 0;
        int pos = 0;
        while (pos < totalSamples)
        {
            const int n = juce::jmin (kBlockSize, totalSamples - pos);
            block.setSize (2, n, false, false, true);
            block.clear();

            midi.clear();
            processor.processBlock (block, midi);

            const int step = processor.getBlockPlayheadDivision();
            if (lastStep >= 0 && step != lastStep)
                ++advances;
            lastStep = step;

            pos += n;
        }
        return advances;
    };

    const int advancesAtBaseBpm = renderAndCountStepAdvances (kBpm);
    const int advancesAtDoubleBpm = renderAndCountStepAdvances (kBpm * 2.0);

    printf ("  step advances @ %.0f BPM = %d, @ %.0f BPM = %d\n",
            kBpm, advancesAtBaseBpm, kBpm * 2.0, advancesAtDoubleBpm);

    // At double the BPM the playhead should cross roughly twice as many step boundaries in the
    // same wall-clock render (allow generous tolerance for edge effects at start/end of render).
    const bool pass = advancesAtDoubleBpm > advancesAtBaseBpm * 3 / 2
                       && advancesAtBaseBpm > 0;
    printf ("  %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// (f) Rate choice labels must name the duration the DSP actually produces.
//
// Through v1.1.2 they did not: the rate table's fractions are relative to a quarter note,
// but the labels read as if they were bar fractions, so every displayed value was 4x longer
// than what was heard (selecting "1/4" produced a 16th-note loop). Nothing checked the
// labels against the arithmetic, so the mismatch shipped. This test closes that gap for both
// parameter surfaces -- the APVTS choice list a host sees, and the ParamDescriptor list the
// UI builds its combo box from.
bool testRateLabelsMatchActualDurations()
{
    printf ("\n[Test F] Rate choice labels name the durations actually produced\n");

    // Parse "1/16", "1/32T", "1/64." into how many of that note fit in one bar. Take the
    // denominator only -- retaining all digits would fold the leading "1/" into the number
    // (turning "1/16" into 116).
    auto notesPerBarFromLabel = [] (const juce::String& label) -> double
    {
        const double base = (double) label.fromFirstOccurrenceOf ("/", false, false)
                                          .retainCharacters ("0123456789")
                                          .getIntValue();
        if (label.endsWith ("T")) return base * 1.5;  // triplet: 2/3 the length -> 3/2 as many
        if (label.endsWith (".")) return base / 1.5;  // dotted: 3/2 the length -> 2/3 as many
        return base;
    };

    bool ok = true;

    // The DSP converts a table fraction to samples as (fraction / 0.25) * divisionLength,
    // where a division is a 16th under the fixed v1 grid. Work in bar-relative terms so the
    // check is independent of tempo and sample rate.
    constexpr double divisionsPerBar = 16.0;
    for (int i = 0; i < stutter::numLegacyRateIndices; ++i)
    {
        const double fraction = stutter::legacyRateIndexToFraction (i);
        const double divisions = fraction / 0.25;              // in divisions (16ths)
        const double notesPerBar = divisionsPerBar / divisions; // 1/N of a bar
        const juce::String label = stutter::legacyRateIndexLabels()[i];
        const double claimed = notesPerBarFromLabel (label);

        const bool match = std::abs (claimed - notesPerBar) < 0.01;
        if (! match)
        {
            printf ("  FAIL: index %d labelled \"%s\" (1/%.2f of bar) but produces 1/%.2f of bar\n",
                    i, label.toRawUTF8(), claimed, notesPerBar);
            ok = false;
        }
    }

    // Both parameter surfaces must expose that same corrected list; they are built
    // separately, so a fix applied to only one would leave the other lying.
    StutterAudioProcessor processor;
    processor.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
    processor.prepareToPlay (kSampleRate, kBlockSize);

    auto checkApvtsChoices = [&ok] (juce::AudioProcessorValueTreeState& apvts,
                                    int lane, const juce::String& paramName, const char* what)
    {
        auto* raw = apvts.getParameter (stutter::ID::lanePrefix (lane) + paramName);
        auto* choice = dynamic_cast<juce::AudioParameterChoice*> (raw);
        if (choice == nullptr)
        {
            printf ("  FAIL: %s is not a choice parameter\n", what);
            ok = false;
            return;
        }
        if (choice->choices.size() != stutter::numLegacyRateIndices)
        {
            printf ("  FAIL: %s has %d choices, expected %d\n",
                    what, choice->choices.size(), stutter::numLegacyRateIndices);
            ok = false;
            return;
        }
        for (int i = 0; i < stutter::numLegacyRateIndices; ++i)
            if (choice->choices[i] != juce::String (stutter::legacyRateIndexLabels()[i]))
            {
                printf ("  FAIL: %s choice %d is \"%s\", expected \"%s\"\n", what, i,
                        choice->choices[i].toRawUTF8(), stutter::legacyRateIndexLabels()[i]);
                ok = false;
            }
    };

    checkApvtsChoices (processor.getAPVTS(), StutterAudioProcessor::laneStutter,
                       stutter::ID::stutterRate, "APVTS Stutter Rate");
    checkApvtsChoices (processor.getAPVTS(), StutterAudioProcessor::laneReverse,
                       stutter::ID::reverseSliceLen, "APVTS Reverse Slice Length");

    stutter::StutterEffect stutterEffect;
    stutter::ReverseEffect reverseEffect;
    const auto stutterDescs = stutterEffect.getParamDescriptors();
    const auto reverseDescs = reverseEffect.getParamDescriptors();

    auto checkDescriptorChoices = [&ok] (const stutter::ParamDescriptor& d, const char* what)
    {
        if (d.choices == nullptr || d.numChoices != stutter::numLegacyRateIndices)
        {
            printf ("  FAIL: %s descriptor has %d choices, expected %d\n",
                    what, d.numChoices, stutter::numLegacyRateIndices);
            ok = false;
            return;
        }
        for (int i = 0; i < stutter::numLegacyRateIndices; ++i)
            if (juce::String (d.choices[i]) != juce::String (stutter::legacyRateIndexLabels()[i]))
            {
                printf ("  FAIL: %s descriptor choice %d is \"%s\", expected \"%s\"\n", what, i,
                        d.choices[i], stutter::legacyRateIndexLabels()[i]);
                ok = false;
            }
    };

    checkDescriptorChoices (stutterDescs[0], "Stutter rate");
    checkDescriptorChoices (reverseDescs[0], "Reverse sliceLen");

    if (ok)
        printf ("  all %d labels match their produced duration on both surfaces (e.g. index 0 = \"%s\")\n",
                stutter::numLegacyRateIndices, stutter::legacyRateIndexLabels()[0]);
    printf ("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// (g) v2 scene schema: parsing, clamping, the block ordering invariant, curve baking, and
// the SceneStore publish/retire lifecycle.
//
// The block ordering check is the load-bearing one: BlockSequencer will advance a
// forward-only cursor through each lane's blocks, so an unsorted or overlapping array
// silently breaks playback in a way that is hard to trace back here. sceneFromTree is the
// only place that invariant is established.
bool testSceneSchemaAndStore()
{
    printf ("\n[Test G] v2 scene schema, block ordering, and store lifecycle\n");

    using namespace stutter;
    bool ok = true;
    auto check = [&ok] (const char* what, bool cond)
    {
        if (! cond) { printf ("  FAIL: %s\n", what); ok = false; }
    };

    // ---- G1: malformed input yields a usable scene, never garbage --------------------
    {
        juce::ValueTree empty (SceneIDs::scene);
        const auto s = SceneSchema::sceneFromTree (empty);
        check ("empty scene gets default geometry",
               s.beats == 4 && s.divisions == 4 && s.populated);

        juce::ValueTree wild (SceneIDs::scene);
        wild.setProperty (SceneIDs::beats, 999, nullptr);
        wild.setProperty (SceneIDs::divisions, -5, nullptr);
        wild.setProperty (SceneIDs::swing, 17.0f, nullptr);
        wild.setProperty (SceneIDs::loopPolicy, 42, nullptr);
        wild.setProperty (SceneIDs::releaseMode, -3, nullptr);
        const auto w = SceneSchema::sceneFromTree (wild);
        check ("out-of-range beats clamps to 1..8", w.beats >= 1 && w.beats <= 8);
        check ("out-of-range divisions clamps to 2..8", w.divisions >= 2 && w.divisions <= 8);
        check ("out-of-range swing clamps to -1..1", w.swing >= -1.0f && w.swing <= 1.0f);
        check ("invalid loopPolicy falls back to a valid enum",
               (int) w.loopPolicy >= 0 && (int) w.loopPolicy <= 2);
        check ("invalid releaseMode falls back to a valid enum",
               (int) w.releaseMode >= 0 && (int) w.releaseMode <= 4);

        // A lane index far outside the array must not write past the end.
        juce::ValueTree badLaneScene (SceneIDs::scene);
        juce::ValueTree laneParams (SceneIDs::laneParams);
        juce::ValueTree badLane (SceneIDs::lane);
        badLane.setProperty (SceneIDs::index, 999, nullptr);
        laneParams.appendChild (badLane, nullptr);
        badLaneScene.appendChild (laneParams, nullptr);
        const auto bl = SceneSchema::sceneFromTree (badLaneScene);
        check ("out-of-range lane index is ignored, not written", bl.populated);

        // An out-of-range curve target must be rejected rather than indexing the mod array.
        juce::ValueTree curveScene (SceneIDs::scene);
        juce::ValueTree curvesNode (SceneIDs::curvesNode);
        juce::ValueTree badCurve (SceneIDs::curve);
        badCurve.setProperty (SceneIDs::target, 99999, nullptr);
        badCurve.setProperty (SceneIDs::enabled, true, nullptr);
        curvesNode.appendChild (badCurve, nullptr);
        curveScene.appendChild (curvesNode, nullptr);
        const auto cs = SceneSchema::sceneFromTree (curveScene);
        check ("invalid curve target is rejected",
               cs.curves[0].targetParam == -1 || isValidParamIndex (cs.curves[0].targetParam));
        check ("invalid-target curve is not counted as an active route",
               cs.numActiveCurves == 0);
    }

    // ---- G2: blocks come out sorted and non-overlapping -------------------------------
    {
        juce::ValueTree sceneTree (SceneIDs::scene);
        sceneTree.setProperty (SceneIDs::beats, 4, nullptr);
        sceneTree.setProperty (SceneIDs::divisions, 4, nullptr);
        juce::ValueTree blocks (SceneIDs::blocksNode);

        // Deliberately out of order, with an overlap and a negative length.
        auto addBlock = [&blocks] (int lane, int start, int len)
        {
            juce::ValueTree b (SceneIDs::block);
            b.setProperty (SceneIDs::laneRef, lane, nullptr);
            b.setProperty (SceneIDs::start, start, nullptr);
            b.setProperty (SceneIDs::length, len, nullptr);
            blocks.appendChild (b, nullptr);
        };
        addBlock (0, 12, 2);
        addBlock (0, 0, 4);
        addBlock (0, 2, 4);    // overlaps the previous block
        addBlock (0, 8, 2);
        addBlock (0, 5, -3);   // nonsense length
        sceneTree.appendChild (blocks, nullptr);

        const auto s = SceneSchema::sceneFromTree (sceneTree);
        const auto& lane0 = s.lanes[0];

        bool sorted = true, disjoint = true, sane = true;
        for (int i = 0; i < lane0.numBlocks; ++i)
        {
            const auto& blk = lane0.blocks[(size_t) i];
            if (blk.lengthDiv <= 0)
                sane = false;
            if (blk.startDiv < 0 || blk.endDiv() > s.totalDivisions())
                sane = false;
            if (i > 0)
            {
                const auto& prev = lane0.blocks[(size_t) i - 1];
                if (blk.startDiv < prev.startDiv)
                    sorted = false;
                if (blk.startDiv < prev.endDiv())
                    disjoint = false;
            }
        }
        check ("blocks are sorted by startDiv", sorted);
        check ("blocks do not overlap", disjoint);
        check ("blocks have positive length and stay in range", sane);
        check ("at least the valid blocks survived", lane0.numBlocks >= 2);
    }

    // ---- G3: curve baking, including the anti-click weights ---------------------------
    {
        auto maxAdjacentJump = [] (const float* t, int n)
        {
            float m = 0.0f;
            for (int i = 1; i < n; ++i)
                m = juce::jmax (m, std::abs (t[(size_t) i] - t[(size_t) i - 1]));
            return m;
        };

        // A hard step: two points at nearly the same position. This is the shape the point
        // weights exist to tame.
        auto stepPoints = [] (PointWeight w)
        {
            return std::vector<CurvePointV2> {
                { 0.0f,    0.0f, 0.0f, w },
                { 0.5f,    0.0f, 0.0f, w },
                { 0.5001f, 1.0f, 0.0f, w },
                { 1.0f,    1.0f, 0.0f, w }
            };
        };

        std::vector<float> table ((size_t) CurveSnapshot::tableSize);
        SceneSchema::bakeCurveTable (stepPoints (PointWeight::Hard), table.data(), CurveSnapshot::tableSize);
        const float hardJump = maxAdjacentJump (table.data(), CurveSnapshot::tableSize);
        SceneSchema::bakeCurveTable (stepPoints (PointWeight::Medium), table.data(), CurveSnapshot::tableSize);
        const float mediumJump = maxAdjacentJump (table.data(), CurveSnapshot::tableSize);
        SceneSchema::bakeCurveTable (stepPoints (PointWeight::Soft), table.data(), CurveSnapshot::tableSize);
        const float softJump = maxAdjacentJump (table.data(), CurveSnapshot::tableSize);

        printf ("  step max adjacent jump: Hard=%.4f Medium=%.4f Soft=%.4f\n",
                hardJump, mediumJump, softJump);

        // The weights are an anti-click mechanism; if Medium and Soft do not actually reduce
        // the discontinuity they are decorative. Soft must be the smoothest of the three.
        check ("Medium reduces the step discontinuity", mediumJump < hardJump * 0.5f);
        check ("Soft reduces the step discontinuity", softJump < hardJump * 0.5f);
        check ("Soft is at least as smooth as Medium", softJump <= mediumJump + 1.0e-4f);

        // Smoothing must not destroy an ordinary shape: a linear ramp should still span
        // its endpoints closely.
        const std::vector<CurvePointV2> ramp {
            { 0.0f, 0.0f, 0.0f, PointWeight::Soft },
            { 1.0f, 1.0f, 0.0f, PointWeight::Soft }
        };
        SceneSchema::bakeCurveTable (ramp, table.data(), CurveSnapshot::tableSize);
        check ("Soft leaves a plain ramp near its endpoints",
               table[0] < 0.05f && table[(size_t) CurveSnapshot::tableSize - 1] > 0.95f);

        // Degenerate input must not read out of bounds or produce NaN.
        SceneSchema::bakeCurveTable ({}, table.data(), CurveSnapshot::tableSize);
        bool finite = true;
        for (int i = 0; i < CurveSnapshot::tableSize; ++i)
            if (! std::isfinite (table[(size_t) i])) finite = false;
        check ("empty point list bakes to a finite table", finite);

        const std::vector<CurvePointV2> single { { 0.3f, 0.7f, 0.0f, PointWeight::Hard } };
        SceneSchema::bakeCurveTable (single, table.data(), CurveSnapshot::tableSize);
        check ("single point bakes to that flat value",
               std::abs (table[0] - 0.7f) < 1.0e-4f
               && std::abs (table[(size_t) CurveSnapshot::tableSize / 2] - 0.7f) < 1.0e-4f);
    }

    // ---- G4: tier resolution ----------------------------------------------------------
    {
        const auto locked = SceneSchema::pointsForTier (0, 0.42f, 0.0f, 1.0f, false);
        check ("Locked tier yields a flat pair",
               locked.size() == 2
               && std::abs (locked[0].value - 0.42f) < 1.0e-6f
               && std::abs (locked[1].value - 0.42f) < 1.0e-6f);

        const auto split = SceneSchema::pointsForTier (1, 0.0f, 0.2f, 0.9f, false);
        check ("Split tier ramps low to high",
               split.size() == 2 && split[0].value < split[1].value);

        const auto reversed = SceneSchema::pointsForTier (1, 0.0f, 0.2f, 0.9f, true);
        check ("Split tier reverses on request",
               reversed.size() == 2 && reversed[0].value > reversed[1].value);

        check ("Custom tier defers to stored points",
               SceneSchema::pointsForTier (2, 0.0f, 0.0f, 1.0f, false).empty());
    }

    // ---- G5: store publish / retire lifecycle -----------------------------------------
    {
        SceneStore store;
        check ("empty store reads as nullptr", store.get (0) == nullptr);

        auto bank = SceneStore::createBank();
        bank->scenes[5].populated = true;
        bank->scenes[5].beats = 7;
        store.publish (std::move (bank));

        const auto* s = store.get (5);
        check ("published scene is readable", s != nullptr && s->beats == 7);
        check ("index beyond the bank is nullptr", store.get (maxScenes) == nullptr);
        check ("negative index is nullptr", store.get (-1) == nullptr);
        check ("nothing retired after the first publish", store.getPendingRetireCount() == 0);

        auto bank2 = SceneStore::createBank();
        bank2->scenes[5].populated = true;
        bank2->scenes[5].beats = 3;
        store.publish (std::move (bank2));

        check ("republish is visible immediately", store.get (5)->beats == 3);
        check ("the replaced bank is retired, not freed", store.getPendingRetireCount() == 1);

        // Inside the grace window the bank must survive: freeing it early is exactly the
        // use-after-free the retire queue exists to prevent.
        store.collectGarbage();
        check ("collectGarbage keeps banks inside the grace window",
               store.getPendingRetireCount() == 1);
    }

    // ---- G6: the v1 state guard actually rejects v1 state -----------------------------
    //
    // v2 drops v1 compatibility deliberately, so the guard is the only thing standing
    // between an old session file and a half-applied, incoherent state. A guard that
    // silently lets v1 through would be worse than none, because the failure would surface
    // later as inexplicable parameter values.
    {
        StutterAudioProcessor proc;
        proc.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
        proc.prepareToPlay (kSampleRate, kBlockSize);

        auto& apvts = proc.getAPVTS();
        auto* dryWetParam = apvts.getParameter (ID::dryWet);
        const float defaultDryWet = apvts.getRawParameterValue (ID::dryWet)->load();

        // Build a v1-shaped state: the APVTS tree carrying a deliberately non-default value
        // and, crucially, no version property.
        dryWetParam->setValueNotifyingHost (0.123f);
        auto v1State = apvts.copyState();
        v1State.removeProperty (SceneIDs::version, nullptr);
        const float smuggledValue = apvts.getRawParameterValue (ID::dryWet)->load();

        std::unique_ptr<juce::XmlElement> xml (v1State.createXml());
        juce::MemoryBlock block;
        proc.copyXmlToBinary (*xml, block);

        // Move the live value away from the smuggled one so "rejected" is distinguishable
        // from "coincidentally already equal".
        dryWetParam->setValueNotifyingHost (1.0f);

        proc.setStateInformation (block.getData(), (int) block.getSize());
        const float afterLoad = apvts.getRawParameterValue (ID::dryWet)->load();

        check ("v1-shaped state (no version property) is not applied",
               std::abs (afterLoad - smuggledValue) > 1.0e-4f);
        juce::ignoreUnused (defaultDryWet);
    }

    printf ("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// (h) The block model's own behaviour: held envelopes and swing.
//
// This began as a v1-equivalence test that rendered every lane through both StepSequencer and
// BlockSequencer and required bit-identical output. That comparison earned its keep -- it
// caught adjacent blocks re-latching ContinueThroughRun envelopes, and swing changing the
// tempo instead of the groove -- and it proved the block model reproduced the old grid before
// the audio path was switched to it.
//
// It is retired along with StepSequencer itself. The equivalence it established is now held
// by the golden baseline, where seven of the eight lanes still carry their exact v1
// checksums. What remains below are the checks that never had a v1 counterpart.
bool testBlockSequencerBehaviour()
{
    printf ("\n[Test H] block model: held envelopes and swing\n");

    using namespace stutter;
    bool ok = true;

    // --- The v2-only features, which by definition have no v1 reference to compare to ---
    //
    // Equivalence above proves the block model reproduces the old grid. These check that the
    // things the old grid could not express actually behave, since a silent no-op here would
    // otherwise look exactly like success.
    {
        // A held block must not re-latch a ContinueThroughRun envelope. That is the whole
        // reason variable-length blocks exist: v1 restarted TapeStop every 16th, so it never
        // reached a stop. Held across 8 divisions it must actually get there.
        CaptureBuffer cap; cap.prepare (kSampleRate, 2, 2.5);
        BlockSequencer seq;
        seq.setLaneEffect (StutterAudioProcessor::laneTapeStop, std::make_unique<TapeStopEffect>());
        seq.prepare (kSampleRate, 2);
        seq.setEnabled (true);

        SceneSnapshot scene {};
        scene.beats = 4; scene.divisions = 4;
        auto& lane = scene.lanes[(size_t) StutterAudioProcessor::laneTapeStop];
        lane.blocks[0].startDiv = 0;
        lane.blocks[0].lengthDiv = 8;   // one block spanning half the pattern
        lane.numBlocks = 1;
        if (auto* eff = seq.getLaneEffect (StutterAudioProcessor::laneTapeStop))
        {
            const auto set = eff->getParamDescriptors();
            for (int i = 0; i < set.count && i < maxParamsPerLane; ++i)
                lane.params[(size_t) i] = set[i].defaultValue;
        }
        seq.updateChainOrder (scene);

        const int holdTotal = (int) (kSampleRate * 2.0);
        juce::AudioBuffer<float> holdSource (2, holdTotal);
        fillTestSignal (holdSource, kSampleRate, kBpm);

        const double holdPpqPerSample = (kBpm / 60.0) / kSampleRate;
        double clock = 0.0;
        double lateRms = 0.0;
        int lateCount = 0;
        for (int pos = 0; pos < holdTotal; pos += kBlockSize)
        {
            const int n = juce::jmin (kBlockSize, holdTotal - pos);
            juce::AudioBuffer<float> blk (2, n);
            for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, holdSource, c, pos, n);
            cap.write (blk);
            seq.processBlock (blk, cap, scene, clock, holdPpqPerSample);
            clock += holdPpqPerSample * (double) n;

            // Sample the tail of the block's span, where a tape stop should have wound down.
            const double divLen = 0.25 / holdPpqPerSample;
            if ((double) pos > divLen * 7.0 && (double) pos < divLen * 8.0)
            {
                lateRms += rmsOf (blk);
                ++lateCount;
            }
        }

        const double avgLate = lateCount > 0 ? lateRms / (double) lateCount : 1.0;
        const bool stopped = avgLate < 0.2;   // dry RMS is ~0.5
        if (! stopped)
        {
            printf ("  FAIL: an 8-division TapeStop block did not wind down (tail RMS %.4f)\n", avgLate);
            ok = false;
        }
        else
        {
            printf ("  held 8-division TapeStop reaches a stop (tail RMS %.4f)\n", avgLate);
        }
    }

    {
        // Swing must move odd division boundaries and leave even ones alone. Checked through
        // the playhead rather than the audio, so it isolates the timing change itself.
        BlockSequencer seq;
        seq.setLaneEffect (StutterAudioProcessor::laneGate, std::make_unique<GateEffect>());
        seq.prepare (kSampleRate, 2);
        seq.setEnabled (true);

        auto firstDivisionChangeSample = [&] (float swing) -> int
        {
            CaptureBuffer cap; cap.prepare (kSampleRate, 2, 2.5);
            seq.reset();

            SceneSnapshot scene {};
            scene.beats = 4; scene.divisions = 4; scene.swing = swing;
            seq.updateChainOrder (scene);

            const double swingPpqPerSample = (kBpm / 60.0) / kSampleRate;
            const int swingTotal = (int) (kSampleRate * 0.6);
            juce::AudioBuffer<float> blk (2, 1);
            double clock = 0.0;
            int lastDiv = -1;
            for (int i = 0; i < swingTotal; ++i)
            {
                blk.clear();
                cap.write (blk);
                seq.processBlock (blk, cap, scene, clock, swingPpqPerSample);
                clock += swingPpqPerSample;
                const int d = seq.getPlayheadDivision();
                if (lastDiv == 0 && d == 1)
                    return i;
                lastDiv = d;
            }
            return -1;
        };

        const int straight = firstDivisionChangeSample (0.0f);
        const int swung    = firstDivisionChangeSample (0.6f);

        // Two properties, and the second is the one that is easy to lose: the odd boundary
        // must move, AND the even boundaries must not. An implementation that rescales each
        // division independently passes the first check while shortening the pattern -- a
        // tempo change rather than a groove -- so both are asserted.
        //
        // At swing 0.6 the 0->1 boundary should land 30% of a division late.
        const double divisionSamples = 0.25 / ((kBpm / 60.0) / kSampleRate);
        const double expectedShift = 0.3 * divisionSamples;
        const double actualShift = (double) (swung - straight);

        const bool moved = straight > 0 && swung > 0
                        && std::abs (actualShift - expectedShift) < divisionSamples * 0.05;
        if (! moved)
            printf ("  FAIL: swing shift was %.0f samples, expected ~%.0f (straight=%d swung=%d)\n",
                    actualShift, expectedShift, straight, swung);
        else
            printf ("  swing delays the 0->1 boundary by %.0f samples (expected ~%.0f)\n",
                    actualShift, expectedShift);
        ok = ok && moved;

        // The pattern's total length must be unchanged: check the even boundary at division 2.
        auto secondBoundarySample = [&] (float swing) -> int
        {
            CaptureBuffer cap; cap.prepare (kSampleRate, 2, 2.5);
            seq.reset();
            SceneSnapshot scene {};
            scene.beats = 4; scene.divisions = 4; scene.swing = swing;
            seq.updateChainOrder (scene);

            const double pps = (kBpm / 60.0) / kSampleRate;
            juce::AudioBuffer<float> blk (2, 1);
            double clock = 0.0;
            int lastDiv = -1;
            for (int i = 0; i < (int) (kSampleRate * 0.9); ++i)
            {
                blk.clear();
                cap.write (blk);
                seq.processBlock (blk, cap, scene, clock, pps);
                clock += pps;
                const int d = seq.getPlayheadDivision();
                if (lastDiv == 1 && d == 2)
                    return i;
                lastDiv = d;
            }
            return -1;
        };

        const int evenStraight = secondBoundarySample (0.0f);
        const int evenSwung    = secondBoundarySample (0.6f);
        const bool pinned = evenStraight > 0 && evenSwung > 0
                         && std::abs (evenSwung - evenStraight) <= 2;   // within rounding
        if (! pinned)
            printf ("  FAIL: swing moved the EVEN boundary (%d -> %d); pattern length changed\n",
                    evenStraight, evenSwung);
        else
            printf ("  swing leaves the 1->2 boundary pinned (%d vs %d samples)\n",
                    evenStraight, evenSwung);
        ok = ok && pinned;
    }

    printf ("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// (i) MIDI gesture layer: note->scene selection, the five release modes, early-trigger
// quantization, Scene Lock, and -- most importantly -- that the gate is click-free.
//
// The gate is a ramp on the wet path rather than a mute on the sequencer, specifically so
// that every transition is smooth by construction. That property is worth asserting: a
// regression to a hard mute would be inaudible in a spectrum plot and obvious on a note-off.
bool testGestureEngine()
{
    printf ("\n[Test I] MIDI gesture layer\n");

    using namespace stutter;
    bool ok = true;
    auto check = [&ok] (const char* what, bool cond)
    {
        if (! cond) { printf ("  FAIL: %s\n", what); ok = false; }
    };

    const double ppqPerSample = (kBpm / 60.0) / kSampleRate;

    auto noteOn = [] (int note, int offset)
    {
        juce::MidiBuffer b;
        b.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), offset);
        return b;
    };
    auto noteOff = [] (int note, int offset)
    {
        juce::MidiBuffer b;
        b.addEvent (juce::MidiMessage::noteOff (1, note), offset);
        return b;
    };

    // ---- I1: note selects a scene, and only in MIDI/unlocked conditions ----------------
    {
        GestureEngine g;
        g.prepare (kSampleRate);
        g.setIdentityMapping();
        g.setPlayMode (PlayMode::Midi);

        g.processMidi (noteOn (60, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
        check ("note 60 selects scene 60", g.getActiveScene() == 60);
        check ("a scene change is flagged for the APVTS mirror", g.consumePendingMirror() == 60);
        check ("the mirror flag is consumed once", g.consumePendingMirror() == -1);

        g.setSceneLock (true);
        g.processMidi (noteOn (72, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
        check ("Scene Lock refuses the selection", g.getActiveScene() == 60);

        g.setSceneLock (false);
        g.processMidi (noteOn (72, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
        check ("unlocking restores selection", g.getActiveScene() == 72);
    }

    // ---- I2: the gate ramps, never steps ----------------------------------------------
    {
        GestureEngine g;
        g.prepare (kSampleRate);
        g.setIdentityMapping();
        g.setPlayMode (PlayMode::Midi);

        float maxStep = 0.0f;
        float prev = g.getGateGain();

        auto runSamples = [&] (int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const float gain = g.nextGateGain();
                maxStep = juce::jmax (maxStep, std::abs (gain - prev));
                prev = gain;
            }
        };

        g.processMidi (noteOn (60, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
        runSamples (kBlockSize);
        check ("gate opens on note-on", g.getGateGain() > 0.99f);

        g.processMidi (noteOff (60, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
        runSamples (kBlockSize);
        check ("gate closes on note-off", g.getGateGain() < 0.01f);

        // A 5ms ramp at 48k is 240 samples, so no single step may exceed ~1/240.
        const float allowed = 1.0f / 200.0f;
        printf ("  max per-sample gate step = %.6f (limit %.6f)\n", maxStep, allowed);
        check ("gate never steps discontinuously", maxStep <= allowed);
    }

    // ---- I3: Auto mode ignores the gate ------------------------------------------------
    {
        GestureEngine g;
        g.prepare (kSampleRate);
        g.setIdentityMapping();
        g.setPlayMode (PlayMode::Auto);

        juce::MidiBuffer empty;
        g.processMidi (empty, kBlockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
        for (int i = 0; i < kBlockSize; ++i) g.nextGateGain();
        check ("Auto mode holds the gate open with no notes", g.getGateGain() > 0.99f);
    }

    // ---- I4: release modes ------------------------------------------------------------
    {
        // Latch: note-off must NOT close the gate.
        GestureEngine g;
        g.prepare (kSampleRate);
        g.setIdentityMapping();
        g.setPlayMode (PlayMode::Midi);
        g.processMidi (noteOn (60, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Latch);
        for (int i = 0; i < kBlockSize; ++i) g.nextGateGain();
        g.processMidi (noteOff (60, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Latch);
        for (int i = 0; i < kBlockSize; ++i) g.nextGateGain();
        check ("Latch keeps the gate open through note-off", g.getGateGain() > 0.99f);

        // OnGrid: the gate must stay open until the next grid boundary, then close.
        GestureEngine g2;
        g2.prepare (kSampleRate);
        g2.setIdentityMapping();
        g2.setPlayMode (PlayMode::Midi);
        g2.setTriggerQuantize (1.0);   // one quarter note

        double clock = 0.0;
        g2.processMidi (noteOn (60, 0), kBlockSize, clock, ppqPerSample, ReleaseMode::OnGrid);
        for (int i = 0; i < kBlockSize; ++i) g2.nextGateGain();
        clock += ppqPerSample * kBlockSize;

        // Release just after the boundary at ppq 0, so the next one is at ppq 1.0.
        g2.processMidi (noteOff (60, 0), kBlockSize, clock, ppqPerSample, ReleaseMode::OnGrid);
        for (int i = 0; i < kBlockSize; ++i) g2.nextGateGain();
        clock += ppqPerSample * kBlockSize;
        check ("OnGrid holds the gate until the boundary", g2.getGateGain() > 0.99f);

        // Advance past ppq 1.0.
        juce::MidiBuffer empty;
        while (clock < 1.2)
        {
            g2.processMidi (empty, kBlockSize, clock, ppqPerSample, ReleaseMode::OnGrid);
            for (int i = 0; i < kBlockSize; ++i) g2.nextGateGain();
            clock += ppqPerSample * kBlockSize;
        }
        check ("OnGrid closes the gate after the boundary", g2.getGateGain() < 0.01f);
    }

    // ---- I5: quantization accepts an early note ----------------------------------------
    {
        GestureEngine g;
        g.prepare (kSampleRate);
        g.setIdentityMapping();
        g.setPlayMode (PlayMode::Midi);
        g.setTriggerQuantize (1.0);

        // A note arriving at ppq 0.9 is aiming at the boundary at 1.0, not at 0.0. It must
        // wait rather than fire late, which is what lets a player anticipate the beat.
        g.processMidi (noteOn (64, 0), kBlockSize, 0.9, ppqPerSample, ReleaseMode::Instant);
        check ("an early note does not fire immediately", g.getActiveScene() != 64);

        double clock = 0.9;
        juce::MidiBuffer empty;
        for (int b = 0; b < 40 && g.getActiveScene() != 64; ++b)
        {
            g.processMidi (empty, kBlockSize, clock, ppqPerSample, ReleaseMode::Instant);
            clock += ppqPerSample * kBlockSize;
        }
        check ("an early note fires on the boundary it was aiming at", g.getActiveScene() == 64);

        // A note well past the boundary is late and should fire at once rather than waiting
        // an entire grid unit.
        GestureEngine g2;
        g2.prepare (kSampleRate);
        g2.setIdentityMapping();
        g2.setPlayMode (PlayMode::Midi);
        g2.setTriggerQuantize (1.0);
        g2.processMidi (noteOn (67, 0), kBlockSize, 2.1, ppqPerSample, ReleaseMode::Instant);
        check ("a late note fires immediately", g2.getActiveScene() == 67);
    }

    // ---- I6: polyphonic release ---------------------------------------------------------
    {
        GestureEngine g;
        g.prepare (kSampleRate);
        g.setIdentityMapping();
        g.setPlayMode (PlayMode::Midi);

        g.processMidi (noteOn (60, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
        g.processMidi (noteOn (64, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
        for (int i = 0; i < kBlockSize; ++i) g.nextGateGain();

        g.processMidi (noteOff (60, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
        for (int i = 0; i < kBlockSize; ++i) g.nextGateGain();
        check ("releasing one of two held notes keeps the gate open", g.getGateGain() > 0.99f);

        g.processMidi (noteOff (64, 0), kBlockSize, 0.0, ppqPerSample, ReleaseMode::Instant);
        for (int i = 0; i < kBlockSize; ++i) g.nextGateGain();
        check ("releasing the last note closes the gate", g.getGateGain() < 0.01f);
    }

    // ---- I7: MIDI actually reaches the processor -------------------------------------
    //
    // Every check above exercises GestureEngine in isolation. This one drives the whole
    // plugin, because the wiring between processBlock and the engine is its own failure
    // surface: acceptsMidi(), the per-chunk MIDI slice, and the gate's position in the mix
    // are all things that can be individually correct and collectively broken.
    {
        StutterAudioProcessor proc;
        proc.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
        proc.prepareToPlay (kSampleRate, kBlockSize);

        check ("processor advertises MIDI input", proc.acceptsMidi());

        auto& engine = proc.getGestureEngine();
        engine.setPlayMode (PlayMode::Midi);

        juce::AudioBuffer<float> block (2, kBlockSize);
        juce::MidiBuffer midi;

        auto renderBlock = [&] (const juce::MidiBuffer& m) -> double
        {
            block.clear();
            for (int c = 0; c < 2; ++c)
            {
                auto* d = block.getWritePointer (c);
                for (int i = 0; i < kBlockSize; ++i)
                    d[i] = 0.5f;   // DC, so the gate's effect is unambiguous
            }
            juce::MidiBuffer copy (m);
            proc.processBlock (block, copy);
            return rmsOf (block);
        };

        // With no note held in MIDI mode the wet path is gated shut, so the output is the
        // dry signal -- not silence. Gating to dry rather than to nothing is deliberate:
        // releasing a note should not punch a hole in the track.
        const double idleRms = renderBlock (juce::MidiBuffer {});

        midi.clear();
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        renderBlock (midi);                    // gate ramps up during this block
        const double heldRms = renderBlock (juce::MidiBuffer {});

        check ("idle output is present (gated to dry, not silenced)", idleRms > 0.1);
        check ("a held note keeps the output live", heldRms > 0.1);
        check ("the note selected its scene through the processor",
               engine.getActiveScene() == 60);

        // MIDI must be consumed, not passed through: this is an audio effect, not a MIDI FX.
        juce::MidiBuffer passthrough;
        passthrough.addEvent (juce::MidiMessage::noteOn (1, 64, 1.0f), 0);
        block.clear();
        proc.processBlock (block, passthrough);
        check ("MIDI is consumed rather than passed through", passthrough.isEmpty());
    }

    printf ("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// (j) Modulation matrix: routing, precedence, speed multiplier, and the CPU budget.
//
// The budget check is not optional decoration. 16 curves x 12 lanes x 12 params evaluated
// per sample is millions of lookups a second; a plugin that glitches at 128-sample buffers
// is a failed plugin regardless of how expressive its modulation is. The mitigations (route
// list + control-rate evaluation) are only worth having if they are measured.
bool testModulationEngine()
{
    printf ("\n[Test J] modulation matrix\n");

    using namespace stutter;
    bool ok = true;
    auto check = [&ok] (const char* what, bool cond)
    {
        if (! cond) { printf ("  FAIL: %s\n", what); ok = false; }
    };

    // Build a scene with one curve routed to lane 6 (Filter) param 1 (cutoff).
    auto makeScene = [] (float depth, float speed, bool bipolar, float baseValue)
    {
        SceneSnapshot s {};
        s.beats = 4; s.divisions = 4;
        s.lanes[6].params[1] = baseValue;

        // A saw ramp 0 -> 1 across the cycle, so the phase->value mapping is unambiguous.
        std::vector<CurvePointV2> pts {
            { 0.0f, 0.0f, 0.0f, PointWeight::Hard },
            { 1.0f, 1.0f, 0.0f, PointWeight::Hard }
        };
        SceneSchema::bakeCurveTable (pts, s.curves[0].table.data(), CurveSnapshot::tableSize);
        s.curves[0].targetParam = paramIndex (6, 1);
        s.curves[0].depth = depth;
        s.curves[0].speedMultiplier = speed;
        s.curves[0].bipolar = bipolar;
        s.curves[0].enabled = true;
        s.activeCurves[0] = 0;
        s.numActiveCurves = 1;
        return s;
    };

    // ---- J1: an unrouted scene leaves parameters at their base ------------------------
    {
        SceneSnapshot plain {};
        plain.lanes[6].params[1] = 0.42f;
        ModulationEngine eng;
        eng.prepare (kSampleRate);
        const float* v = eng.nextSample (plain, 0.0);
        check ("unrouted parameter reads its scene value",
               std::abs (v[paramIndex (6, 1)] - 0.42f) < 1.0e-5f);
        check ("hasActiveRoutes reports nothing to do", ! ModulationEngine::hasActiveRoutes (plain));
    }

    // ---- J2: a routed curve sweeps the parameter --------------------------------------
    {
        const auto scene = makeScene (1.0f, 1.0f, false, 0.0f);
        ModulationEngine eng;
        eng.prepare (kSampleRate);

        // Sample the curve at phase 0 and phase ~0.75. With depth 1 and a 0->1 ramp the
        // value should track the phase.
        const float atStart = eng.nextSample (scene, 0.0)[paramIndex (6, 1)];

        // Advance enough samples for the control-rate interpolation to land.
        for (int i = 0; i < 64; ++i) eng.nextSample (scene, 0.75);
        const float atThreeQuarters = eng.nextSample (scene, 0.75)[paramIndex (6, 1)];

        printf ("  routed cutoff: phase 0.00 -> %.4f, phase 0.75 -> %.4f\n",
                atStart, atThreeQuarters);
        check ("curve drives its target", atThreeQuarters > atStart + 0.5f);
        check ("value stays in range", atThreeQuarters <= 1.0f && atStart >= 0.0f);
    }

    // ---- J3: precedence -- automation moves the base, the curve offsets from it -------
    {
        ModulationEngine engLow, engHigh;
        engLow.prepare (kSampleRate);
        engHigh.prepare (kSampleRate);

        // Same curve, different base values. A depth of 0.5 on a unipolar curve interpolates
        // halfway from the base toward the curve, so a higher base must yield a higher result
        // at the same phase -- i.e. the base is genuinely the starting point.
        const auto low  = makeScene (0.5f, 1.0f, false, 0.2f);
        const auto high = makeScene (0.5f, 1.0f, false, 0.8f);

        for (int i = 0; i < 64; ++i) { engLow.nextSample (low, 0.0); engHigh.nextSample (high, 0.0); }
        const float lowV  = engLow.nextSample (low, 0.0)[paramIndex (6, 1)];
        const float highV = engHigh.nextSample (high, 0.0)[paramIndex (6, 1)];

        printf ("  base 0.2 -> %.4f, base 0.8 -> %.4f (curve identical)\n", lowV, highV);
        check ("a higher base yields a higher modulated value", highV > lowV);
    }

    // ---- J4: speed multiplier -----------------------------------------------------------
    {
        // Probed at pattern phase 0.25, NOT 0.5. At speed 2 a pattern phase of 0.5 maps to a
        // curve phase of exactly 1.0, which wraps to 0 -- the saw's discontinuity lands
        // precisely on the probe, so the reading is 0.0 and says nothing about whether the
        // multiplier works. 0.25 maps to 0.5 under speed 2, which is unambiguous.
        const auto s1 = makeScene (1.0f, 1.0f, false, 0.0f);
        const auto s2 = makeScene (1.0f, 2.0f, false, 0.0f);

        ModulationEngine e1, e2;
        e1.prepare (kSampleRate); e2.prepare (kSampleRate);
        for (int i = 0; i < 64; ++i) { e1.nextSample (s1, 0.25); e2.nextSample (s2, 0.25); }
        const float v1 = e1.nextSample (s1, 0.25)[paramIndex (6, 1)];
        const float v2 = e2.nextSample (s2, 0.25)[paramIndex (6, 1)];

        printf ("  at pattern phase 0.25: speed 1x -> %.4f, speed 2x -> %.4f (want ~0.25 / ~0.50)\n",
                v1, v2);
        check ("speed 1x tracks the pattern phase", std::abs (v1 - 0.25f) < 0.02f);
        check ("speed 2x advances the curve twice as fast", std::abs (v2 - 0.50f) < 0.02f);
    }

    // ---- J5: modulation is smooth, not stepped -----------------------------------------
    {
        // Control-rate evaluation only works if the interpolation between evaluations is
        // real. Without it a swept cutoff would move in 16-sample stairs and zipper.
        const auto scene = makeScene (1.0f, 1.0f, false, 0.0f);
        ModulationEngine eng;
        eng.prepare (kSampleRate);

        float prev = eng.nextSample (scene, 0.0)[paramIndex (6, 1)];
        float maxStep = 0.0f;
        const int steps = 2000;
        for (int i = 1; i < steps; ++i)
        {
            const double phase = (double) i / (double) steps;
            const float v = eng.nextSample (scene, phase)[paramIndex (6, 1)];
            maxStep = juce::jmax (maxStep, std::abs (v - prev));
            prev = v;
        }
        // A 0->1 sweep over 2000 samples averages 0.0005 per sample; allow generous headroom
        // but reject anything resembling a 16-sample staircase (which would show ~0.008).
        printf ("  max per-sample modulation step over a full sweep = %.6f\n", maxStep);
        check ("modulation interpolates between control-rate updates", maxStep < 0.005f);
    }

    // ---- J6: CPU budget ------------------------------------------------------------------
    {
        // A fully-loaded scene: every curve routed, every lane populated. This is the worst
        // case the design has to survive.
        SceneSnapshot heavy {};
        heavy.beats = 4; heavy.divisions = 4;
        std::vector<CurvePointV2> pts {
            { 0.0f, 0.0f, 0.0f, PointWeight::Hard },
            { 0.5f, 1.0f, 0.3f, PointWeight::Hard },
            { 1.0f, 0.0f, -0.3f, PointWeight::Hard }
        };
        for (int i = 0; i < maxCurves; ++i)
        {
            SceneSchema::bakeCurveTable (pts, heavy.curves[(size_t) i].table.data(),
                                         CurveSnapshot::tableSize);
            heavy.curves[(size_t) i].targetParam = (juce::int16) paramIndex (i % maxLanes, i % maxParamsPerLane);
            heavy.curves[(size_t) i].depth = 1.0f;
            heavy.curves[(size_t) i].speedMultiplier = 1.0f;
            heavy.curves[(size_t) i].enabled = true;
            heavy.activeCurves[(size_t) i] = (juce::int16) i;
        }
        heavy.numActiveCurves = maxCurves;

        ModulationEngine eng;
        eng.prepare (kSampleRate);

        // Render 10 seconds of modulation and compare against wall clock.
        const int totalSamples = (int) (kSampleRate * 10.0);
        const double startTime = juce::Time::getMillisecondCounterHiRes();
        double sink = 0.0;
        for (int i = 0; i < totalSamples; ++i)
        {
            const double phase = (double) (i % 96000) / 96000.0;
            const float* v = eng.nextSample (heavy, phase);
            sink += v[0];   // keep the loop from being optimised away
        }
        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startTime;
        const double realtimeMs = 10.0 * 1000.0;
        const double cpuPercent = 100.0 * elapsedMs / realtimeMs;

        printf ("  %d curves x %d slots: %.1f%% of realtime (budget 30%%)\n",
                maxCurves, totalParamSlots, cpuPercent);
        check ("fully-loaded modulation stays inside the CPU budget", cpuPercent < 30.0);
        juce::ignoreUnused (sink);
    }

    // ---- J7: modulation actually reaches the audio -------------------------------------
    //
    // Everything above tests the engine in isolation. This drives BlockSequencer with and
    // without a modulation engine attached and requires the output to differ -- otherwise
    // the matrix could be perfectly correct and simply not plugged in, which is exactly the
    // failure a unit test of the engine alone cannot see.
    {
        auto render = [&] (bool withModulation)
        {
            CaptureBuffer cap; cap.prepare (kSampleRate, 2, 2.5);
            BlockSequencer seq;
            seq.setLaneEffect (StutterAudioProcessor::laneFilter, std::make_unique<FilterEffect>());
            seq.prepare (kSampleRate, 2);
            seq.setEnabled (true);

            SceneSnapshot scene {};
            scene.beats = 4; scene.divisions = 4;
            auto& lane = scene.lanes[(size_t) StutterAudioProcessor::laneFilter];
            lane.blocks[0].startDiv = 0;
            lane.blocks[0].lengthDiv = 16;
            lane.numBlocks = 1;

            // Filter defaults, then a curve sweeping cutoff (param 1), which the descriptor
            // table marks continuous precisely so it can be swept.
            if (auto* eff = seq.getLaneEffect (StutterAudioProcessor::laneFilter))
            {
                const auto set = eff->getParamDescriptors();
                for (int i = 0; i < set.count && i < maxParamsPerLane; ++i)
                    lane.params[(size_t) i] = set[i].defaultValue;
            }

            std::vector<CurvePointV2> pts {
                { 0.0f, 0.05f, 0.0f, PointWeight::Hard },
                { 1.0f, 1.0f,  0.0f, PointWeight::Hard }
            };
            SceneSchema::bakeCurveTable (pts, scene.curves[0].table.data(), CurveSnapshot::tableSize);
            scene.curves[0].targetParam = (juce::int16) paramIndex (StutterAudioProcessor::laneFilter, 1);
            scene.curves[0].depth = 1.0f;
            scene.curves[0].speedMultiplier = 1.0f;
            scene.curves[0].enabled = true;
            scene.activeCurves[0] = 0;
            scene.numActiveCurves = 1;
            seq.updateChainOrder (scene);

            ModulationEngine mod;
            mod.prepare (kSampleRate);

            const int total = (int) (kSampleRate * 1.0);
            juce::AudioBuffer<float> source (2, total);
            fillTestSignal (source, kSampleRate, kBpm);

            const double pps = (kBpm / 60.0) / kSampleRate;
            double clock = 0.0;
            juce::AudioBuffer<float> out (2, total);
            out.clear();
            for (int pos = 0; pos < total; pos += kBlockSize)
            {
                const int n = juce::jmin (kBlockSize, total - pos);
                juce::AudioBuffer<float> blk (2, n);
                for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, source, c, pos, n);
                cap.write (blk);
                seq.processBlock (blk, cap, scene, clock, pps, withModulation ? &mod : nullptr);
                clock += pps * (double) n;
                for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
            }
            return out;
        };

        const auto plain = render (false);
        const auto swept = render (true);

        double maxDiff = 0.0;
        for (int c = 0; c < 2; ++c)
        {
            const float* a = plain.getReadPointer (c);
            const float* b = swept.getReadPointer (c);
            for (int i = 0; i < plain.getNumSamples(); ++i)
                maxDiff = juce::jmax (maxDiff, std::abs ((double) a[i] - (double) b[i]));
        }

        printf ("  modulated vs unmodulated filter sweep: maxDiff = %.6f\n", maxDiff);
        check ("a routed curve audibly changes the output", maxDiff > 0.01);

        // And the sweep must be smooth: a stepped cutoff would zipper.
        const auto sweptMetrics = analyze (swept, kClickThreshold, kDiscontinuityThreshold);
        printf ("  swept output severe clicks = %d\n", sweptMetrics.severeClickCount);
        check ("the modulated sweep introduces no clicks", sweptMetrics.severeClickCount == 0);
    }

    printf ("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// (k) The editable scene document: block editing, undo, and the invariants the sequencer
// depends on.
//
// The UI mutates state through SceneDocument, so this is where block editing is verified.
// Driving the actual mouse handlers would need a live component and a display; testing the
// document directly covers the logic that can actually be wrong, and does so headlessly.
bool testSceneDocument()
{
    printf ("\n[Test K] scene document editing and undo\n");

    using namespace stutter;
    bool ok = true;
    auto check = [&ok] (const char* what, bool cond)
    {
        if (! cond) { printf ("  FAIL: %s\n", what); ok = false; }
    };

    SceneStore store;
    juce::UndoManager undo;
    SceneDocument doc (store, undo);

    // ---- K1: create, query, remove ------------------------------------------------------
    {
        check ("a fresh document has no blocks", ! doc.hasBlockAt (0, 0, 0));

        doc.addBlock (0, 0, 4, 3);
        check ("a created block covers its whole span",
               doc.hasBlockAt (0, 0, 4) && doc.hasBlockAt (0, 0, 6));
        check ("and nothing outside it",
               ! doc.hasBlockAt (0, 0, 3) && ! doc.hasBlockAt (0, 0, 7));
        check ("other lanes are unaffected", ! doc.hasBlockAt (0, 1, 4));

        doc.removeBlockAt (0, 0, 5);   // removing from the middle removes the whole block
        check ("removing from inside a block removes all of it", ! doc.hasBlockAt (0, 0, 4));
    }

    // ---- K2: overlap resolution ---------------------------------------------------------
    {
        // Dragging a new block over an existing one must replace it. Leaving both would
        // produce an overlap that SceneSchema then silently discards, so the user would see
        // their edit vanish on the next publish.
        doc.addBlock (0, 2, 0, 4);
        doc.addBlock (0, 2, 2, 4);     // overlaps the first

        doc.publish();
        const auto* scene = store.get (0);
        check ("store has the scene", scene != nullptr);

        if (scene != nullptr)
        {
            const auto& lane = scene->lanes[2];
            bool disjoint = true, sorted = true;
            for (int i = 1; i < lane.numBlocks; ++i)
            {
                if (lane.blocks[(size_t) i].startDiv < lane.blocks[(size_t) i - 1].startDiv)
                    sorted = false;
                if (lane.blocks[(size_t) i].startDiv < lane.blocks[(size_t) i - 1].endDiv())
                    disjoint = false;
            }
            check ("overlapping edits resolve to disjoint blocks", disjoint);
            check ("published blocks are sorted", sorted);
            check ("the later edit survived", lane.numBlocks >= 1);
        }
    }

    // ---- K3: undo groups a gesture into one step ----------------------------------------
    {
        doc.clearLane (0, 3);
        undo.beginNewTransaction();

        // Simulate a drag: several mutations inside one transaction.
        doc.addBlock (0, 3, 0, 1);
        doc.removeBlockAt (0, 3, 0);
        doc.addBlock (0, 3, 0, 2);
        doc.removeBlockAt (0, 3, 0);
        doc.addBlock (0, 3, 0, 3);

        check ("the drag left a block", doc.hasBlockAt (0, 3, 2));

        undo.undo();
        check ("one undo reverses the whole drag", ! doc.hasBlockAt (0, 3, 0));

        undo.redo();
        check ("redo restores it", doc.hasBlockAt (0, 3, 2));
    }

    // ---- K4: geometry is clamped ---------------------------------------------------------
    {
        doc.setBeats (0, 99);
        doc.setDivisions (0, -4);
        check ("beats clamps to 1..8", doc.getBeats (0) >= 1 && doc.getBeats (0) <= 8);
        check ("divisions clamps to 2..8", doc.getDivisions (0) >= 2 && doc.getDivisions (0) <= 8);

        doc.setBeats (0, 4);
        doc.setDivisions (0, 4);
        check ("totalDivisions follows the geometry", doc.totalDivisions (0) == 16);
    }

    // ---- K5: clearLane ----------------------------------------------------------------
    {
        doc.addBlock (0, 5, 0, 2);
        doc.addBlock (0, 5, 4, 2);
        doc.addBlock (0, 6, 0, 2);
        check ("blocks exist before clearing", doc.hasBlockAt (0, 5, 0) && doc.hasBlockAt (0, 5, 4));

        doc.clearLane (0, 5);
        check ("clearLane empties its lane",
               ! doc.hasBlockAt (0, 5, 0) && ! doc.hasBlockAt (0, 5, 4));
        check ("clearLane leaves other lanes alone", doc.hasBlockAt (0, 6, 0));
    }

    // ---- K6: edits reach the audio thread ----------------------------------------------
    {
        doc.clearLane (0, 7);
        doc.publish();
        const auto* before = store.get (0);
        const int countBefore = before != nullptr ? before->lanes[7].numBlocks : -1;

        doc.addBlock (0, 7, 8, 4);
        doc.publish();
        const auto* after = store.get (0);
        const int countAfter = after != nullptr ? after->lanes[7].numBlocks : -1;

        printf ("  lane 7 blocks: %d -> %d after an edit and publish\n", countBefore, countAfter);
        check ("publishing makes an edit visible to the audio thread", countAfter == countBefore + 1);
    }

    printf ("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// (l) The four v2 effects, with determinism as the headline property.
//
// Shuffler and Stretcher randomise. If either reached for a global RNG the output would
// depend on how the host chunked the buffer, so the same project would render differently at
// a different buffer size and an offline bounce would not match what was heard. The
// block-size invariance check below is the one that catches that specifically -- a
// same-seed-twice check alone would pass even with a global generator, as long as both runs
// used the same chunking.
bool testNewEffects()
{
    printf ("\n[Test L] v2 effects: Stretcher, Shuffler, Delay, Distortion\n");

    using namespace stutter;
    bool ok = true;
    auto check = [&ok] (const char* what, bool cond)
    {
        if (! cond) { printf ("  FAIL: %s\n", what); ok = false; }
    };

    auto makeEffect = [] (int lane) -> std::unique_ptr<LaneEffect>
    {
        switch (lane)
        {
            case StutterAudioProcessor::laneStretcher: return std::make_unique<StretcherEffect>();
            case StutterAudioProcessor::laneShuffler:  return std::make_unique<ShufflerEffect>();
            case StutterAudioProcessor::laneDelay:     return std::make_unique<DelayEffect>();
            default:                                   return std::make_unique<DistortionEffect>();
        }
    };

    struct LaneCase { int lane; const char* name; };
    const LaneCase cases[] = {
        { StutterAudioProcessor::laneStretcher, "Stretcher" },
        { StutterAudioProcessor::laneShuffler,  "Shuffler"  },
        { StutterAudioProcessor::laneDelay,     "Delay"     },
        { StutterAudioProcessor::laneDistort,   "Distort"   },
    };

    // Render one lane, at a chosen block size, with a chosen seed.
    auto render = [&] (int lane, int blockSize, uint32_t seed)
    {
        CaptureBuffer cap; cap.prepare (kSampleRate, 2, 2.5);
        BlockSequencer seq;
        seq.setLaneEffect (lane, makeEffect (lane));
        seq.prepare (kSampleRate, 2);
        seq.setEnabled (true);

        SceneSnapshot scene {};
        scene.beats = 4; scene.divisions = 4; scene.seed = seed;
        auto& laneSnap = scene.lanes[(size_t) lane];
        laneSnap.blocks[0].startDiv = 0;
        laneSnap.blocks[0].lengthDiv = 16;
        laneSnap.numBlocks = 1;
        if (auto* eff = seq.getLaneEffect (lane))
        {
            const auto set = eff->getParamDescriptors();
            for (int i = 0; i < set.count && i < maxParamsPerLane; ++i)
                laneSnap.params[(size_t) i] = set[i].defaultValue;
        }
        seq.updateChainOrder (scene);

        const int preRoll = (int) kSampleRate;
        const int total = preRoll + (int) (kSampleRate * 1.0);
        juce::AudioBuffer<float> source (2, total);
        fillTestSignal (source, kSampleRate, kBpm);

        juce::AudioBuffer<float> out (2, total);
        out.clear();

        const double pps = (kBpm / 60.0) / kSampleRate;
        double clock = 0.0;
        for (int pos = 0; pos < total; pos += blockSize)
        {
            const int n = juce::jmin (blockSize, total - pos);
            juce::AudioBuffer<float> blk (2, n);
            for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, source, c, pos, n);
            cap.write (blk);
            // The scene is active throughout. Switching it mid-render would engage on
            // whichever block boundary each chunk size happens to provide, which is a
            // property of the harness rather than of the effect -- and it masks exactly the
            // block-size dependence this test exists to detect.
            seq.processBlock (blk, cap, scene, clock, pps);
            clock += pps * (double) n;
            for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
        }
        return out;
    };

    auto maxDiff = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        double m = 0.0;
        const int n = juce::jmin (a.getNumSamples(), b.getNumSamples());
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                m = juce::jmax (m, std::abs ((double) a.getReadPointer (c)[i]
                                             - (double) b.getReadPointer (c)[i]));
        return m;
    };

    for (const auto& lc : cases)
    {
        const auto a = render (lc.lane, 512, 1234u);
        const auto metrics = analyze (a, kClickThreshold, kDiscontinuityThreshold);

        // Produces audio at all, and does not click.
        const bool audible = metrics.rms > 0.001;
        const bool clean = metrics.severeClickCount == 0;

        // Same seed, same block size -> identical.
        const auto b = render (lc.lane, 512, 1234u);
        const bool repeatable = maxDiff (a, b) == 0.0;

        // Same seed, DIFFERENT block size -> still identical. This is the check that a
        // global RNG would fail.
        const auto c = render (lc.lane, 128, 1234u);
        const double blockDiff = maxDiff (a, c);
        const bool blockInvariant = blockDiff == 0.0;

        printf ("  %-10s rms=%.4f clicks=%d repeatable=%s blockInvariant=%s (diff %.9g)\n",
                lc.name, metrics.rms, metrics.severeClickCount,
                repeatable ? "yes" : "NO", blockInvariant ? "yes" : "NO", blockDiff);

        if (! audible)        { printf ("    FAIL: %s produced no audio\n", lc.name); ok = false; }
        if (! clean)          { printf ("    FAIL: %s clicks\n", lc.name); ok = false; }
        if (! repeatable)     { printf ("    FAIL: %s is not repeatable\n", lc.name); ok = false; }
        if (! blockInvariant) { printf ("    FAIL: %s depends on block size\n", lc.name); ok = false; }
    }

    // A different seed must actually change the seeded effects, or the seed is not wired up.
    {
        const auto s1 = render (StutterAudioProcessor::laneShuffler, 512, 1u);
        const auto s2 = render (StutterAudioProcessor::laneShuffler, 512, 999u);
        const double d = maxDiff (s1, s2);
        printf ("  Shuffler seed 1 vs 999: maxDiff = %.6f\n", d);
        check ("a different seed changes the Shuffler's choices", d > 0.001);
    }

    printf ("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// (m) Factory scene banks parse, bake, and produce audio.
//
// Presets are data, and data that nothing loads is data nobody notices is broken. Each bank
// is built, published, and rendered here, so a schema change that invalidates a preset fails
// the build rather than surfacing as a silent scene in someone's session.
bool testFactoryScenes()
{
    printf ("\n[Test M] factory scene banks\n");

    using namespace stutter;
    bool ok = true;

    const int numBanks = FactoryScenes::getNumBanks();
    if (numBanks <= 0)
    {
        printf ("  FAIL: no factory banks\n");
        return false;
    }

    for (int b = 0; b < numBanks; ++b)
    {
        const auto name = FactoryScenes::getBankName (b);
        const auto tree = FactoryScenes::createBank (b);

        if (! tree.isValid() || tree.getNumChildren() == 0)
        {
            printf ("  FAIL: bank %d (%s) is empty\n", b, name.toRawUTF8());
            ok = false;
            continue;
        }

        SceneStore store;
        store.rebuildFromTree (tree);

        int populated = 0, withBlocks = 0, withCurves = 0;
        for (int i = 0; i < maxScenes; ++i)
        {
            const auto* s = store.get (i);
            if (s == nullptr || ! s->populated)
                continue;
            ++populated;

            int blocks = 0;
            for (int l = 0; l < maxLanes; ++l)
                blocks += s->lanes[(size_t) l].numBlocks;
            if (blocks > 0)
                ++withBlocks;
            if (s->numActiveCurves > 0)
                ++withCurves;

            // Geometry must be in range, or the sequencer would be handed a pattern it
            // cannot iterate.
            if (s->beats < 1 || s->beats > 8 || s->divisions < 2 || s->divisions > 8)
            {
                printf ("  FAIL: bank %d scene %d has out-of-range geometry (%d x %d)\n",
                        b, i, s->beats, s->divisions);
                ok = false;
            }
        }

        printf ("  bank %d %-20s scenes=%d withBlocks=%d withCurves=%d\n",
                b, name.toRawUTF8(), populated, withBlocks, withCurves);

        if (populated == 0) { printf ("    FAIL: no scenes survived baking\n"); ok = false; }
        if (withBlocks == 0) { printf ("    FAIL: no scene has any blocks\n"); ok = false; }

        // Render the first populated scene: a bank that bakes but makes no sound is not a
        // usable preset.
        for (int i = 0; i < maxScenes; ++i)
        {
            const auto* s = store.get (i);
            if (s == nullptr || ! s->populated)
                continue;

            int blocks = 0;
            for (int l = 0; l < maxLanes; ++l)
                blocks += s->lanes[(size_t) l].numBlocks;
            if (blocks == 0)
                continue;

            CaptureBuffer cap; cap.prepare (kSampleRate, 2, 2.5);
            BlockSequencer seq;
            seq.setLaneEffect (StutterAudioProcessor::laneStutter, std::make_unique<StutterEffect>());
            seq.setLaneEffect (StutterAudioProcessor::laneTapeStop, std::make_unique<TapeStopEffect>());
            seq.setLaneEffect (StutterAudioProcessor::laneTapeStart, std::make_unique<TapeStartEffect>());
            seq.setLaneEffect (StutterAudioProcessor::laneReverse, std::make_unique<ReverseEffect>());
            seq.setLaneEffect (StutterAudioProcessor::laneGate, std::make_unique<GateEffect>());
            seq.setLaneEffect (StutterAudioProcessor::laneFilter, std::make_unique<FilterEffect>());
            seq.setLaneEffect (StutterAudioProcessor::laneCrush, std::make_unique<CrushEffect>());
            seq.setLaneEffect (StutterAudioProcessor::laneStretcher, std::make_unique<StretcherEffect>());
            seq.setLaneEffect (StutterAudioProcessor::laneShuffler, std::make_unique<ShufflerEffect>());
            seq.setLaneEffect (StutterAudioProcessor::laneDelay, std::make_unique<DelayEffect>());
            seq.setLaneEffect (StutterAudioProcessor::laneDistort, std::make_unique<DistortionEffect>());
            seq.prepare (kSampleRate, 2);
            seq.setEnabled (true);
            seq.updateChainOrder (*s);

            ModulationEngine mod;
            mod.prepare (kSampleRate);

            const int total = (int) (kSampleRate * 2.0);
            juce::AudioBuffer<float> source (2, total);
            fillTestSignal (source, kSampleRate, kBpm);

            const double pps = (kBpm / 60.0) / kSampleRate;
            double clock = 0.0;
            juce::AudioBuffer<float> out (2, total);
            out.clear();
            for (int pos = 0; pos < total; pos += kBlockSize)
            {
                const int n = juce::jmin (kBlockSize, total - pos);
                juce::AudioBuffer<float> blk (2, n);
                for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, source, c, pos, n);
                cap.write (blk);
                seq.processBlock (blk, cap, *s, clock, pps, &mod);
                clock += pps * (double) n;
                for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, blk, c, 0, n);
            }

            const auto m = analyze (out, kClickThreshold, kDiscontinuityThreshold);
            printf ("    scene %d renders: rms=%.4f clicks=%d\n", i, m.rms, m.severeClickCount);

            if (m.severeClickCount != 0)
            {
                printf ("    FAIL: bank %d scene %d clicks\n", b, i);
                ok = false;
            }
            // Nothing finite means NaN escaped somewhere, which would be silent in an RMS
            // check but lethal in a host.
            bool finite = true;
            for (int c = 0; c < 2 && finite; ++c)
                for (int k = 0; k < total; ++k)
                    if (! std::isfinite (out.getReadPointer (c)[k])) { finite = false; break; }
            if (! finite)
            {
                printf ("    FAIL: bank %d scene %d produced non-finite output\n", b, i);
                ok = false;
            }
            break;
        }
    }

    printf ("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// (n) The APVTS mirror: switching scenes updates the parameters the UI and host see.
//
// Without this a MIDI-triggered scene change alters what is heard but leaves every knob
// showing the previous scene's values -- the plugin would sound one way and look another.
// The feedback check matters as much as the mirroring: the parameter writes must not be
// read back as user edits, or every scene switch would silently rewrite the scene it came
// from.
bool testApvtsMirror()
{
    printf ("\n[Test N] APVTS mirrors the active scene\n");

    using namespace stutter;
    bool ok = true;
    auto check = [&ok] (const char* what, bool cond)
    {
        if (! cond) { printf ("  FAIL: %s\n", what); ok = false; }
    };

    StutterAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
    proc.prepareToPlay (kSampleRate, kBlockSize);

    auto& apvts = proc.getAPVTS();
    auto& doc = proc.getSceneDocument();
    auto& engine = proc.getGestureEngine();

    // Give two scenes distinguishable values for the same parameter. Filter cutoff is a
    // convenient probe: wide range, and continuous so the value round-trips exactly.
    const juce::String cutoffId = ID::lanePrefix (StutterAudioProcessor::laneFilter) + "cutoff";

    auto setSceneCutoff = [&doc] (int sceneIdx, float hz)
    {
        auto scene = doc.ensureScene (sceneIdx);
        auto lp = scene.getOrCreateChildWithName (SceneIDs::laneParams, nullptr);
        juce::ValueTree laneNode (SceneIDs::lane);
        laneNode.setProperty (SceneIDs::index, StutterAudioProcessor::laneFilter, nullptr);
        juce::ValueTree pt (SceneIDs::param);
        pt.setProperty (SceneIDs::paramIndexProp, 1, nullptr);   // descriptor slot 1 = cutoff
        pt.setProperty (SceneIDs::value, hz, nullptr);
        laneNode.appendChild (pt, nullptr);
        lp.appendChild (laneNode, nullptr);
    };

    setSceneCutoff (10, 400.0f);
    setSceneCutoff (11, 8000.0f);
    doc.publish();

    // Drive the mirror the way the audio thread would: flag a scene, then let the timer run.
    auto switchAndMirror = [&] (int sceneIdx)
    {
        // Drive it the way a player would -- a note-on, which is what selects a scene in
        // MIDI mode. Setting the scene directly first would make the trigger a no-op and
        // silently skip the mirror.
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, sceneIdx, 1.0f), 0);
        juce::AudioBuffer<float> b (2, kBlockSize);
        b.clear();
        proc.processBlock (b, m);

        // The mirror normally runs on the processor's timer; drive it directly here so the
        // test does not depend on message-loop scheduling.
        proc.pumpSceneMirror();
    };

    switchAndMirror (10);
    const float after10 = apvts.getRawParameterValue (cutoffId)->load();

    switchAndMirror (11);
    const float after11 = apvts.getRawParameterValue (cutoffId)->load();

    printf ("  scene 10 -> cutoff %.1f Hz, scene 11 -> cutoff %.1f Hz\n", after10, after11);
    check ("scene 10 mirrors its own cutoff", std::abs (after10 - 400.0f) < 5.0f);
    check ("scene 11 mirrors its own cutoff", std::abs (after11 - 8000.0f) < 50.0f);

    // No feedback: mirroring scene 11 must not have rewritten scene 10.
    switchAndMirror (10);
    const float back10 = apvts.getRawParameterValue (cutoffId)->load();
    printf ("  returning to scene 10 -> cutoff %.1f Hz\n", back10);
    check ("switching away and back preserves the original scene",
           std::abs (back10 - 400.0f) < 5.0f);

    return ok;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit; // safe/no-op headless; ensures MessageManager exists for APVTS

    const juce::File outputDirectory = argc > 1
        ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])
        : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("stutter_render_test");
    outputDirectory.createDirectory();
    printf ("output dir: %s\n", outputDirectory.getFullPathName().toRawUTF8());

    // Pre-roll: run the test signal through the capture buffer for ~1s with the sequencer
    // pattern still off before enabling all 16 steps, so buffer-category lanes (TapeStop/
    // TapeStart/Stutter/Reverse/Repitch) have real prior audio history to anchor into instead of
    // the artificial silence that would otherwise sit before sample 0 of a cold CaptureBuffer.
    // This mirrors realistic use (audio was already playing before the user engages the effect)
    // and avoids a cold-start artifact that has nothing to do with the fix being verified.
    const double preRollSeconds = 1.0;
    const int preRollSamples = (int) std::round (preRollSeconds * kSampleRate);

    // Full test signal, generated once (pre-roll + rendered region), long enough to feed every render.
    const int totalStepsToRender = kNumBars * stutter::numSteps;
    const double stepLengthSeconds = (60.0 / kBpm) / 4.0; // 16th note
    const int renderSamples = (int) std::round (totalStepsToRender * stepLengthSeconds * kSampleRate) + kBlockSize;
    const int totalSamples = preRollSamples + renderSamples;

    juce::AudioBuffer<float> sourceSignal (2, totalSamples);
    fillTestSignal (sourceSignal, kSampleRate, kBpm);

    juce::WavAudioFormat wavFormat;
    bool anyFailures = false;

    printf ("(discontinuity threshold = %.6f = 2x clean 220Hz sine baseline)\n", kDiscontinuityThreshold);
    printf ("%-10s %12s %10s %14s %12s %10s\n", "Lane", "maxDelta", ">0.3", ">discont", "RMS", "rangeViol");
    printf ("--------------------------------------------------------------------------------------\n");

    for (const auto& laneSpec : kLanes)
    {
        // Reset the slice-window read-range violation counter for this lane's render
        // (incremented by the effects whenever a capture-buffer read falls outside the
        // [anchorAbs - sliceLen, anchorAbs] window they claim to be reading from).
        stutter::debug::sliceRangeViolations.store (0);

        StutterAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, kSampleRate, kBlockSize);
        processor.prepareToPlay (kSampleRate, kBlockSize);

        auto& apvts = processor.getAPVTS();
        // hostSync OFF -> internal free-running clock drives the sequencer.
        apvts.getParameter (stutter::ID::hostSync)->setValueNotifyingHost (0.0f);
        setInternalBpm (processor, kBpm);

        // The audio path is the block sequencer now, so the lane is driven by writing blocks
        // into the scene document rather than by setting v1 steps. Sixteen 1-division blocks
        // describe the same pattern the old "all 16 steps on" did.
        //
        // Blocks are added AFTER the pre-roll (see the loop below) so the capture buffer
        // fills with real history before the lane is ever triggered -- buffer-category lanes
        // would otherwise anchor into the silence that precedes sample 0.
        auto& doc = processor.getSceneDocument();
        const int renderScene = processor.getGestureEngine().getActiveScene();

        juce::AudioBuffer<float> renderedOutput (2, totalSamples);
        renderedOutput.clear();

        int pos = 0;
        juce::MidiBuffer midi;
        bool stepsEnabled = false;
        while (pos < totalSamples)
        {
            if (! stepsEnabled && pos >= preRollSamples)
            {
                for (int d = 0; d < stutter::numSteps; ++d)
                    doc.addBlock (renderScene, laneSpec.laneIndex, d, 1);
                doc.publish();
                stepsEnabled = true;
            }

            const int n = juce::jmin (kBlockSize, totalSamples - pos);

            juce::AudioBuffer<float> block (2, kBlockSize);
            block.clear();
            for (int c = 0; c < 2; ++c)
                block.copyFrom (c, 0, sourceSignal, c, pos, n);

            midi.clear();
            processor.processBlock (block, midi);

            for (int c = 0; c < 2; ++c)
                renderedOutput.copyFrom (c, pos, block, c, 0, n);

            pos += n;
        }

        // Analyze/write out only the post-pre-roll region -- the pre-roll's own dry passthrough
        // (steps off) isn't part of what's being verified. Skip one extra block after that region
        // starts (transient from filters/gate ramps settling from zero state); we care about
        // steady-state discontinuities, not the unavoidable single cold-start edge shared by
        // every lane.
        const int analysisStart = juce::jmin (preRollSamples + kBlockSize, totalSamples);
        juce::AudioBuffer<float> analysisView (renderedOutput.getArrayOfWritePointers(), 2,
                                                analysisStart, totalSamples - analysisStart);
        const Metrics m = analyze (analysisView, kClickThreshold, kDiscontinuityThreshold);
        const long long rangeViolations = (long long) stutter::debug::sliceRangeViolations.load();

        printf ("%-10s %12.6f %10d %14d %12.6f %10lld\n", laneSpec.name, (double) m.maxAdjacentDelta,
                m.severeClickCount, m.discontinuityCount, m.rms, rangeViolations);

        const bool pass = m.severeClickCount == 0 && rangeViolations == 0;
        if (! pass)
            anyFailures = true;

        // Write only the rendered (post-pre-roll) region to WAV for manual listen/inspection.
        const juce::File wavFile = outputDirectory.getChildFile (juce::String (laneSpec.name) + ".wav");
        wavFile.deleteFile();
        std::unique_ptr<juce::FileOutputStream> fos (wavFile.createOutputStream());
        if (fos != nullptr)
        {
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wavFormat.createWriterFor (fos.get(), kSampleRate, (unsigned int) renderedOutput.getNumChannels(),
                                            24, {}, 0));
            if (writer != nullptr)
            {
                juce::AudioBuffer<float> wavView (renderedOutput.getArrayOfWritePointers(), 2,
                                                   preRollSamples, totalSamples - preRollSamples);
                fos.release(); // writer now owns the stream
                writer->writeFromAudioSampleBuffer (wavView, 0, wavView.getNumSamples());
            }
        }
    }

    printf ("--------------------------------------------------------------------------------------\n");
    printf ("pass = (>%.2f count == 0) AND (rangeViol == 0); %s\n", kClickThreshold,
            anyFailures ? "SOME LANES FAILED" : "ALL LANES PASS");

    const bool testAPass = testFreshInstanceIsTransparent();
    const bool testBPass = testPresetTransitionResetsCurves();
    const bool testCPass = testMalformedCurveTreeFixtures();
    const bool testDPass = testSequencerOffBypassesStepsButCurvesStillWork();
    const bool testEPass = testApvtsInternalBpmChangesFreeRunSpeed();
    const bool testFPass = testRateLabelsMatchActualDurations();
    const bool testGPass = testSceneSchemaAndStore();
    const bool testHPass = testBlockSequencerBehaviour();
    const bool testIPass = testGestureEngine();
    const bool testJPass = testModulationEngine();
    const bool testKPass = testSceneDocument();
    const bool testLPass = testNewEffects();
    const bool testMPass = testFactoryScenes();
    const bool testNPass = testApvtsMirror();
    if (! testAPass || ! testBPass || ! testCPass || ! testDPass || ! testEPass || ! testFPass
        || ! testGPass || ! testHPass || ! testIPass || ! testJPass || ! testKPass || ! testLPass || ! testMPass || ! testNPass)
        anyFailures = true;

    printf ("\n========================================================================================\n");
    printf ("OVERALL: %s\n", anyFailures ? "FAIL" : "PASS");

    return anyFailures ? 1 : 0;
}
