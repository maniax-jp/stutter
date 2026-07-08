#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

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
        readOffsetSamples = 0.0;
        elapsedSamples = 0.0;
        durationSamples = sampleRate * 0.5;
        curveAmount = 0.0f;
        anchorAbs = 0;
        stopped = false;
        for (auto& v : heldSample)
            v = 0.0f;
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples, juce::int64 nowAbs) override
    {
        juce::ignoreUnused (capture);
        curveAmount = getParam (ID::tapeStopCurve, 0.5f);
        const float timeParam = getParam (ID::tapeStopTime, 0.5f); // 0..1 -> scales decel duration
        // Duration ranges from ~0.25x to ~3x the step length
        const double scale = 0.25 + timeParam * 2.75;
        durationSamples = juce::jmax (256.0, stepLengthSamples * scale);
        elapsedSamples = 0.0;
        readOffsetSamples = 0.0;
        // Anchor "now" once, at the start of the (possibly multi-step) continuous ON run --
        // see getRetriggerPolicy(). The read head then advances forward from this anchor at
        // the decelerating speed, i.e. it falls further and further behind "now" as speed drops
        // toward 0, which is what makes it sound like tape slowing down rather than a jump cut.
        anchorAbs = nowAbs;
        stopped = false;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress,
                         juce::int64 nowAbs) override
    {
        juce::ignoreUnused (progress, nowAbs);

        // Once the decel curve has fully reached speed 0, the read position stops advancing
        // forever (ContinueThroughRun lanes may stay active far longer than this buffer's ~2.5s
        // history window if the pattern keeps this step on continuously). Rather than keep
        // asking CaptureBuffer for a position that eventually falls outside valid history (which
        // it can only clamp to the *nearest* valid index -- and that nearest index keeps moving
        // forward as the write head advances, silently turning "frozen" into "drifting" and
        // producing periodic jumps as the clamped read crosses the source material), latch the
        // last real sample once and hold it: a stopped tape stays stopped.
        if (stopped)
        {
            for (int c = 0; c < numCh; ++c)
                channelSamples[c] = heldSample[(size_t) juce::jmin (c, maxChannels - 1)];
            return;
        }

        const double t = juce::jlimit (0.0, 1.0, elapsedSamples / durationSamples);
        const double speed = speedForT (t);

        // readOffsetSamples accumulates the integral of speed (1.0 -> 0.0), i.e. how far the
        // read head has fallen behind "now" so far; subtract it from the anchor to move backward
        // in time as the effect decelerates (mirrors the original readInterpolated(samplesAgo)
        // semantics, where samplesAgo grew over time).
        const double absPos = (double) anchorAbs - readOffsetSamples;
        for (int c = 0; c < numCh; ++c)
        {
            const float s = capture.readInterpolatedAbsolute (c, absPos);
            channelSamples[c] = s;
            if (c < maxChannels)
                heldSample[(size_t) c] = s;
        }

        readOffsetSamples += speed;
        elapsedSamples += 1.0;

        if (t >= 1.0)
            stopped = true;
    }

    // TapeStop's deceleration is a single directional envelope across the whole ON region: if a
    // pattern holds this lane on for several consecutive 16ths, re-triggering on every step
    // boundary would restart the decel from full speed each time and it would never actually
    // reach a stop within a busy pattern. Continue the same envelope/anchor through an unbroken
    // run and only re-latch when a fresh (non-contiguous) run begins.
    RetriggerPolicy getRetriggerPolicy() const noexcept override { return RetriggerPolicy::ContinueThroughRun; }

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

    static constexpr int maxChannels = 8;

    juce::AudioProcessorValueTreeState& apvts;
    int laneIndex;
    double sampleRate = 44100.0;
    int numChannels = 2;

    double readOffsetSamples = 0.0;
    double elapsedSamples = 0.0;
    double durationSamples = 22050.0;
    float curveAmount = 0.5f;
    juce::int64 anchorAbs = 0;
    bool stopped = false;
    std::array<float, maxChannels> heldSample {};
};

} // namespace stutter
