#pragma once
#include "../LaneEffectV2.h"
#include "../TimingMode.h"

namespace stutter
{

/** Reverse: plays the most recent slice of the capture buffer backwards. */
class ReverseEffect : public LaneEffect
{
public:
    ReverseEffect() : LaneEffect (LaneCategory::Buffer) {}

    const char* getName() const noexcept override { return "Reverse"; }

    ParamDescriptorSet getParamDescriptors() const noexcept override
    {
        static constexpr const char* rateChoices[] = {
            "1/4", "1/8", "1/16", "1/32", "1/64",
            "1/4T", "1/8T", "1/16T",
            "1/4.", "1/8.", "1/16."
        };
        static constexpr ParamDescriptor descs[] = {
            { "sliceLen", "Reverse Slice Length", 0.0f, 10.0f, 2.0f, 0.0f, 1.0f, "", rateChoices, 11, true, true },
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
        sliceLenSamples = sampleRate * 0.25;
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

        // The rate table is in fractions of a quarter note (v1 labelled them as bar fractions
        // but hardcoded `* 4.0` against a 16th-note step -- preserving that labelling bug);
        // quarterBarFraction is therefore 1/4 of a bar, and a division under the v1 grid is
        // 1/16 of a bar, hence the 4:1 ratio that v1 hardcoded as `* 4.0`.
        constexpr double quarterBarFraction = 0.25;
        const double fraction = legacyRateIndexToFraction (params.getIndex (0));
        sliceLenSamples = juce::jmax (32.0,
            fraction / quarterBarFraction * ctx.divisionLengthSamples);
        readPosSamples = 0.0;
        // Fixed anchor: the reversed slice is the sliceLenSamples immediately before "now",
        // frozen for the lifetime of this trigger (see CaptureBuffer::readInterpolatedAbsolute).
        anchorAbs = ctx.blockStartAbs;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh,
                        const SampleContext& ctx) override
    {
        juce::ignoreUnused (ctx);

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

    double sampleRate = 44100.0;
    int numChannels = 2;

    double readPosSamples = 0.0;
    double sliceLenSamples = 11025.0;
    juce::int64 anchorAbs = 0;
    RetriggerSmoother retrigSmoother;
};

} // namespace stutter
