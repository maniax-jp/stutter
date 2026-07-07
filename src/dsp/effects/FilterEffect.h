#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <cmath>

namespace stutter
{

/**
    Filter: state-variable filter (LP/BP/HP) with an LFO sweeping the cutoff.
    Texture category: stacks with buffer lanes and other texture lanes.
*/
class FilterEffect : public LaneEffect
{
public:
    FilterEffect (juce::AudioProcessorValueTreeState& state, int laneIdx)
        : LaneEffect (LaneCategory::Texture), apvts (state), laneIndex (laneIdx) {}

    void prepare (double sampleRateIn, int numChannelsIn) override
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jmin (numChannelsIn, maxChannels);
        reset();
    }

    void reset() override
    {
        for (auto& s : svf)
        {
            s.low = 0.0f;
            s.band = 0.0f;
        }
        lfoPhase = 0.0;
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples) override
    {
        juce::ignoreUnused (capture);
        filterType = (int) getParam (ID::filterType, 0.0f);
        cutoffHz = getParam (ID::filterCutoff, 1000.0f);
        resonance = juce::jlimit (0.0f, 0.99f, getParam (ID::filterResonance, 0.2f));
        lfoRateParam = getParam (ID::filterLfoRate, 4.0f);
        lfoDepth = juce::jlimit (0.0f, 1.0f, getParam (ID::filterLfoDepth, 0.0f));
        stepLenSamplesD = juce::jmax (1.0, (double) stepLengthSamples);
        lfoCyclesPerStep = rateToCycles ((int) lfoRateParam);
        lfoPhase = 0.0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress) override
    {
        juce::ignoreUnused (capture, progress);

        const float lfoValue = lfoDepth > 0.0f
            ? std::sin (juce::MathConstants<float>::twoPi * (float) std::fmod (lfoPhase * lfoCyclesPerStep, 1.0))
            : 0.0f;

        // Map cutoff (with LFO modulation) into normalized SVF coefficient. Clamp strictly below
        // sampleRate * 0.4 (rather than 0.45) to keep the "2*sin(pi*f/sr)" coefficient well away
        // from the point where the SVF becomes unstable/self-oscillating at high resonance.
        const float modOctaves = lfoValue * lfoDepth * 3.0f; // +/-3 octaves sweep at full depth
        const float modulatedCutoff = juce::jlimit (20.0f, (float) sampleRate * 0.4f,
                                                      cutoffHz * std::pow (2.0f, modOctaves));

        const float f = 2.0f * std::sin (juce::MathConstants<float>::pi * modulatedCutoff / (float) sampleRate);
        // q = 1 - resonance would reach 0 at resonance=1.0 (undamped -> runaway feedback).
        // Clamp resonance to 0.95 equivalent so q never drops below a small positive floor.
        const float q = 1.0f - juce::jmin (resonance, 0.95f);

        for (int c = 0; c < numCh && c < maxChannels; ++c)
        {
            auto& s = svf[(size_t) c];
            const float input = channelSamples[c];

            s.low += f * s.band;
            const float high = input - s.low - q * s.band;
            s.band += f * high;

            float out;
            switch (filterType)
            {
                case 1: out = s.band; break;    // BP
                case 2: out = high; break;      // HP
                default: out = s.low; break;    // LP
            }

            channelSamples[c] = out;
        }

        lfoPhase += 1.0 / stepLenSamplesD;
        // Phase advances by a small fixed increment per sample, so it can only ever exceed 1.0
        // by less than one increment: a plain subtract is exact and avoids fmod's per-sample
        // division cost (and, for edge-case negative/NaN inputs, its less predictable behaviour).
        if (lfoPhase >= 1.0)
            lfoPhase -= 1.0;
    }

private:
    static double rateToCycles (int index)
    {
        static const double table[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 };
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

    struct SvfState
    {
        float low = 0.0f;
        float band = 0.0f;
    };

    static constexpr int maxChannels = 8;

    juce::AudioProcessorValueTreeState& apvts;
    int laneIndex;
    double sampleRate = 44100.0;
    int numChannels = 2;

    std::array<SvfState, maxChannels> svf {};
    int filterType = 0;
    float cutoffHz = 1000.0f;
    float resonance = 0.2f;
    float lfoRateParam = 4.0f;
    float lfoDepth = 0.0f;
    double lfoPhase = 0.0;
    double lfoCyclesPerStep = 1.0;
    double stepLenSamplesD = 11025.0;
};

} // namespace stutter
