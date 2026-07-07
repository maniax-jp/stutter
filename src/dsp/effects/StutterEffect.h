#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace stutter
{

/**
    Stutter: repeats a short slice of the capture buffer in a loop, with the loop
    optionally shrinking on each repeat (Stutter-Edit style gradient) and an optional
    pitch slide across repeats (achieved via changing playback rate over time).
*/
class StutterEffect : public LaneEffect
{
public:
    StutterEffect (juce::AudioProcessorValueTreeState& state, int laneIdx)
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
        loopLenSamples = 0.0;
        repeatCount = 0;
        currentRateSemitoneOffset = 0.0f;
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples) override
    {
        juce::ignoreUnused (capture);

        const float rateParam = getParam (ID::stutterRate, 4.0f);   // divisions index, see rateToFraction
        const float decayParam = getParam (ID::stutterDecay, 0.0f); // 0..1: how much loop shrinks per repeat
        pitchSlideSemis = getParam (ID::stutterPitchSlide, 0.0f);

        const double fraction = rateToFraction ((int) rateParam);
        baseLoopLenSamples = juce::jmax (32.0, stepLengthSamples * fraction * 4.0); // stepLength = 1/16, scale to musical fraction of a bar
        loopLenSamples = baseLoopLenSamples;
        decayAmount = decayParam;
        readPosSamples = 0.0;
        repeatCount = 0;
        currentRateSemitoneOffset = 0.0f;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress) override
    {
        juce::ignoreUnused (progress);

        // Pitch slide: ramps semitone offset from 0 to pitchSlideSemis across repeats (capped at ~8 repeats span)
        const float slideProgress = juce::jlimit (0.0f, 1.0f, (float) repeatCount / 8.0f);
        currentRateSemitoneOffset = pitchSlideSemis * slideProgress;
        const double playbackRate = std::pow (2.0, currentRateSemitoneOffset / 12.0);

        for (int c = 0; c < numCh; ++c)
            channelSamples[c] = capture.readInterpolated (c, loopLenSamples - readPosSamples);

        readPosSamples += playbackRate;

        if (readPosSamples >= loopLenSamples)
        {
            readPosSamples = 0.0;
            ++repeatCount;
            // Loop-length decay: shrink loop by decayAmount each repeat (gradient toward shorter stutters)
            if (decayAmount > 0.0f)
            {
                const double shrink = 1.0 - (0.35 * decayAmount * (1.0 / (1.0 + repeatCount * 0.5)));
                loopLenSamples = juce::jmax (32.0, loopLenSamples * shrink);
            }
        }
    }

private:
    static double rateToFraction (int index)
    {
        // index -> fraction of a bar; includes straight, triplet, dotted variants
        static const double table[] = {
            1.0 / 4.0,          // 0: 1/4
            1.0 / 8.0,          // 1: 1/8
            1.0 / 16.0,         // 2: 1/16
            1.0 / 32.0,         // 3: 1/32
            1.0 / 64.0,         // 4: 1/64
            (1.0 / 4.0) * (2.0 / 3.0),  // 5: 1/4 triplet
            (1.0 / 8.0) * (2.0 / 3.0),  // 6: 1/8 triplet
            (1.0 / 16.0) * (2.0 / 3.0), // 7: 1/16 triplet
            (1.0 / 4.0) * 1.5,  // 8: 1/4 dotted
            (1.0 / 8.0) * 1.5,  // 9: 1/8 dotted
            (1.0 / 16.0) * 1.5, // 10: 1/16 dotted
        };
        constexpr int n = (int) (sizeof (table) / sizeof (double));
        index = juce::jlimit (0, n - 1, index);
        return table[index];
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
    double loopLenSamples = 0.0;
    double baseLoopLenSamples = 0.0;
    float decayAmount = 0.0f;
    int repeatCount = 0;
    float pitchSlideSemis = 0.0f;
    float currentRateSemitoneOffset = 0.0f;
};

} // namespace stutter
