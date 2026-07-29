#pragma once
#include "../LaneEffectV2.h"

namespace stutter
{

/** Gate: trance-gate. Rate (tempo-synced subdivisions within the step), duty cycle,
    and shape (0 = square/hard, 1 = sine/smooth). Texture category: stacks with buffer lanes. */
class GateEffect : public LaneEffect
{
public:
    GateEffect() : LaneEffect (LaneCategory::Texture) {}

    const char* getName() const noexcept override { return "Gate"; }

    ParamDescriptorSet getParamDescriptors() const noexcept override
    {
        static constexpr const char* rateChoices[] = {
            "1/1", "1/2", "1/3", "1/4", "1/6", "1/8", "1/12", "1/16"
        };
        static constexpr ParamDescriptor descs[] = {
            { "rate",  "Gate Rate",  0.0f, 7.0f,  3.0f, 0.0f, 1.0f, "", rateChoices, 8, true,  true },
            { "duty",  "Gate Duty",  0.01f, 0.99f, 0.5f, 0.0f, 1.0f, "", nullptr,     0, false, true },
            { "shape", "Gate Shape", 0.0f, 1.0f,  0.0f, 0.0f, 1.0f, "", nullptr,     0, false, true },
        };
        return { descs, (int) (sizeof (descs) / sizeof (descs[0])) };
    }

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

    void onBlockStart (const CaptureBuffer& capture, const LaneParams& params,
                       const BlockContext& ctx) override
    {
        juce::ignoreUnused (capture);
        duty = juce::jlimit (0.01f, 0.99f, params.get (1));
        shape = juce::jlimit (0.0f, 1.0f, params.get (2));
        pulsesPerStep = rateToPulses (params.getIndex (0));
        stepLenSamplesD = juce::jmax (1.0, ctx.divisionLengthSamples);
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

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh,
                        const SampleContext& ctx) override
    {
        juce::ignoreUnused (capture);

        // Duty and shape are declared continuous and so are read here. Rate stays latched: it
        // sets how many pulses fit in a division, and changing that mid-block would move the
        // pulse edges out from under the phase already accumulated.
        if (ctx.modulatedParams != nullptr)
        {
            duty  = juce::jlimit (0.01f, 0.99f, ctx.modulatedParams[1]);
            shape = juce::jlimit (0.0f, 1.0f, ctx.modulatedParams[2]);
        }

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
