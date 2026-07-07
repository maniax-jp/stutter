#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

namespace stutter
{

/** Crush: bitcrusher (quantizes to N bits) + downsampler (sample-and-hold at a divided rate).
    Texture category: stacks with buffer lanes and other texture lanes. */
class CrushEffect : public LaneEffect
{
public:
    CrushEffect (juce::AudioProcessorValueTreeState& state, int laneIdx)
        : LaneEffect (LaneCategory::Texture), apvts (state), laneIndex (laneIdx) {}

    void prepare (double sampleRateIn, int numChannelsIn) override
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jmin (numChannelsIn, maxChannels);
        reset();
    }

    void reset() override
    {
        holdCounter = 0;
        for (auto& v : heldValue)
            v = 0.0f;
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples) override
    {
        juce::ignoreUnused (capture, stepLengthSamples);
        bitDepth = juce::jlimit (1.0f, 16.0f, getParam (ID::crushBitDepth, 16.0f));
        const float rateDivParam = juce::jlimit (0.0f, 1.0f, getParam (ID::crushRateDiv, 0.0f));
        // rateDiv 0..1 maps to hold length 1..40 samples (downsample factor)
        holdLength = juce::jmax (1, (int) std::round (1.0f + rateDivParam * 39.0f));
        holdCounter = 0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress) override
    {
        juce::ignoreUnused (capture, progress);

        const bool sampleNow = (holdCounter % holdLength) == 0;
        const float levels = std::pow (2.0f, bitDepth) - 1.0f;

        for (int c = 0; c < numCh && c < maxChannels; ++c)
        {
            if (sampleNow)
            {
                const float clamped = juce::jlimit (-1.0f, 1.0f, channelSamples[c]);
                heldValue[(size_t) c] = std::round (clamped * levels) / levels;
            }
            channelSamples[c] = heldValue[(size_t) c];
        }

        ++holdCounter;
    }

private:
    float getParam (const juce::String& name, float fallback) const
    {
        if (auto* p = apvts.getRawParameterValue (ID::lanePrefix (laneIndex) + name))
            return p->load();
        return fallback;
    }

    static constexpr int maxChannels = 8;

    juce::AudioProcessorValueTreeState& apvts;
    int laneIndex;
    double sampleRate = 44100.0;
    int numChannels = 2;

    float bitDepth = 16.0f;
    int holdLength = 1;
    int holdCounter = 0;
    std::array<float, maxChannels> heldValue {};
};

} // namespace stutter
