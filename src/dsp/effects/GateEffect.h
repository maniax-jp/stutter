#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace stutter
{

/** Gate: trance-gate. Rate (tempo-synced subdivisions within the step), duty cycle,
    and shape (0 = square/hard, 1 = sine/smooth). Texture category: stacks with buffer lanes. */
class GateEffect : public LaneEffect
{
public:
    GateEffect (juce::AudioProcessorValueTreeState& state, int laneIdx)
        : LaneEffect (LaneCategory::Texture), apvts (state), laneIndex (laneIdx) {}

    void prepare (double sampleRateIn, int numChannelsIn) override
    {
        sampleRate = sampleRateIn;
        numChannels = numChannelsIn;
        reset();
    }

    void reset() override
    {
        phase = 0.0;
        pulsesPerStep = 4.0;
        smoothedGain = 0.0f;
        maxStepPerSample = 1.0f;
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples, juce::int64 nowAbs) override
    {
        juce::ignoreUnused (capture, nowAbs);
        const float rateParam = getParam (ID::gateRate, 4.0f); // pulses-per-step index
        duty = juce::jlimit (0.01f, 0.99f, getParam (ID::gateDuty, 0.5f));
        shape = juce::jlimit (0.0f, 1.0f, getParam (ID::gateShape, 0.0f));
        pulsesPerStep = rateToPulses ((int) rateParam);
        stepLenSamplesD = juce::jmax (1.0, (double) stepLengthSamples);
        phase = 0.0;
        // Minimum edge slew: even at shape=0 (hard square), the gate's gain can never move from
        // 0<->1 in fewer than ~1.5ms of samples. This is independent of the shape parameter's
        // "smooth window" (which shapes the sustained open-region envelope, not the edge speed
        // at shape=0) -- without it, shape=0 produces a true zero-time transition, which is an
        // instant discontinuity in the output waveform and clicks on every pulse edge.
        constexpr double minEdgeSeconds = 0.0015; // 1.5ms
        const double edgeSamples = juce::jmax (1.0, sampleRate * minEdgeSeconds);
        maxStepPerSample = (float) (1.0 / edgeSamples);
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress,
                         juce::int64 nowAbs) override
    {
        juce::ignoreUnused (capture, progress, nowAbs);

        const double pulsePhase = std::fmod (phase * pulsesPerStep, 1.0);

        float g;
        if (shape <= 0.001f)
        {
            g = (pulsePhase < duty) ? 1.0f : 0.0f;
        }
        else
        {
            // Blend hard square with a raised-cosine ("sine") window based on shape: at shape=0
            // the gate is a hard on/off square; as shape rises toward 1 the open region's edges
            // round off into a smooth half-cosine attack, giving a soft trance-gate feel.
            const float square = (pulsePhase < duty) ? 1.0f : 0.0f;
            const float smooth = pulsePhase < duty
                ? 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (pulsePhase / duty))
                : 0.0f;
            g = square + shape * (smooth - square);
        }

        // Slew-limit toward the target gain by at most maxStepPerSample per sample, guaranteeing
        // a minimum edge time regardless of `shape`. At shape>0 this is normally a no-op (the
        // raised-cosine window is already slower than the min-slew rate); at shape=0 it turns
        // the instantaneous square edge into a ~1.5ms ramp.
        if (smoothedGain < g)
            smoothedGain = juce::jmin (g, smoothedGain + maxStepPerSample);
        else if (smoothedGain > g)
            smoothedGain = juce::jmax (g, smoothedGain - maxStepPerSample);

        for (int c = 0; c < numCh; ++c)
            channelSamples[c] *= smoothedGain;

        phase += 1.0 / stepLenSamplesD;
        // phase advances by a small fixed per-sample increment, so a plain subtract is exact
        // (and cheaper than fmod) to wrap it back into [0,1).
        if (phase >= 1.0)
            phase -= 1.0;
    }

private:
    static double rateToPulses (int index)
    {
        static const double table[] = { 1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0 };
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

    double phase = 0.0;
    double pulsesPerStep = 4.0;
    double stepLenSamplesD = 11025.0;
    float duty = 0.5f;
    float shape = 0.0f;
    float smoothedGain = 0.0f;
    float maxStepPerSample = 1.0f;
};

} // namespace stutter
