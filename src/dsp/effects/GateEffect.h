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
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples) override
    {
        juce::ignoreUnused (capture);
        const float rateParam = getParam (ID::gateRate, 4.0f); // pulses-per-step index
        duty = juce::jlimit (0.01f, 0.99f, getParam (ID::gateDuty, 0.5f));
        shape = juce::jlimit (0.0f, 1.0f, getParam (ID::gateShape, 0.0f));
        pulsesPerStep = rateToPulses ((int) rateParam);
        stepLenSamplesD = juce::jmax (1.0, (double) stepLengthSamples);
        phase = 0.0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress) override
    {
        juce::ignoreUnused (capture, progress);

        const double pulsePhase = std::fmod (phase * pulsesPerStep, 1.0);

        float g;
        if (shape <= 0.001f)
        {
            g = (pulsePhase < duty) ? 1.0f : 0.0f;
        }
        else
        {
            // Blend hard square with smooth sine window based on shape
            const float square = (pulsePhase < duty) ? 1.0f : 0.0f;
            const float sineWin = 0.5f * (1.0f + std::sin (juce::MathConstants<float>::twoPi * ((float) pulsePhase - duty * 0.5f) - juce::MathConstants<float>::halfPi));
            const float smooth = pulsePhase < duty
                ? 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (pulsePhase / duty))
                : 0.0f;
            juce::ignoreUnused (sineWin);
            g = square + shape * (smooth - square);
        }

        for (int c = 0; c < numCh; ++c)
            channelSamples[c] *= g;

        phase += 1.0 / stepLenSamplesD;
        if (phase >= 1.0)
            phase = std::fmod (phase, 1.0);
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
};

} // namespace stutter
