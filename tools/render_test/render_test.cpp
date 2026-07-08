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

void setAllStepsOn (stutter::StepSequencer& seq, int lane)
{
    for (int s = 0; s < stutter::numSteps; ++s)
        seq.setStep (lane, s, true);
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
    processor.setInternalBpm (kBpm);
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

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit; // safe/no-op headless; ensures MessageManager exists for APVTS

    const juce::String outDir = argc > 1 ? juce::String (argv[1]) : juce::String (".");
    juce::File outputDirectory (outDir);
    outputDirectory.createDirectory();

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
        processor.setInternalBpm (kBpm);

        auto& seq = processor.getSequencer();
        seq.setEnabled (true);
        // Pattern starts with all steps off during the pre-roll (see preRollSamples above) so
        // the capture buffer fills with real history before the lane is ever triggered.

        juce::AudioBuffer<float> renderedOutput (2, totalSamples);
        renderedOutput.clear();

        int pos = 0;
        juce::MidiBuffer midi;
        bool stepsEnabled = false;
        while (pos < totalSamples)
        {
            if (! stepsEnabled && pos >= preRollSamples)
            {
                setAllStepsOn (seq, laneSpec.laneIndex);
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
    if (! testAPass || ! testBPass || ! testCPass)
        anyFailures = true;

    printf ("\n========================================================================================\n");
    printf ("OVERALL: %s\n", anyFailures ? "FAIL" : "PASS");

    return anyFailures ? 1 : 0;
}
