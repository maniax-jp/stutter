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
        elapsedSamples = 0.0;
        durationSamples = sampleRate * 0.5;
        curveAmount = 0.0f;
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples, juce::int64 nowAbs) override
    {
        juce::ignoreUnused (capture, nowAbs);
        curveAmount = getParam (ID::tapeStartCurve, 0.5f);
        const float timeParam = getParam (ID::tapeStartTime, 0.5f);
        const double scale = 0.25 + timeParam * 2.75;
        durationSamples = juce::jmax (256.0, stepLengthSamples * scale);
        elapsedSamples = 0.0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress,
                         juce::int64 nowAbs) override
    {
        juce::ignoreUnused (progress);

        const double t = juce::jlimit (0.0, 1.0, elapsedSamples / durationSamples);

        // absPos is defined directly as a position curve of t, referenced against the *current*
        // nowAbs every sample (not a fixed anchor latched once in onStepStart): gapT(t) is the
        // "samples behind now" gap, shrinking from durationSamples at t=0 to exactly 0 at t=1 by
        // construction, so playback is guaranteed to land exactly on nowAbs when the spin-up
        // completes -- no discontinuity at the handoff to real-time tracking. (Integrating a
        // speed curve instead, as a moving read offset accumulated sample-by-sample, would
        // under-shoot durationSamples for any spin-up curve whose average speed is below 1.0 --
        // true of every curve shape here -- causing a jump right when playback should already be
        // back in sync.) Referencing live nowAbs (rather than a fixed onStepStart-time anchor)
        // also means a ContinueThroughRun lane held on far longer than this buffer's ~2.5s
        // history window naturally keeps tracking real-time audio instead of drifting into
        // out-of-range territory that CaptureBuffer's clamp would have to silently reinterpret.
        const double gapT = durationSamples * (1.0 - gapShapeForT (t));
        const double absPos = (double) nowAbs - gapT;
        for (int c = 0; c < numCh; ++c)
            channelSamples[c] = capture.readInterpolatedAbsolute (c, absPos);

        elapsedSamples += 1.0;
    }

    // TapeStart's spin-up is a single directional envelope across the whole ON region, same
    // reasoning as TapeStop: continue an unbroken run rather than restarting the ramp every 16th.
    RetriggerPolicy getRetriggerPolicy() const noexcept override { return RetriggerPolicy::ContinueThroughRun; }

private:
    // t: 0..1 progress through spin-up. Returns the fraction of the "now" gap already closed
    // (0 = still fully durationSamples behind, 1 = fully caught up) -- same curve shape the
    // original implementation used for instantaneous speed, reused here as a position fraction.
    double gapShapeForT (double t) const
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

    double elapsedSamples = 0.0;
    double durationSamples = 22050.0;
    float curveAmount = 0.5f;
};

} // namespace stutter
