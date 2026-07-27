#pragma once
#include "../LaneEffectV2.h"
#include <array>

namespace stutter
{

/**
    Stretcher: granular time-stretch, from Glitch 2's module of the same name.

    Overlapping Hann-windowed grains are read from the capture buffer at a playback rate
    independent of how fast the grain stream advances, which is what lets the audio slow down
    without dropping in pitch. Short grains sound metallic, long ones smear -- that trade is
    the parameter, not a defect.

    Grains live in a fixed-capacity pool. No allocation on the audio thread, and a hard bound
    on per-sample cost regardless of grain size or density.

    Jitter randomises grain size and start position, seeded from BlockContext like Shuffler,
    so the same seed reproduces the same texture. Glitch 2's note about jitter masking the
    metallic resonance of short grains is accurate and worth having.
*/
class StretcherEffect : public LaneEffect
{
public:
    StretcherEffect() : LaneEffect (LaneCategory::Buffer) {}

    const char* getName() const noexcept override { return "Stretcher"; }

    RetriggerPolicy getRetriggerPolicy() const noexcept override
    {
        return RetriggerPolicy::RetriggerEachDivision;
    }

    ParamDescriptorSet getParamDescriptors() const noexcept override
    {
        static constexpr ParamDescriptor descs[] = {
            { "speed",  "Stretch Speed",  0.05f, 1.0f,  0.5f,  0.0f, 1.0f, "",   nullptr, 0, true,  true },
            { "grain",  "Grain Size",     0.01f, 0.3f,  0.08f, 0.0f, 1.0f, "s",  nullptr, 0, true,  true },
            { "jitter", "Grain Jitter",   0.0f,  1.0f,  0.2f,  0.0f, 1.0f, "",   nullptr, 0, true,  true },
            { "pitch",  "Stretch Pitch",  -12.0f, 12.0f, 0.0f, 0.0f, 1.0f, "st", nullptr, 0, true,  true },
        };
        return { descs, (int) (sizeof (descs) / sizeof (descs[0])) };
    }

    void prepare (double sampleRateIn, int numChannelsIn) override
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jmin (numChannelsIn, maxChannels);
        retrigSmoother.prepare (sampleRate);
        reset();
    }

    void reset() override
    {
        for (auto& g : grains)
            g = Grain {};
        nextGrainCountdown = 0.0;
        sourcePos = 0.0;
        rngState = 1u;
        retrigSmoother.reset();
    }

    void onBlockEnd() override { retrigSmoother.reset(); }

    void onBlockStart (const CaptureBuffer& capture, const LaneParams& params,
                       const BlockContext& ctx) override
    {
        juce::ignoreUnused (capture);
        retrigSmoother.notifyRetrigger();

        rngState = ctx.seed != 0 ? ctx.seed : 1u;

        speed = juce::jlimit (0.05f, 1.0f, params.get (0));
        grainSeconds = juce::jlimit (0.01f, 0.3f, params.get (1));
        jitter = juce::jlimit (0.0f, 1.0f, params.get (2));
        pitchRate = std::pow (2.0f, juce::jlimit (-12.0f, 12.0f, params.get (3)) / 12.0f);

        anchorAbs = ctx.blockStartAbs;
        sourcePos = 0.0;
        nextGrainCountdown = 0.0;

        for (auto& g : grains)
            g = Grain {};
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh,
                        const SampleContext& ctx) override
    {
        juce::ignoreUnused (ctx);

        const double grainLen = juce::jmax (64.0, (double) grainSeconds * sampleRate);

        // Spawn at half the grain length so grains overlap 2x. Below that the windows leave
        // gaps and the output gates; far above it the overlap smears into mush.
        if (nextGrainCountdown <= 0.0)
        {
            spawnGrain (grainLen);
            nextGrainCountdown = grainLen * 0.5;
        }
        nextGrainCountdown -= 1.0;

        float sum[maxChannels] = {};
        for (auto& g : grains)
        {
            if (! g.active)
                continue;

            const double t = g.position / g.length;
            if (t >= 1.0)
            {
                g.active = false;
                continue;
            }

            // Hann window. Grains fade in and out rather than starting abruptly, which is
            // what keeps overlapping reads from clicking at every boundary.
            const float window = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                                                          * (float) t));

            const double absPos = g.startAbs + g.position * (double) pitchRate;
            for (int c = 0; c < numCh && c < maxChannels; ++c)
                sum[c] += capture.readInterpolatedAbsolute (c, absPos) * window;

            g.position += 1.0;
        }

        for (int c = 0; c < numCh && c < maxChannels; ++c)
            channelSamples[c] = sum[c];

        retrigSmoother.process (channelSamples, numCh);

        // The source advances more slowly than realtime, which is the stretch.
        sourcePos += (double) speed;
    }

private:
    struct Grain
    {
        bool active = false;
        double position = 0.0;
        double length = 1.0;
        double startAbs = 0.0;
    };

    void spawnGrain (double grainLen) noexcept
    {
        for (auto& g : grains)
        {
            if (g.active)
                continue;

            const double sizeJitter = 1.0 + ((double) nextFloat() - 0.5) * (double) jitter;
            const double posJitter = ((double) nextFloat() - 0.5) * (double) jitter * grainLen * 2.0;

            g.active = true;
            g.position = 0.0;
            g.length = juce::jmax (32.0, grainLen * sizeJitter);
            // Read behind the anchor by however far the (slowed) source has advanced.
            g.startAbs = (double) anchorAbs - grainLen + sourcePos + posJitter;
            return;
        }
        // Pool exhausted: drop the grain rather than allocate. At 2x overlap the pool cannot
        // fill under any legal parameter combination, so this is a guard, not a policy.
    }

    float nextFloat() noexcept
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return (float) (rngState & 0x00ffffffu) / (float) 0x01000000u;
    }

    static constexpr int maxChannels = 8;
    static constexpr int maxGrains = 32;

    double sampleRate = 44100.0;
    int numChannels = 2;

    std::array<Grain, maxGrains> grains {};
    double nextGrainCountdown = 0.0;
    double sourcePos = 0.0;
    juce::int64 anchorAbs = 0;

    float speed = 0.5f;
    float grainSeconds = 0.08f;
    float jitter = 0.2f;
    float pitchRate = 1.0f;
    juce::uint32 rngState = 1u;
    RetriggerSmoother retrigSmoother;
};

} // namespace stutter
