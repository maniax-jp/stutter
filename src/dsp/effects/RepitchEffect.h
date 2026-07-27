#pragma once
#include "../LaneEffectV2.h"

namespace stutter
{

/**
    Repitch: varispeed pitch up/down (-24..+24 semitones) while looping the captured
    slice, with an optional slide from 0 semitones to the target over the step region.
*/
class RepitchEffect : public LaneEffect
{
public:
    RepitchEffect() : LaneEffect (LaneCategory::Buffer) {}

    const char* getName() const noexcept override { return "Repitch"; }

    ParamDescriptorSet getParamDescriptors() const noexcept override
    {
        static constexpr ParamDescriptor descs[] = {
            { "semitones", "Repitch Semitones", -24.0f, 24.0f, -12.0f, 0.0f, 1.0f, "st", nullptr, 0, true, true },
            { "slide",     "Repitch Slide",       0.0f,  1.0f,   0.0f, 0.0f, 1.0f, "",   nullptr, 0, true, true },
        };
        return { descs, (int) (sizeof (descs) / sizeof (descs[0])) };
    }

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
        loopLenSamples = sampleRate * 0.25;
        elapsedSamples = 0.0;
        anchorAbs = 0;
        retrigSmoother.reset();
    }

    void onBlockEnd() override
    {
        retrigSmoother.reset();
    }

    void onBlockStart (const CaptureBuffer& capture, const LaneParams& params,
                       const BlockContext& ctx) override
    {
        juce::ignoreUnused (capture);
        // Blend from the previous output into the newly-anchored slice on a per-block retrigger
        // (the outer sequencer's gain crossfade can't mask this: gain is already 1.0).
        retrigSmoother.notifyRetrigger();
        targetSemitones = params.get (0);
        slideAmount = params.get (1); // 0..1: 0 = instant, 1 = slides across whole step
        loopLenSamples = juce::jmax (256.0, ctx.divisionLengthSamples);
        stepLenSamplesD = ctx.divisionLengthSamples;
        readPosSamples = 0.0;
        elapsedSamples = 0.0;
        // Fixed anchor: the looped slice is loopLenSamples immediately before "now", frozen for
        // the lifetime of this trigger.
        anchorAbs = ctx.blockStartAbs;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh,
                        const SampleContext& ctx) override
    {
        juce::ignoreUnused (ctx);

        float currentSemitones = targetSemitones;
        if (slideAmount > 0.0f)
        {
            const double slideDuration = juce::jmax (1.0, stepLenSamplesD * slideAmount);
            const double slideT = juce::jlimit (0.0, 1.0, elapsedSamples / slideDuration);
            currentSemitones = (float) (targetSemitones * slideT);
        }

        const double rate = std::pow (2.0, currentSemitones / 12.0);

        // Slice spans absolute positions [anchorAbs - loopLen, anchorAbs]; readPos advancing
        // 0 -> loopLen at `rate` plays it forward at the varispeed rate, looping back (with a
        // warp crossfade) when it reaches the end.
        const double fadeLen = loopCrossfadeSamples();
        const double sliceStart = (double) anchorAbs - loopLenSamples;
        const double absPos = sliceStart + readPosSamples;

        // Warp crossfade into the next revolution across the loop wrap point: during the last
        // fadeLen samples, the outgoing tail is equal-power blended with the next revolution's
        // opening samples, starting exactly at the slice start. After the wrap, readPosSamples
        // resumes at fadeLen (below) so the head samples the fade partner already played are not
        // played twice -- the blend hands off seamlessly. Both read positions stay inside
        // [anchorAbs - loopLenSamples, anchorAbs] by construction: the main read because
        // readPos in [0, loopLen), the partner because it spans [sliceStart, sliceStart + fadeLen)
        // with fadeLen <= 0.1 * loopLen.
        const bool inFade = readPosSamples >= loopLenSamples - fadeLen;
        double wrapAbsPos = 0.0;
        float gOut = 1.0f, gIn = 0.0f;
        if (inFade)
        {
            const double fadeProgress = readPosSamples - (loopLenSamples - fadeLen);
            const double fadeT = juce::jlimit (0.0, 1.0, fadeLen > 0.0 ? fadeProgress / fadeLen : 1.0);
            wrapAbsPos = sliceStart + fadeProgress;
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

        readPosSamples += rate; // rate = 2^(semis/12) is always > 0, so readPos only moves forward
        if (readPosSamples >= loopLenSamples)
        {
            // Resume past the head samples the fade partner already played (see fade comment).
            readPosSamples = fadeLen + (readPosSamples - loopLenSamples);
        }

        elapsedSamples += 1.0;
    }

private:
    double loopCrossfadeSamples() const noexcept
    {
        const double msSamples = sampleRate * 0.003; // 3ms
        return juce::jmin (msSamples, loopLenSamples * 0.1);
    }

    double sampleRate = 44100.0;
    int numChannels = 2;

    double readPosSamples = 0.0;
    double loopLenSamples = 11025.0;
    double stepLenSamplesD = 11025.0;
    double elapsedSamples = 0.0;
    float targetSemitones = 0.0f;
    float slideAmount = 0.0f;
    juce::int64 anchorAbs = 0;
    RetriggerSmoother retrigSmoother;
};

} // namespace stutter
