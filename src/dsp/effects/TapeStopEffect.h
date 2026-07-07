#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace stutter
{

/**
    TapeStop: playback speed ramps from 1.0 down to 0.0 following a curve
    (linear <-> exponential, controlled by `curve` param), over a duration
    scaled by the step length and the `time` param.
*/
class TapeStopEffect : public LaneEffect
{
public:
    TapeStopEffect (juce::AudioProcessorValueTreeState& state, int laneIdx)
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
        elapsedSamples = 0.0;
        durationSamples = sampleRate * 0.5;
        curveAmount = 0.0f;
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples) override
    {
        juce::ignoreUnused (capture);
        curveAmount = getParam (ID::tapeStopCurve, 0.5f);
        const float timeParam = getParam (ID::tapeStopTime, 0.5f); // 0..1 -> scales decel duration
        // Duration ranges from ~0.25x to ~3x the step length
        const double scale = 0.25 + timeParam * 2.75;
        durationSamples = juce::jmax (256.0, stepLengthSamples * scale);
        elapsedSamples = 0.0;
        readPosSamples = 0.0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress) override
    {
        juce::ignoreUnused (progress);

        const double t = juce::jlimit (0.0, 1.0, elapsedSamples / durationSamples);
        const double speed = speedForT (t);

        for (int c = 0; c < numCh; ++c)
            channelSamples[c] = capture.readInterpolated (c, readPosSamples);

        readPosSamples += speed;
        elapsedSamples += 1.0;
    }

private:
    // t: 0..1 progress through the decel. Returns instantaneous playback speed 1.0 -> 0.0.
    double speedForT (double t) const
    {
        // curveAmount 0 = linear decel, 1 = exponential (fast then long tail)
        const double linear = 1.0 - t;
        const double exponent = 1.0 + curveAmount * 4.0; // 1..5
        const double exp = std::pow (1.0 - t, exponent);
        return juce::jmap ((double) curveAmount, 0.0, 1.0, linear, exp);
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
    double elapsedSamples = 0.0;
    double durationSamples = 22050.0;
    float curveAmount = 0.5f;
};

} // namespace stutter
