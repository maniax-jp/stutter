#pragma once
#include "../LaneEffectV2.h"
#include <array>

namespace stutter
{

/**
    Distortion: four waveshaping characters, from Glitch 2's module of the same name.

    The four modes are chosen because they fail differently rather than because they sound
    similar at different intensities: Razor hard-clips, Shape soft-clips, Fold reflects peaks
    back on themselves (adding inharmonic content a clipper cannot), and Rectify folds the
    negative half up (an octave-ish effect). Texture category, so it stacks.

    No oversampling. Every mode aliases at high drive, which is inherent to the glitch idiom
    -- Crush already aliases by design and nobody has asked it not to. Worth stating so the
    omission reads as a choice rather than an oversight; adding 4x oversampling later is a
    contained change if it turns out to matter.
*/
class DistortionEffect : public LaneEffect
{
public:
    DistortionEffect() : LaneEffect (LaneCategory::Texture) {}

    const char* getName() const noexcept override { return "Distort"; }

    ParamDescriptorSet getParamDescriptors() const noexcept override
    {
        static constexpr const char* modeChoices[] = { "Razor", "Shape", "Fold", "Rectify" };
        static constexpr ParamDescriptor descs[] = {
            { "mode",  "Distort Mode",  0.0f, 3.0f,  0.0f, 1.0f, 1.0f, "", modeChoices, 4, true,  true },
            { "drive", "Distort Drive", 1.0f, 24.0f, 4.0f, 0.0f, 1.0f, "", nullptr,     0, false, true },
            { "tone",  "Distort Tone",  -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, "", nullptr,     0, false, true },
            { "mix",   "Distort Mix",   0.0f, 1.0f,  1.0f, 0.0f, 1.0f, "", nullptr,     0, false, true },
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
        for (auto& v : toneState)
            v = 0.0f;
    }

    void onBlockStart (const CaptureBuffer& capture, const LaneParams& params,
                       const BlockContext& ctx) override
    {
        juce::ignoreUnused (capture, ctx);
        mode = juce::jlimit (0, 3, params.getIndex (0));
        drive = juce::jlimit (1.0f, 24.0f, params.get (1));
        tone = juce::jlimit (-1.0f, 1.0f, params.get (2));
        mix = juce::jlimit (0.0f, 1.0f, params.get (3));
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh,
                        const SampleContext& ctx) override
    {
        juce::ignoreUnused (capture);

        // Continuous parameters re-read per sample so a curve can sweep them; the descriptor
        // table marks drive/tone/mix as latchAtBlockStart = false for exactly this.
        if (ctx.modulatedParams != nullptr)
        {
            drive = juce::jlimit (1.0f, 24.0f, ctx.modulatedParams[1]);
            tone = juce::jlimit (-1.0f, 1.0f, ctx.modulatedParams[2]);
            mix = juce::jlimit (0.0f, 1.0f, ctx.modulatedParams[3]);
        }

        // Gain compensation keeps the level roughly constant as drive rises, so sweeping
        // drive reads as a change of character rather than of volume.
        const float comp = 1.0f / std::sqrt (drive);

        for (int c = 0; c < numCh && c < maxChannels; ++c)
        {
            const float dry = channelSamples[c];
            const float x = dry * drive;
            float y = 0.0f;

            switch (mode)
            {
                case 0: // Razor -- hard clip
                    y = juce::jlimit (-1.0f, 1.0f, x);
                    break;

                case 1: // Shape -- soft clip
                    y = std::tanh (x);
                    break;

                case 2: // Fold -- reflect peaks back, generating inharmonic content
                {
                    float f = x;
                    // Bounded rather than while(): at drive 24 an unbounded fold would iterate
                    // many times per sample on peaks, which is unpredictable cost on the audio
                    // thread. Eight reflections is far past the point of audible difference.
                    for (int i = 0; i < 8 && (f > 1.0f || f < -1.0f); ++i)
                        f = f > 1.0f ? 2.0f - f : -2.0f - f;
                    y = juce::jlimit (-1.0f, 1.0f, f);
                    break;
                }

                case 3: // Rectify -- fold the negative half up
                default:
                    y = std::tanh (std::abs (x)) * 2.0f - 1.0f;
                    break;
            }

            y *= comp;

            // One-pole tilt: negative tone lowpasses, positive highpasses. Cheap, and enough
            // to tame or brighten the harmonics the shaper just generated.
            const float coeff = 0.05f + 0.4f * std::abs (tone);
            toneState[(size_t) c] += coeff * (y - toneState[(size_t) c]);
            const float shaped = tone < 0.0f ? toneState[(size_t) c]
                               : (tone > 0.0f ? y - toneState[(size_t) c] : y);

            channelSamples[c] = dry + mix * (shaped - dry);
        }
    }

private:
    static constexpr int maxChannels = 8;

    double sampleRate = 44100.0;
    int numChannels = 2;

    int mode = 0;
    float drive = 4.0f;
    float tone = 0.0f;
    float mix = 1.0f;
    std::array<float, maxChannels> toneState {};
};

} // namespace stutter
