#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace stutter
{

/**
    TapeStart: inverse of TapeStop — playback speed spins up from 0.0 to 1.0
    following a curve, over a duration scaled by step length and `time` param.
    Reads forward from the point in the capture buffer that corresponds to
    "now" minus the step length, so that by the time it reaches full speed it
    lands back in sync with the present.
*/
class TapeStartEffect : public LaneEffect
{
public:
    TapeStartEffect (juce::AudioProcessorValueTreeState& state, int laneIdx)
        : LaneEffect (LaneCategory::Buffer), apvts (state), laneIndex (laneIdx) {}

    void prepare (double sampleRateIn, int numChannelsIn) override
    {
        sampleRate = sampleRateIn;
        numChannels = numChannelsIn;
        reset();
    }

    void reset() override
    {
        samplesAgoAtStart = sampleRate * 0.5;
        readOffsetSamples = 0.0;
        elapsedSamples = 0.0;
        durationSamples = sampleRate * 0.5;
        curveAmount = 0.0f;
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples) override
    {
        juce::ignoreUnused (capture);
        curveAmount = getParam (ID::tapeStartCurve, 0.5f);
        const float timeParam = getParam (ID::tapeStartTime, 0.5f);
        const double scale = 0.25 + timeParam * 2.75;
        durationSamples = juce::jmax (256.0, stepLengthSamples * scale);

        // Start reading from "durationSamples ago" so that ramping forward at speed
        // (which reduces samplesAgo over time) reaches "now" (samplesAgo = stepLengthSamples)
        // roughly as the step region ends.
        samplesAgoAtStart = durationSamples;
        readOffsetSamples = 0.0;
        elapsedSamples = 0.0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress) override
    {
        juce::ignoreUnused (progress);

        const double t = juce::jlimit (0.0, 1.0, elapsedSamples / durationSamples);
        const double speed = speedForT (t);

        const double samplesAgo = juce::jmax (0.0, samplesAgoAtStart - readOffsetSamples);
        for (int c = 0; c < numCh; ++c)
            channelSamples[c] = capture.readInterpolated (c, samplesAgo);

        readOffsetSamples += speed;
        elapsedSamples += 1.0;
    }

private:
    // t: 0..1 progress through spin-up. Returns instantaneous playback speed 0.0 -> 1.0.
    double speedForT (double t) const
    {
        const double linear = t;
        const double exponent = 1.0 + curveAmount * 4.0;
        const double exp = 1.0 - std::pow (1.0 - t, exponent);
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

    double samplesAgoAtStart = 22050.0;
    double readOffsetSamples = 0.0;
    double elapsedSamples = 0.0;
    double durationSamples = 22050.0;
    float curveAmount = 0.5f;
};

} // namespace stutter
