#pragma once
#include "ParamIndex.h"
#include "../state/SceneSnapshot.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>

namespace stutter
{

/**
    Evaluates the scene's curves into a flat array of modulated parameter values.

    v1 had three curves hardwired to Volume, Filter and Pan. v2 lets any curve drive any
    parameter, which is Stutter Edit 2's central idea: "you can modulate every parameter of
    every effect -- and modulate them rhythmically."

    Two things about this class are performance decisions rather than style, and both are
    designed in rather than retrofitted, because the arithmetic makes the risk concrete:
    16 curves x 12 lanes x 12 params evaluated per sample at 48kHz is millions of table
    lookups a second, comfortably enough to make the plugin unusable at small buffer sizes.

      1. It iterates SceneSnapshot::activeCurves, not all maxCurves. A scene with two routed
         curves does two lookups per evaluation, not sixteen. The route list is baked by
         SceneSchema when the scene is parsed, so the audio thread never scans for it.

      2. It evaluates at control rate (every `updateInterval` samples) and interpolates in
         between. This is the same trick the global filter already uses
         (PluginProcessor.cpp:170), and it is inaudible for anything that is not an
         audio-rate modulator -- which nothing here is, since curves are tempo-synced.

    Precedence, stated explicitly because it is easy to get backwards: the scene (or the live
    overlay, or host automation) sets the BASE value, and a curve offsets from it. Never the
    reverse. Automating a knob that a curve is also driving moves the whole modulated range,
    which is what SE2 and ShaperBox both do and what users expect.
*/
class ModulationEngine
{
public:
    /** Samples between full evaluations. 16 gives ~3kHz update at 48k -- far above any
        tempo-synced curve's bandwidth, and 16x cheaper than per-sample. */
    static constexpr int updateInterval = 16;

    ModulationEngine() { reset(); }

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        reset();
    }

    void reset()
    {
        current.fill (0.0f);
        target.fill (0.0f);
        delta.fill (0.0f);
        counter = 0;
        primed = false;
    }

    /**
        Advance one sample, returning the modulated parameter array.

        Call once per sample, before the lanes are processed. The returned pointer is valid
        until the next call and is never null.
    */
    const float* nextSample (const SceneSnapshot& scene, double patternPhase) noexcept
    {
        if (counter <= 0)
        {
            evaluate (scene, patternPhase);
            counter = updateInterval;
        }
        else
        {
            for (int i = 0; i < totalParamSlots; ++i)
                current[(size_t) i] += delta[(size_t) i];
        }

        --counter;
        return current.data();
    }

    /** Read one modulated value. Bounds-checked; out of range reads as 0. */
    float get (int paramIdx) const noexcept
    {
        return isValidParamIndex (paramIdx) ? current[(size_t) paramIdx] : 0.0f;
    }

