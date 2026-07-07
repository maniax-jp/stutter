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
        reset();
    }

    void reset() override
    {
        readPosSamples = 0.0;
        loopLenSamples = sampleRate * 0.25;
        elapsedSamples = 0.0;
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples) override
    {
        juce::ignoreUnused (capture);
        targetSemitones = getParam (ID::repitchSemitones, 0.0f);
        slideAmount = getParam (ID::repitchSlide, 0.0f); // 0..1: 0 = instant, 1 = slides across whole step
        loopLenSamples = juce::jmax (256.0, (double) stepLengthSamples);
        stepLenSamplesD = (double) stepLengthSamples;
        readPosSamples = 0.0;
        elapsedSamples = 0.0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress) override
    {
        juce::ignoreUnused (progress);

        float currentSemitones = targetSemitones;
        if (slideAmount > 0.0f)
        {
            const double slideDuration = juce::jmax (1.0, stepLenSamplesD * slideAmount);
            const double slideT = juce::jlimit (0.0, 1.0, elapsedSamples / slideDuration);
            currentSemitones = (float) (targetSemitones * slideT);
        }

        const double rate = std::pow (2.0, currentSemitones / 12.0);

        for (int c = 0; c < numCh; ++c)
            channelSamples[c] = capture.readInterpolated (c, loopLenSamples - readPosSamples);

        readPosSamples += rate;
        if (readPosSamples >= loopLenSamples || readPosSamples < 0.0)
            readPosSamples = std::fmod (readPosSamples, loopLenSamples);
        if (readPosSamples < 0.0)
            readPosSamples += loopLenSamples;

        elapsedSamples += 1.0;
    }

private:
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
};

} // namespace stutter
