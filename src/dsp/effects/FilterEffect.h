#pragma once
#include "../LaneEffectV2.h"
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
    FilterEffect() : LaneEffect (LaneCategory::Texture) {}

    const char* getName() const noexcept override { return "Filter"; }

    ParamDescriptorSet getParamDescriptors() const noexcept override
    {
        static constexpr const char* typeChoices[] = { "Low Pass", "Band Pass", "High Pass" };
        static constexpr const char* lfoRateChoices[] = { "1/4", "1/2", "1/1", "2/1", "4/1", "8/1" };
        static constexpr ParamDescriptor descs[] = {
            { "type",      "Filter Type",      0.0f,     2.0f,     0.0f,    0.0f, 1.0f, "",   typeChoices,    3, true,  true },
            { "cutoff",    "Filter Cutoff",    20.0f, 20000.0f,  1000.0f,   1.0f, 0.3f, "Hz", nullptr,        0, false, true },
            { "resonance", "Filter Resonance", 0.0f,     0.99f,    0.2f,    0.0f, 1.0f, "",   nullptr,        0, false, true },
            { "lfoRate",   "Filter LFO Rate",  0.0f,     5.0f,     2.0f,    0.0f, 1.0f, "",   lfoRateChoices, 6, true,  true },
            { "lfoDepth",  "Filter LFO Depth", 0.0f,     1.0f,     0.0f,    0.0f, 1.0f, "",   nullptr,        0, false, true },
        };
        return { descs, (int) (sizeof (descs) / sizeof (descs[0])) };
    }

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

    void onBlockStart (const CaptureBuffer& capture, const LaneParams& params,
                       const BlockContext& ctx) override
    {
        juce::ignoreUnused (capture);
        filterType = params.getIndex (0);
        cutoffHz = params.get (1);
        resonance = juce::jlimit (0.0f, 0.99f, params.get (2));
        lfoRateParam = params.get (3);
        lfoDepth = juce::jlimit (0.0f, 1.0f, params.get (4));
        stepLenSamplesD = juce::jmax (1.0, ctx.divisionLengthSamples);
        lfoCyclesPerStep = rateToCycles (params.getIndex (3));
        lfoPhase = 0.0;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh,
                        const SampleContext& ctx) override
    {
        juce::ignoreUnused (capture);

        // Cutoff, resonance and LFO depth are declared continuous, so they have to be read
        // here rather than latched at block start. Reading them only in onBlockStart made them
        // stick for as long as a block lasted -- and a preset whose lane is on for all sixteen
        // steps is a single block, so during playback they never refreshed at all: changing
        // preset left the old values sounding until something re-triggered the lane.
        // Type and LFO rate stay latched; both define structure that must not shift mid-block.
        if (ctx.modulatedParams != nullptr)
        {
            cutoffHz  = ctx.modulatedParams[1];
            resonance = juce::jlimit (0.0f, 0.99f, ctx.modulatedParams[2]);
            lfoDepth  = juce::jlimit (0.0f, 1.0f, ctx.modulatedParams[4]);
        }

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

    struct SvfState
    {
        float low = 0.0f;
        float band = 0.0f;
    };

    static constexpr int maxChannels = 8;

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
