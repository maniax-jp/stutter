#pragma once
#include "../LaneEffectV2.h"
#include "../TimingMode.h"
#include <array>
#include <vector>

namespace stutter
{

/**
    Delay with Glitch 2's Send / Feedback / Return topology rather than a plain mix knob.

    Send/Feedback/Return is worth the extra control because it decouples "how much goes in"
    from "how much comes out": you can cut the return to silence while the line keeps ringing,
    then bring it back, which a single mix control cannot express. Glitch 2's slap-back recipe
    (Send 50%, Feedback 0%, Return 100%) is only reachable with the three separated.

    This is the Send category, and it is why that category exists. Feeding a delay's output
    back through a serial chain that also feeds the delay is re-entrant; running it as a
    send/return keeps the feedback path closed and bounded.

    Owns its own line rather than reading CaptureBuffer, because feedback requires writing
    processed audio back -- CaptureBuffer holds the dry input and must stay that way.
*/
class DelayEffect : public LaneEffect
{
public:
    DelayEffect() : LaneEffect (LaneCategory::Send) {}

    const char* getName() const noexcept override { return "Delay"; }

    RetriggerPolicy getRetriggerPolicy() const noexcept override
    {
        // A delay's tail is the point; re-latching per block would reset the line and make
        // it impossible to hold a wash across several blocks.
        return RetriggerPolicy::ContinueThroughRun;
    }

    ParamDescriptorSet getParamDescriptors() const noexcept override
    {
        static constexpr const char* timeChoices[] = { "1/4", "1/8", "1/16", "1/32", "1/8T", "1/8." };
        static constexpr ParamDescriptor descs[] = {
            { "time",     "Delay Time",     0.0f, 5.0f, 2.0f, 1.0f, 1.0f, "", timeChoices, 6, true,  true },
            { "feedback", "Delay Feedback", 0.0f, 0.95f, 0.4f, 0.0f, 1.0f, "", nullptr,    0, false, true },
            { "send",     "Delay Send",     0.0f, 1.0f, 0.7f, 0.0f, 1.0f, "", nullptr,     0, false, true },
            { "ret",      "Delay Return",   0.0f, 1.0f, 0.7f, 0.0f, 1.0f, "", nullptr,     0, false, true },
            { "slew",     "Delay Slew",     0.0f, 1.0f, 0.5f, 0.0f, 1.0f, "", nullptr,     0, false, true },
        };
        return { descs, (int) (sizeof (descs) / sizeof (descs[0])) };
    }

    void prepare (double sampleRateIn, int numChannelsIn) override
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jmin (numChannelsIn, maxChannels);

        // 4 seconds is long enough for a 1/4 note down to 30 BPM. Allocated once here so the
        // audio thread never resizes.
        lineLength = juce::jmax (1024, (int) (sampleRate * 4.0));
        for (auto& line : lines)
            line.assign ((size_t) lineLength, 0.0f);

        reset();
    }

    void reset() override
    {
        writePos = 0;
        currentDelay = sampleRate * 0.25;
        targetDelay = currentDelay;
        for (auto& line : lines)
            std::fill (line.begin(), line.end(), 0.0f);
    }

    void onBlockStart (const CaptureBuffer& capture, const LaneParams& params,
                       const BlockContext& ctx) override
    {
        juce::ignoreUnused (capture);

        static constexpr double fractions[] = { 1.0, 0.5, 0.25, 0.125, 0.5 * (2.0 / 3.0), 0.5 * 1.5 };
        const int idx = juce::jlimit (0, 5, params.getIndex (0));

        // Times are quarter-note relative, matching the rate table's convention (see
        // TimingMode.h) so "1/4" here means the same length it means on Stutter.
        targetDelay = juce::jlimit (16.0, (double) lineLength - 4.0,
                                    ctx.divisionLengthSamples * 4.0 * fractions[idx]);

        feedback = juce::jlimit (0.0f, 0.95f, params.get (1));
        send = juce::jlimit (0.0f, 1.0f, params.get (2));
        returnAmt = juce::jlimit (0.0f, 1.0f, params.get (3));
        slew = juce::jlimit (0.0f, 1.0f, params.get (4));
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh,
                        const SampleContext& ctx) override
    {
        juce::ignoreUnused (capture);

        if (ctx.modulatedParams != nullptr)
        {
            feedback = juce::jlimit (0.0f, 0.95f, ctx.modulatedParams[1]);
            send = juce::jlimit (0.0f, 1.0f, ctx.modulatedParams[2]);
            returnAmt = juce::jlimit (0.0f, 1.0f, ctx.modulatedParams[3]);
            slew = juce::jlimit (0.0f, 1.0f, ctx.modulatedParams[4]);
        }

        // Slew: how fast the read head chases a new delay time. Low values pitch-bend the
        // tail as the line stretches -- the dub-delay warp -- while high values snap. A jump
        // straight to the target would click, so there is no "instant" setting.
        const double slewRate = 0.00002 + (double) slew * 0.01;
        currentDelay += (targetDelay - currentDelay) * slewRate;

        const double readPos = (double) writePos - currentDelay;

        for (int c = 0; c < numCh && c < maxChannels; ++c)
        {
            const float delayed = readInterpolated (c, readPos);
            const float input = channelSamples[c];

            // Write the send plus the feedback of what came out; the return scales only what
            // reaches the output, so cutting it leaves the line ringing.
            const float toLine = input * send + delayed * feedback;
            lines[(size_t) c][(size_t) writePos] = juce::jlimit (-4.0f, 4.0f, toLine);

            channelSamples[c] = input + delayed * returnAmt;
        }

        if (++writePos >= lineLength)
            writePos = 0;
    }

private:
    float readInterpolated (int channel, double pos) const noexcept
    {
        const auto& line = lines[(size_t) channel];
        if (line.empty())
            return 0.0f;

        double p = pos;
        while (p < 0.0)
            p += (double) lineLength;

        const int i0 = ((int) p) % lineLength;
        const int i1 = (i0 + 1) % lineLength;
        const float frac = (float) (p - std::floor (p));
        return line[(size_t) i0] + frac * (line[(size_t) i1] - line[(size_t) i0]);
    }

    static constexpr int maxChannels = 8;

    double sampleRate = 44100.0;
    int numChannels = 2;

    std::array<std::vector<float>, maxChannels> lines;
    int lineLength = 0;
    int writePos = 0;

    double currentDelay = 11025.0;
    double targetDelay = 11025.0;
    float feedback = 0.4f;
    float send = 0.7f;
    float returnAmt = 0.7f;
    float slew = 0.5f;
};

} // namespace stutter
