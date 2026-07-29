#pragma once
#include "../LaneEffectV2.h"
#include <array>

namespace stutter
{

/** Crush: bitcrusher (quantizes to N bits) + downsampler (sample-and-hold at a divided rate).
    Texture category: stacks with buffer lanes and other texture lanes. */
class CrushEffect : public LaneEffect
{
public:
    CrushEffect() : LaneEffect (LaneCategory::Texture) {}

    const char* getName() const noexcept override { return "Crush"; }

    ParamDescriptorSet getParamDescriptors() const noexcept override
    {
        static constexpr ParamDescriptor descs[] = {
            { "bitDepth", "Crush Bit Depth", 1.0f, 16.0f, 16.0f, 1.0f, 1.0f, "bit", nullptr, 0, false, true },
            { "rateDiv",  "Crush Rate Div",  0.0f,  1.0f,  0.0f, 0.0f, 1.0f, "",    nullptr, 0, false, true },
        };
        return { descs, (int) (sizeof (descs) / sizeof (descs[0])) };
    }

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

    void onBlockStart (const CaptureBuffer& capture, const LaneParams& params,
                       const BlockContext& ctx) override
    {
        juce::ignoreUnused (capture, ctx);
        bitDepth = juce::jlimit (1.0f, 16.0f, params.get (0));
        const float rateDivParam = juce::jlimit (0.0f, 1.0f, params.get (1));
        // rateDiv 0..1 maps to hold length 1..40 samples (downsample factor)
        holdLength = juce::jmax (1, (int) std::round (1.0f + rateDivParam * 39.0f));
        holdCounter = 0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh,
                        const SampleContext& ctx) override
    {
        juce::ignoreUnused (capture);

        // Both parameters are declared continuous, so they are read here rather than latched
        // at block start. Latching them meant a lane that is on for the whole bar -- one block,
        // never re-triggered during playback -- kept whatever values were current when it
        // started, so changing preset did not change the sound until the lane fired again.
        if (ctx.modulatedParams != nullptr)
        {
            bitDepth = juce::jlimit (1.0f, 16.0f, ctx.modulatedParams[0]);

            // rateDiv 0..1 maps to a hold length of 1..40 samples (the downsample factor).
            const float rateDivParam = juce::jlimit (0.0f, 1.0f, ctx.modulatedParams[1]);
            holdLength = juce::jmax (1, (int) std::round (1.0f + rateDivParam * 39.0f));
        }

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
    static constexpr int maxChannels = 8;

    double sampleRate = 44100.0;
    int numChannels = 2;

    float bitDepth = 16.0f;
    int holdLength = 1;
    int holdCounter = 0;
    std::array<float, maxChannels> heldValue {};
};

} // namespace stutter