    /** True when at least one curve is routed, so callers can skip the whole path. */
    static bool hasActiveRoutes (const SceneSnapshot& scene) noexcept
    {
        return scene.numActiveCurves > 0;
    }

private:
    /**
        Recompute every parameter's target and the per-sample slope toward it.

        Two passes: seed the base values from the scene, then apply each routed curve. Doing
        it in that order is what makes the precedence rule hold -- a curve always reads a base
        that is already final.
    */
    void evaluate (const SceneSnapshot& scene, double patternPhase) noexcept
    {
        // Pass 1: base values straight from the scene.
        for (int lane = 0; lane < maxLanes; ++lane)
        {
            const auto& laneSnap = scene.lanes[(size_t) lane];
            for (int p = 0; p < maxParamsPerLane; ++p)
                target[(size_t) paramIndex (lane, p)] = laneSnap.params[(size_t) p];
        }

        // Routable globals live in the tail of the index space. They have no per-scene
        // storage (they are true APVTS globals), so a curve targeting one offsets from
        // whatever the processor last published as the base.
        target[(size_t) paramIndex (GlobalParam::dryWet)] = globalBase[0];
        target[(size_t) paramIndex (GlobalParam::outputGain)] = globalBase[1];

        // Pass 2: apply the routed curves.
        for (int i = 0; i < scene.numActiveCurves && i < maxCurves; ++i)
        {
            const int curveIdx = scene.activeCurves[(size_t) i];
            if (curveIdx < 0 || curveIdx >= maxCurves)
                continue;

            const auto& curve = scene.curves[(size_t) curveIdx];
            if (! curve.enabled)
                continue;

            const int t = curve.targetParam;
            if (! isValidParamIndex (t))
                continue;

            // Speed multiplies how many times the curve traverses per pattern. Snapped to
            // {0.25, 0.5, 1, 2, 4} at bake time so the phase cannot drift over long patterns.
            const double scaled = patternPhase * (double) curve.speedMultiplier;
            const float phase = (float) (scaled - std::floor (scaled));
            const float raw = curve.valueAtPhase (phase);

            // A curve is drawn in 0..1 -- that is what the editor shows and what the breakpoint
            // values mean -- but parameters carry natural units: cutoff in Hz, bit depth in
            // bits, repitch in semitones. Map the curve onto the parameter's own range before
            // combining, or a swept filter lands at 1Hz and goes silent.
            //
            // The skew has to come along. Filter cutoff spans 20Hz..20kHz skewed at 0.3, so
            // half-way up a curve is ~1kHz, not the ~10kHz a straight-line reading gives.
            // Mapping linearly swept the cutoff through the top of the spectrum far faster
            // than the curve describes, and at high resonance that drove the filter into
            // self-oscillation -- a blast of noise rather than a sweep.
            //
            // Globals genuinely are 0..1 linear, and the fallback says so.
            float lo = 0.0f, hi = 1.0f, skew = 1.0f;
            if (t < laneParamSlots)
                SceneSchema::getLaneRange (t / maxParamsPerLane, t % maxParamsPerLane, lo, hi, skew);

            const float span = hi - lo;

            // Same shape as juce::NormalisableRange: value = lo + span * norm^(1/skew).
            auto denormalise = [lo, span, skew] (float norm)
            {
                const float clamped = juce::jlimit (0.0f, 1.0f, norm);
                return lo + span * (juce::approximatelyEqual (skew, 1.0f)
                                        ? clamped
                                        : std::pow (clamped, 1.0f / skew));
            };

            auto normalise = [lo, span, skew] (float value)
            {
                if (span <= 0.0f)
                    return 0.0f;
                const float norm = juce::jlimit (0.0f, 1.0f, (value - lo) / span);
                return juce::approximatelyEqual (skew, 1.0f) ? norm : std::pow (norm, skew);
            };

            const float base = target[(size_t) t];

            // Blend in normalised space so depth means the same fraction of travel everywhere
            // on a skewed range, then denormalise once.
            const float baseNorm = normalise (base);
            const float targetNorm = curve.bipolar
                ? baseNorm + (raw - 0.5f) * 2.0f * curve.depth
                : baseNorm + (raw - baseNorm) * curve.depth;

            target[(size_t) t] = juce::jlimit (lo, hi, denormalise (targetNorm));
        }

        // On the first evaluation there is nothing to interpolate from, so land directly on
        // the target. Ramping up from zero instead would sweep every parameter from 0 to its
        // value over the first 16 samples -- an audible chirp on the very first block.
        if (! primed)
        {
            current = target;
            delta.fill (0.0f);
            primed = true;
            return;
        }

        constexpr float invInterval = 1.0f / (float) updateInterval;
        for (int i = 0; i < totalParamSlots; ++i)
            delta[(size_t) i] = (target[(size_t) i] - current[(size_t) i]) * invInterval;
    }

public:
    /** Base values for the routable globals, published by the processor each block. Curves
        offset from these rather than replacing them. */
    void setGlobalBase (float dryWet, float outputGain) noexcept
    {
        globalBase[0] = dryWet;
        globalBase[1] = outputGain;
    }

private:
    alignas (64) std::array<float, totalParamSlots> current {};
    alignas (64) std::array<float, totalParamSlots> target {};
    alignas (64) std::array<float, totalParamSlots> delta {};

    std::array<float, (size_t) GlobalParam::count> globalBase { { 1.0f, 0.0f } };

    double sampleRate = 44100.0;
    int counter = 0;
    bool primed = false;
};

} // namespace stutter
