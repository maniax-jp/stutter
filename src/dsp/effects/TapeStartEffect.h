#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

namespace stutter
{

/**
    TapeStart: playback speed spins up from 0 to 1 following s0(t)+B·sin(πt)
    (0 → peak ~1.35 → 1), over a duration scaled by step length and `time`.
    Position fraction G(t) integrates that speed with G(1)=1 by construction,
    so at t=1 absPos lands exactly on nowAbs and speed is continuous at 1
    (no handoff glitch). For ContinueThroughRun long holds, t≥1 keeps gap=0
    and tracks real-time via live nowAbs.
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

        // absPos is a position curve of t vs live nowAbs each sample (not a fixed onStepStart
        // anchor). G(t) = I0(t) + B·(1-cos(πt))/π integrates the spin-up speed
        // s0(t)+B·sin(πt) with G(0)=0 and G(1)=1 identically, so gapT = D·(t-G(t)) is 0 at
        // both ends and absPos lands exactly on nowAbs at t=1; speed is then 1, so the handoff
        // to real-time is continuous. Live nowAbs also means a ContinueThroughRun lane held
        // past t=1 keeps gap=0 and tracks real-time within CaptureBuffer's history window.
        const double gapT = durationSamples * (t - positionFractionForT (t));
        const double absPos = (double) nowAbs - gapT;
        for (int c = 0; c < numCh; ++c)
            channelSamples[c] = capture.readInterpolatedAbsolute (c, absPos);

        elapsedSamples += 1.0;
    }

    // TapeStart's spin-up is a single directional envelope across the whole ON region, same
    // reasoning as TapeStop: continue an unbroken run rather than restarting the ramp every 16th.
    RetriggerPolicy getRetriggerPolicy() const noexcept override { return RetriggerPolicy::ContinueThroughRun; }

private:
    // Integrated position fraction G(t) for spin-up: G(0)=0, G(1)=1.
    // s0(t) = jmap(c, t, 1-(1-t)^e); I0 = ∫s0; A0=I0(1); B=(1-A0)·π/2;
    // G(t) = I0(t) + B·(1-cos(πt))/π  ⇒  G'(t) = s0(t)+B·sin(πt) (speed 0→~1.35→1).
    double positionFractionForT (double t) const
    {
        const double c = (double) curveAmount;
        const double e = 1.0 + c * 4.0;
        const double pi = juce::MathConstants<double>::pi;

        const double i0Linear = t * t * 0.5;
        const double i0Exp = t - (1.0 - std::pow (1.0 - t, e + 1.0)) / (e + 1.0);
        const double i0 = juce::jmap (c, 0.0, 1.0, i0Linear, i0Exp);

        const double a0 = juce::jmap (c, 0.0, 1.0, 0.5, e / (e + 1.0));
        const double B = (1.0 - a0) * pi * 0.5;

        return i0 + B * (1.0 - std::cos (pi * t)) / pi;
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
