#pragma once
#include "PluginProcessor.h"
#include "dsp/BlockSequencer.h"
#include "dsp/CaptureBuffer.h"
#include "state/SceneDocument.h"
#include "state/SceneSchema.h"
#include "state/SceneStore.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <memory>

/**
    Shared fixtures for the test suites.

    These were originally file-local helpers in render_test.cpp. Both targets need them --
    render_test still measures discontinuity to decide what to write to disk, and the Catch2
    suites measure the same quantities to assert on them -- so they live here rather than
    being duplicated, which is how the two would otherwise drift apart and start disagreeing
    about what counts as a click.
*/
namespace stutter::test
{

inline constexpr double sampleRate = 48000.0;
inline constexpr int blockSize = 512;
inline constexpr double bpm = 120.0;
inline constexpr int numBars = 4;

/** Pass/fail bar for a severe click: an adjacent-sample delta this large is a splice
    artefact rather than signal, at any level a musical source reaches. */
inline constexpr double clickThreshold = 0.3;

/** A continuous 220Hz sine at full scale can only move this far between samples
    (2*pi*f/sr). Anything above it is a splice artefact rather than signal, which is what
    makes this the threshold that actually discriminates buzz -- the 0.3 bar above only
    catches full-scale clicks. */
inline constexpr double cleanSineMaxDelta = 3.14159265358979323846 * 2.0 * 220.0 / sampleRate;
inline constexpr double discontinuityThreshold = cleanSineMaxDelta * 2.0;

struct Metrics
{
    float maxAdjacentDelta = 0.0f;
    int severeClickCount = 0;
    int discontinuityCount = 0;
    double rms = 0.0;
    int numSamples = 0;
};

inline Metrics analyze (const juce::AudioBuffer<float>& buf,
                        double severeThreshold = clickThreshold,
                        double discThreshold = discontinuityThreshold)
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
                m.maxAdjacentDelta = juce::jmax (m.maxAdjacentDelta, delta);
                if (delta > (float) severeThreshold)   ++m.severeClickCount;
                if (delta > (float) discThreshold)     ++m.discontinuityCount;
            }
            prev = v;
        }
    }
    m.rms = std::sqrt (sumSq / (double) juce::jmax (1, n * ch));
    return m;
}

inline double rmsOf (const juce::AudioBuffer<float>& buf)
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

/** 220Hz sine alternating full/quiet each beat -- the "loud material spliced with quiet
    material" case that makes splice artefacts audible in the metrics. */
inline void fillTestSignal (juce::AudioBuffer<float>& buf,
                            double sr = sampleRate, double tempo = bpm)
{
    constexpr double freqHz = 220.0;
    const int samplesPerBeat = (int) std::round (60.0 / tempo * sr);

    for (int c = 0; c < buf.getNumChannels(); ++c)
    {
        float* d = buf.getWritePointer (c);
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            const int beatIndex = i / juce::jmax (1, samplesPerBeat);
            const float amp = (beatIndex % 2 == 0) ? 1.0f : 0.05f;
            d[i] = amp * (float) std::sin (juce::MathConstants<double>::twoPi * freqHz
                                           * (double) i / sr);
        }
    }
}

/** internalBpm is APVTS-owned, so tests must set it the way a host or preset would rather
    than poking the processor directly. */
inline void setInternalBpm (StutterAudioProcessor& processor, double tempo)
{
    if (auto* param = processor.getAPVTS().getParameter (stutter::ID::internalBpm))
    {
        const auto& range = processor.getAPVTS().getParameterRange (stutter::ID::internalBpm);
        param->setValueNotifyingHost (range.convertTo0to1 ((float) tempo));
    }
}

/** Register every lane's effect on a bare BlockSequencer. Suites that drive the sequencer
    directly (rather than through the processor) need this or the lanes are empty -- a
    mistake that already cost real debugging time once. */
void installAllLaneEffects (BlockSequencer& seq);

/** A scene with one block per division on `lane`, mirroring what "all steps on" meant under
    the v1 grid. */
SceneSnapshot makeFullLaneScene (BlockSequencer& seq, int lane, int beats = 4, int divisions = 4);

} // namespace stutter::test
