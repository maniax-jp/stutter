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
        reset();
    }

    void reset() override
    {
        readPosSamples = 0.0;
        sliceLenSamples = sampleRate * 0.25;
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples) override
    {
        juce::ignoreUnused (capture);
        const float sliceLenParam = getParam (ID::reverseSliceLen, 2.0f); // divisions index like stutter rate
        const double fraction = rateToFraction ((int) sliceLenParam);
        sliceLenSamples = juce::jmax (32.0, stepLengthSamples * fraction * 4.0);
        readPosSamples = 0.0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress) override
    {
        juce::ignoreUnused (progress);

        // Read from (sliceLen - readPos) samples ago, moving backward in time as readPos increases,
        // which plays the slice in reverse order.
        const double samplesAgo = juce::jmax (0.0, sliceLenSamples - readPosSamples);
        for (int c = 0; c < numCh; ++c)
            channelSamples[c] = capture.readInterpolated (c, samplesAgo);

        readPosSamples += 1.0;
        if (readPosSamples >= sliceLenSamples)
            readPosSamples = 0.0;
    }

private:
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
};

} // namespace stutter
