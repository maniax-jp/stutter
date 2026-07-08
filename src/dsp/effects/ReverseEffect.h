#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace stutter
{

/** Reverse: plays the most recent slice of the capture buffer backwards. */
class ReverseEffect : public LaneEffect
{
public:
    ReverseEffect (juce::AudioProcessorValueTreeState& state, int laneIdx)
        : LaneEffect (LaneCategory::Buffer), apvts (state), laneIndex (laneIdx) {}

    void prepare (double sampleRateIn, int numChannelsIn) override
    {
        sampleRate = sampleRateIn;
        numChannels = numChannelsIn;
        retrigSmoother.prepare (sampleRate);
        reset();
    }

    void reset() override
    {
        readPosSamples = 0.0;
        sliceLenSamples = sampleRate * 0.25;
        anchorAbs = 0;
        retrigSmoother.reset();
    }

    void onStepEnd() override
    {
        retrigSmoother.reset();
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples, juce::int64 nowAbs) override
    {
        juce::ignoreUnused (capture);
        // Blend from the previous output into the newly-anchored slice on a per-step retrigger
        // (the outer sequencer's gain crossfade can't mask this: gain is already 1.0).
        retrigSmoother.notifyRetrigger();
        const float sliceLenParam = getParam (ID::reverseSliceLen, 2.0f); // divisions index like stutter rate
        const double fraction = rateToFraction ((int) sliceLenParam);
        sliceLenSamples = juce::jmax (32.0, stepLengthSamples * fraction * 4.0);
        readPosSamples = 0.0;
        // Fixed anchor: the reversed slice is the sliceLenSamples immediately before "now",
        // frozen for the lifetime of this trigger (see CaptureBuffer::readInterpolatedAbsolute).
        anchorAbs = nowAbs;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress,
                         juce::int64 nowAbs) override
    {
        juce::ignoreUnused (progress, nowAbs);

        // Slice spans absolute positions [anchorAbs - sliceLen, anchorAbs]. readPos moving
        // 0 -> sliceLen steps the read point from the slice's end back toward its start, which
        // plays the frozen slice in reverse order.
        const double fadeLen = loopCrossfadeSamples();
        const double sliceStart = (double) anchorAbs - sliceLenSamples;
        const double absPos = (double) anchorAbs - readPosSamples;

        // Warp crossfade into the next revolution across the slice wrap point: when Reverse
        // repeats, the next revolution restarts at the slice's *end* (anchorAbs) and moves
        // backward again -- so the fade partner starts at anchorAbs and walks backward
        // (anchorAbs - fadeProgress), previewing exactly the audio that will play after the
        // wrap. After the wrap, readPosSamples resumes at fadeLen (below) so those samples are
        // not played twice. Both read positions stay inside [anchorAbs - sliceLen, anchorAbs] by
        // construction: the main read because readPos in [0, sliceLen), the partner because it
        // spans (anchorAbs - fadeLen, anchorAbs] with fadeLen <= 0.1 * sliceLen.
        const bool inFade = readPosSamples >= sliceLenSamples - fadeLen;
        double wrapAbsPos = 0.0;
        float gOut = 1.0f, gIn = 0.0f;
        if (inFade)
        {
            const double fadeProgress = readPosSamples - (sliceLenSamples - fadeLen);
            const double fadeT = juce::jlimit (0.0, 1.0, fadeLen > 0.0 ? fadeProgress / fadeLen : 1.0);
            wrapAbsPos = (double) anchorAbs - fadeProgress;
            gOut = std::cos (0.5f * juce::MathConstants<float>::pi * (float) fadeT);
            gIn = std::sin (0.5f * juce::MathConstants<float>::pi * (float) fadeT);
        }

        STUTTER_CHECK_SLICE_RANGE (absPos, sliceStart, (double) anchorAbs);
        if (inFade)
            STUTTER_CHECK_SLICE_RANGE (wrapAbsPos, sliceStart, (double) anchorAbs);

        for (int c = 0; c < numCh; ++c)
        {
            float sample = capture.readInterpolatedAbsolute (c, absPos);
            if (inFade)
                sample = sample * gOut + capture.readInterpolatedAbsolute (c, wrapAbsPos) * gIn;
            channelSamples[c] = sample;
        }

        retrigSmoother.process (channelSamples, numCh);

        readPosSamples += 1.0;
        if (readPosSamples >= sliceLenSamples)
        {
            // Resume past the samples the fade partner already played (see fade comment).
            readPosSamples = fadeLen + (readPosSamples - sliceLenSamples);
        }
    }

private:
    double loopCrossfadeSamples() const noexcept
    {
        const double msSamples = sampleRate * 0.003; // 3ms
        return juce::jmin (msSamples, sliceLenSamples * 0.1);
    }

    static double rateToFraction (int index)
    {
        static const double table[] = {
            1.0 / 4.0, 1.0 / 8.0, 1.0 / 16.0, 1.0 / 32.0, 1.0 / 64.0,
            (1.0 / 4.0) * (2.0 / 3.0), (1.0 / 8.0) * (2.0 / 3.0), (1.0 / 16.0) * (2.0 / 3.0),
            (1.0 / 4.0) * 1.5, (1.0 / 8.0) * 1.5, (1.0 / 16.0) * 1.5,
        };
        constexpr int n = (int) (sizeof (table) / sizeof (double));
        index = juce::jlimit (0, n - 1, index);
        return table[index];
    }

    float getParam (const juce::String& name, float fallback) const
    {
        if (auto* p = apvts.getRawParameterValue (ID::lanePrefix (laneIndex) + name))
            return p->load();
        return fallback;
    }

    juce::AudioProcessorValueTreeState& apvts;
    int laneIndex;
    double sampleRate = 44100.0;
    int numChannels = 2;

    double readPosSamples = 0.0;
    double sliceLenSamples = 11025.0;
    juce::int64 anchorAbs = 0;
    RetriggerSmoother retrigSmoother;
};

} // namespace stutter
