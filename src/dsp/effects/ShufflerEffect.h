#pragma once
#include "../LaneEffectV2.h"
#include <array>

namespace stutter
{

/**
    Shuffler: picks random slices from the capture buffer and plays them out of order, with
    independent chances to repeat or reverse each one. Glitch 2's module of the same name.

    Determinism is the load-bearing property here, not the randomness. Every choice derives
    from BlockContext::seed via a local generator; nothing touches a global RNG. That means a
    looping pattern makes identical choices every pass and an offline bounce matches what was
    heard -- which is the entire reason Glitch 2 makes the seed a first-class scene property.
    A global RNG would additionally make the output depend on how the host chunked the buffer,
    so the same project would render differently at a different buffer size.

    RetriggerEachDivision, so a long block keeps re-rolling rather than freezing on one slice.
*/
class ShufflerEffect : public LaneEffect
{
public:
    ShufflerEffect() : LaneEffect (LaneCategory::Buffer) {}

    const char* getName() const noexcept override { return "Shuffler"; }

    RetriggerPolicy getRetriggerPolicy() const noexcept override
    {
        return RetriggerPolicy::RetriggerEachDivision;
    }

    ParamDescriptorSet getParamDescriptors() const noexcept override
    {
        static constexpr const char* sliceChoices[] = { "1/4", "1/8", "1/16", "1/32" };
        static constexpr ParamDescriptor descs[] = {
            { "slice",   "Shuffle Slice",   0.0f, 3.0f, 2.0f, 1.0f, 1.0f, "", sliceChoices, 4, true, true },
            { "range",   "Shuffle Range",   0.0f, 1.0f, 0.5f, 0.0f, 1.0f, "", nullptr,      0, true, true },
            { "shuffle", "Shuffle Chance",  0.0f, 1.0f, 0.7f, 0.0f, 1.0f, "", nullptr,      0, true, true },
            { "repeat",  "Repeat Chance",   0.0f, 1.0f, 0.3f, 0.0f, 1.0f, "", nullptr,      0, true, true },
            { "reverse", "Reverse Chance",  0.0f, 1.0f, 0.2f, 0.0f, 1.0f, "", nullptr,      0, true, true },
        };
        return { descs, (int) (sizeof (descs) / sizeof (descs[0])) };
    }

    void prepare (double sampleRateIn, int numChannelsIn) override
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jmin (numChannelsIn, maxChannels);
        retrigSmoother.prepare (sampleRate);
        reset();
    }

    void reset() override
    {
        readPos = 0.0;
        sliceLen = 0.0;
        sliceStartAbs = 0;
        reversed = false;
        rngState = 1u;
        retrigSmoother.reset();
    }

    void onBlockEnd() override { retrigSmoother.reset(); }

    void onBlockStart (const CaptureBuffer& capture, const LaneParams& params,
                       const BlockContext& ctx) override
    {
        juce::ignoreUnused (capture);
        retrigSmoother.notifyRetrigger();

        // Seed from the context, never from a global. Same seed -> same slice sequence, no
        // matter the buffer size or how many times the pattern has looped.
        rngState = ctx.seed != 0 ? ctx.seed : 1u;

        const int sliceIdx = juce::jlimit (0, 3, params.getIndex (0));
        static constexpr double sliceFractions[] = { 1.0, 0.5, 0.25, 0.125 };
        sliceLen = juce::jmax (64.0, ctx.divisionLengthSamples * sliceFractions[sliceIdx]);

        const float range = juce::jlimit (0.0f, 1.0f, params.get (1));
        const float shuffleChance = juce::jlimit (0.0f, 1.0f, params.get (2));
        const float repeatChance = juce::jlimit (0.0f, 1.0f, params.get (3));
        const float reverseChance = juce::jlimit (0.0f, 1.0f, params.get (4));

        const bool shouldShuffle = nextFloat() < shuffleChance;
        const bool shouldRepeat = nextFloat() < repeatChance;
        reversed = nextFloat() < reverseChance;

        if (shouldShuffle && ! shouldRepeat)
        {
            // Reach back up to `range` of the available history, quantised to slice
            // boundaries so the result stays rhythmic rather than smeared.
            const double maxSlicesBack = juce::jmax (1.0, (double) range * 16.0);
            const int slicesBack = 1 + (int) (nextFloat() * (float) maxSlicesBack);
            sliceStartAbs = ctx.blockStartAbs - (juce::int64) (sliceLen * (double) slicesBack);
        }
        else if (! shouldRepeat)
        {
            sliceStartAbs = ctx.blockStartAbs - (juce::int64) sliceLen;
        }
        // shouldRepeat: leave sliceStartAbs where it was, which replays the previous slice.

        readPos = 0.0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh,
                        const SampleContext& ctx) override
    {
        juce::ignoreUnused (ctx);

        if (sliceLen <= 0.0)
            return;

        const double offset = reversed ? (sliceLen - readPos) : readPos;
        const double absPos = (double) sliceStartAbs + offset;

        STUTTER_CHECK_SLICE_RANGE (absPos, (double) sliceStartAbs,
                                   (double) sliceStartAbs + sliceLen);

        // Warp crossfade across the loop point, the same mechanism StutterEffect and
        // ReverseEffect use (see StutterEffect.h:92-98). Without it the slice restarts on a
        // hard splice every revolution: measured at 36 severe clicks over a one-second
        // render, because a randomly-chosen slice has no phase relationship to its own start.
        // RetriggerSmoother does not cover this -- it only spans a re-anchor, not the wrap.
        const double fadeLen = juce::jmin (sampleRate * 0.003, sliceLen * 0.1);
        const bool inFade = fadeLen > 1.0 && readPos >= sliceLen - fadeLen;

        for (int c = 0; c < numCh && c < maxChannels; ++c)
        {
            float sample = capture.readInterpolatedAbsolute (c, absPos);

            if (inFade)
            {
                const double fadeProgress = readPos - (sliceLen - fadeLen);
                const double t = juce::jlimit (0.0, 1.0, fadeProgress / fadeLen);
                // Blend into where the next revolution will begin, so the handoff is
                // continuous rather than a jump back to the slice start.
                const double nextOffset = reversed ? (sliceLen - fadeProgress) : fadeProgress;
                const float nextSample =
                    capture.readInterpolatedAbsolute (c, (double) sliceStartAbs + nextOffset);

                const float gOut = std::cos (0.5f * juce::MathConstants<float>::pi * (float) t);
                const float gIn = std::sin (0.5f * juce::MathConstants<float>::pi * (float) t);
                sample = sample * gOut + nextSample * gIn;
            }

            channelSamples[c] = sample;
        }

        retrigSmoother.process (channelSamples, numCh);

        readPos += 1.0;
        if (readPos >= sliceLen)
        {
            // Resume past the head the fade partner already played, so those samples are not
            // heard twice.
            readPos = fadeLen > 1.0 ? fadeLen : 0.0;
        }
    }

private:
    /** xorshift32. Deterministic, cheap, and self-contained -- the point is reproducibility,
        not statistical quality. */
    float nextFloat() noexcept
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return (float) (rngState & 0x00ffffffu) / (float) 0x01000000u;
    }

    static constexpr int maxChannels = 8;

    double sampleRate = 44100.0;
    int numChannels = 2;

    double readPos = 0.0;
    double sliceLen = 0.0;
    juce::int64 sliceStartAbs = 0;
    bool reversed = false;
    juce::uint32 rngState = 1u;
    RetriggerSmoother retrigSmoother;
};

} // namespace stutter
