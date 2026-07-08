#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace stutter
{

/**
    Repitch: varispeed pitch up/down (-24..+24 semitones) while looping the captured
    slice, with an optional slide from 0 semitones to the target over the step region.
*/
class RepitchEffect : public LaneEffect
{
public:
    RepitchEffect (juce::AudioProcessorValueTreeState& state, int laneIdx)
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
        loopLenSamples = sampleRate * 0.25;
        elapsedSamples = 0.0;
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
        targetSemitones = getParam (ID::repitchSemitones, 0.0f);
        slideAmount = getParam (ID::repitchSlide, 0.0f); // 0..1: 0 = instant, 1 = slides across whole step
        loopLenSamples = juce::jmax (256.0, (double) stepLengthSamples);
        stepLenSamplesD = (double) stepLengthSamples;
        readPosSamples = 0.0;
        elapsedSamples = 0.0;
        // Fixed anchor: the looped slice is loopLenSamples immediately before "now", frozen for
        // the lifetime of this trigger.
        anchorAbs = nowAbs;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress,
                         juce::int64 nowAbs) override
    {
        juce::ignoreUnused (progress, nowAbs);

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
    double loopLenSamples = 11025.0;
    double stepLenSamplesD = 11025.0;
    double elapsedSamples = 0.0;
    float targetSemitones = 0.0f;
    float slideAmount = 0.0f;
    juce::int64 anchorAbs = 0;
    RetriggerSmoother retrigSmoother;
};

} // namespace stutter
