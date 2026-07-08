// Offline verification harness for the Stutter noise-fix work.
//
// Instantiates StutterAudioProcessor directly (no plugin wrapper/host involved), drives it
// with a synthetic "loud/quiet alternating" test signal, and renders each lane soloed (all 16
// steps on) for 4 bars at 120 BPM / 48kHz / block=512, with hostSync OFF so the internal
// free-running clock is used. For each lane it writes a WAV file and reports discontinuity
// metrics (max adjacent-sample delta, count of deltas over a threshold, RMS) so the effect of
// the DSP fix can be measured numerically before/after.
//
// Usage: render_test <output-directory>

#include "PluginProcessor.h"
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

    return anyFailures ? 1 : 0;
}
