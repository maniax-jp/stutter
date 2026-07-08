#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <cmath>
#include <memory>
#include "CaptureBuffer.h"
#include "LaneEffect.h"
#include "ParameterIDs.h"

namespace stutter
{

static constexpr int numLanes = 8;
static constexpr int numSteps = 16;

/**
    8-lane x 16-step sequencer, quantized to 16th notes using host PPQ (or an internal
    free-running clock when the host transport is stopped).

    Responsibilities:
      - Track transport position (in 16th-note steps) sample-accurately
      - Determine, per sample block, which steps are active on which lanes
      - Enforce category rules: Buffer lanes are mutually exclusive (topmost active wins),
        Texture lanes stack
      - Crossfade (~5ms) across step boundaries / lane hand-offs to avoid clicks
      - Drive each LaneEffect's onStepStart/processSample/onStepEnd lifecycle
*/
class StepSequencer
{
public:
    StepSequencer()
    {
        for (auto& lane : steps)
            lane.fill (false);
    }

    void setLaneEffect (int laneIndex, std::unique_ptr<LaneEffect> effect)
    {
        jassert (laneIndex >= 0 && laneIndex < numLanes);
        laneEffects[(size_t) laneIndex] = std::move (effect);
    }

    void prepare (double sampleRateIn, int numChannelsIn)
    {
        sampleRate = sampleRateIn;
        numChannels = numChannelsIn;
        crossfadeSamples = juce::jmax (1, (int) std::round (sampleRate * 0.005)); // 5ms

        for (auto& e : laneEffects)
            if (e != nullptr)
                e->prepare (sampleRate, numChannels);

        for (auto& s : laneState)
        {
            s.active = false;
            s.currentStep = -1;
            s.fadeCounter = 0;
            s.fadeDirection = 0;
            s.gain = 0.0f;
        }

        freeRunPpq = 0.0;
        reset();
    }

    void reset()
    {
        for (auto& e : laneEffects)
            if (e != nullptr)
                e->reset();
        for (auto& s : laneState)
        {
            s.active = false;
            s.currentStep = -1;
            s.fadeCounter = 0;
            s.gain = 0.0f;
        }
    }

    bool getStep (int lane, int step) const noexcept
    {
        if (lane < 0 || lane >= numLanes || step < 0 || step >= numSteps)
            return false;
        return steps[(size_t) lane][(size_t) step];
    }

    void setStep (int lane, int step, bool on) noexcept
    {
        if (lane < 0 || lane >= numLanes || step < 0 || step >= numSteps)
            return;
        steps[(size_t) lane][(size_t) step] = on;
    }

    void setEnabled (bool e) noexcept { sequencerEnabled = e; }
    bool isEnabled() const noexcept { return sequencerEnabled; }

    /** Current playhead step (0..15), for UI display. */
    int getCurrentPlayheadStep() const noexcept { return playheadStep.load (std::memory_order_relaxed); }

    // ---- Persistence ----
    juce::ValueTree toValueTree() const
    {
        juce::ValueTree tree (ID::sequencerNode);
        for (int l = 0; l < numLanes; ++l)
        {
            juce::ValueTree laneTree (ID::laneNode);
            laneTree.setProperty (ID::propIndex, l, nullptr);
            for (int s = 0; s < numSteps; ++s)
            {
                juce::ValueTree stepTree (ID::stepNode);
                stepTree.setProperty (ID::propIndex, s, nullptr);
                stepTree.setProperty (ID::propOn, steps[(size_t) l][(size_t) s], nullptr);
                laneTree.appendChild (stepTree, nullptr);
            }
            tree.appendChild (laneTree, nullptr);
        }
        return tree;
    }

    /** Loads the step grid from `tree`. Always starts by clearing every step to OFF first --
        if `tree` is missing/invalid, or only covers some lanes/steps (e.g. a preset saved before
        a lane existed, or one that legitimately leaves a lane fully OFF and so has no Step
        children for it), those steps end up OFF rather than retaining whatever the previously-
        loaded preset left there. This guarantees preset switches never leave step-grid residue
        from the prior state. */
    void fromValueTree (const juce::ValueTree& tree)
    {
        for (auto& lane : steps)
            lane.fill (false);

        if (! tree.isValid())
            return;

        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            auto laneTree = tree.getChild (i);
            if (! laneTree.hasType (ID::laneNode))
                continue;
            const int l = laneTree.getProperty (ID::propIndex, -1);
            if (l < 0 || l >= numLanes)
                continue;

            for (int j = 0; j < laneTree.getNumChildren(); ++j)
            {
                auto stepTree = laneTree.getChild (j);
                if (! stepTree.hasType (ID::stepNode))
                    continue;
                const int s = stepTree.getProperty (ID::propIndex, -1);
                if (s < 0 || s >= numSteps)
                    continue;
                steps[(size_t) l][(size_t) s] = (bool) stepTree.getProperty (ID::propOn, false);
            }
        }
    }

    /**
        Process one block. `ppqAtBlockStart` is the host's quarter-note position at the start
        of this block (already advanced to internal free-run clock by the caller if the host
        transport is stopped). `ppqPerSample` is how much PPQ advances per sample (derived from
        BPM and sample rate: bpm / 60 / sampleRate).

        capture = shared capture buffer to read source audio from.
        buffer  = in/out audio buffer for this block (already contains the dry/captured signal
                  that lanes should replace/modulate when active).
    */
    void processBlock (juce::AudioBuffer<float>& buffer, const CaptureBuffer& capture,
                        double ppqAtBlockStart, double ppqPerSample)
    {
        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0)
            return;

        if (! sequencerEnabled)
        {
            playheadStep.store (-1, std::memory_order_relaxed);
            return;
        }

        // capture.write() (called by the processor before this) already wrote this whole block,
        // so totalWritten reflects the position *after* the last sample of the block. Sample n
        // within this block therefore sits at (totalWritten - numSamples + n) in the
        // CaptureBuffer's absolute coordinate -- this is the fixed anchor coordinate passed to
        // lane effects so their reads no longer drift with the moving write head.
        const juce::int64 totalWrittenAfterBlock = capture.getTotalWritten();
        const juce::int64 blockStartAbs = totalWrittenAfterBlock - (juce::int64) numSamples;

        // 16th note = 0.25 quarter notes
        constexpr double stepLengthPpq = 0.25;
        constexpr double patternLengthPpq = stepLengthPpq * (double) numSteps; // 4 quarter notes = 1 bar of 16ths

        for (int n = 0; n < numSamples; ++n)
        {
            const juce::int64 nowAbs = blockStartAbs + (juce::int64) n;
            const double ppq = ppqAtBlockStart + ppqPerSample * (double) n;
            double patternPos = std::fmod (ppq, patternLengthPpq);
            if (patternPos < 0)
                patternPos += patternLengthPpq;

            // Add a tiny epsilon before the integer step-division: without it, floating-point
            // rounding can put patternPos a hair below an exact step boundary (e.g. representing
            // "step 15" as 14.999999999996), which truncates to the *previous* step and causes
            // a one-sample flicker back and forth across the boundary (e.g. 15 <-> 0 jitter).
            constexpr double stepEpsilon = 1.0e-9;
            const int stepIndex = juce::jlimit (0, numSteps - 1, (int) (patternPos / stepLengthPpq + stepEpsilon));
            const double stepPhase = juce::jlimit (0.0, 1.0,
                (patternPos - stepIndex * stepLengthPpq) / stepLengthPpq); // 0..1 within step
            const double ppqPerStep = stepLengthPpq;
            const int stepLenSamplesEstimate = ppqPerSample > 0.0
                ? (int) std::round (ppqPerStep / ppqPerSample)
                : (int) sampleRate;

            playheadStep.store (stepIndex, std::memory_order_relaxed);

            // Determine active buffer-lane (topmost priority) and active texture lanes for this sample.
            int activeBufferLane = -1;
            std::array<bool, numLanes> textureActive {};
            textureActive.fill (false);

            for (int l = 0; l < numLanes; ++l)
            {
                if (! steps[(size_t) l][(size_t) stepIndex] || laneEffects[(size_t) l] == nullptr)
                    continue;

                if (laneEffects[(size_t) l]->getCategory() == LaneCategory::Buffer)
                {
                    if (activeBufferLane < 0)
                        activeBufferLane = l;
                }
                else
                {
                    textureActive[(size_t) l] = true;
                }
            }

            float* samplePtrs[8] = {};
            const int chCount = juce::jmin (numChannels, buffer.getNumChannels());
            for (int c = 0; c < chCount; ++c)
                samplePtrs[c] = buffer.getWritePointer (c) + n;

            float working[8];
            for (int c = 0; c < chCount; ++c)
                working[c] = samplePtrs[c][0];

            // --- Buffer lane (exclusive), with crossfade on hand-off ---
            for (int l = 0; l < numLanes; ++l)
            {
                auto& st = laneState[(size_t) l];
                auto* effect = laneEffects[(size_t) l].get();
                if (effect == nullptr || effect->getCategory() != LaneCategory::Buffer)
                    continue;

                const bool shouldBeActive = (l == activeBufferLane);
                updateLaneLifecycle (st, effect, shouldBeActive, stepIndex, capture, stepLenSamplesEstimate, nowAbs);

                if (st.gain > 0.0f)
                {
                    float wet[8];
                    for (int c = 0; c < chCount; ++c)
                        wet[c] = working[c];

                    effect->processSample (capture, wet, chCount, stepPhase, nowAbs);

                    // Equal-power crossfade curve (rather than linear) so that a hand-off between
                    // two buffer lanes (one fading out while another fades in on the same sample
                    // range) sums closer to unity power throughout the transition instead of
                    // dipping in the middle -- avoids an audible gap/dulling on lane hand-off.
                    const float eqPowerGain = std::sin (st.gain * juce::MathConstants<float>::halfPi);
                    for (int c = 0; c < chCount; ++c)
                        working[c] = working[c] + eqPowerGain * (wet[c] - working[c]);
                }

                advanceFade (st, effect);
            }

            // --- Texture lanes (additive), each with its own crossfade in/out ---
            for (int l = 0; l < numLanes; ++l)
            {
                auto& st = laneState[(size_t) l];
                auto* effect = laneEffects[(size_t) l].get();
                if (effect == nullptr || effect->getCategory() != LaneCategory::Texture)
                    continue;

                const bool shouldBeActive = textureActive[(size_t) l];
                updateLaneLifecycle (st, effect, shouldBeActive, stepIndex, capture, stepLenSamplesEstimate, nowAbs);

                if (st.gain > 0.0f)
                {
                    float wet[8];
                    for (int c = 0; c < chCount; ++c)
                        wet[c] = working[c];

                    effect->processSample (capture, wet, chCount, stepPhase, nowAbs);

                    for (int c = 0; c < chCount; ++c)
                        working[c] = working[c] + st.gain * (wet[c] - working[c]);
                }

                advanceFade (st, effect);
            }

            for (int c = 0; c < chCount; ++c)
                samplePtrs[c][0] = working[c];
        }
    }

private:
    struct LaneRuntimeState
    {
        bool active = false;
        int currentStep = -1;
        float gain = 0.0f;      // 0..1 crossfade gain
        int fadeDirection = 0;  // +1 fading in, -1 fading out, 0 steady
        int fadeCounter = 0;
    };

    void updateLaneLifecycle (LaneRuntimeState& st, LaneEffect* effect, bool shouldBeActive,
                               int stepIndex, const CaptureBuffer& capture, int stepLenSamples,
                               juce::int64 nowAbs)
    {
        if (shouldBeActive && ! st.active)
        {
            // Trigger start of a new ON run.
            st.active = true;
            st.currentStep = stepIndex;
            st.fadeDirection = 1;
            effect->onStepStart (capture, stepLenSamples, nowAbs);
        }
        else if (shouldBeActive && st.active && st.currentStep != stepIndex)
        {
            // Crossing a step boundary while continuously active (same lane ON on two
            // consecutive steps). Whether this re-latches the effect's anchor/envelope depends
            // on its RetriggerPolicy:
            //  - RetriggerEachStep (e.g. Stutter): re-trigger every step, as before.
            //  - ContinueThroughRun (e.g. TapeStop/TapeStart): only re-trigger when this step
            //    does NOT immediately continue the previous one (i.e. there was a gap / this is
            //    genuinely a fresh run) -- but updateLaneLifecycle is only reached here when
            //    shouldBeActive && st.active, which by construction means the run is unbroken
            //    (a broken run would have gone through the "! shouldBeActive" fade-out branch
            //    and reset st.active to false in advanceFade). So for ContinueThroughRun lanes,
            //    a step-boundary crossing while still active means "keep going" -- do not
            //    call onStepStart again, just update the step index for stepPhase bookkeeping.
            st.currentStep = stepIndex;
            if (effect->getRetriggerPolicy() == RetriggerPolicy::RetriggerEachStep)
            {
                st.fadeDirection = 1;
                effect->onStepStart (capture, stepLenSamples, nowAbs);
            }
            else if (st.fadeDirection < 0)
            {
                // ContinueThroughRun lane re-activated while still fading out (possible when the
                // ~5ms crossfade outlasts a whole step at extreme BPM, or on live pattern
                // edits): cancel the fade-out and ramp the gain back in WITHOUT re-latching the
                // envelope/anchor via onStepStart -- otherwise the lane would silently finish
                // fading to zero and deactivate even though its step is ON.
                st.fadeDirection = 1;
            }
        }
        else if (shouldBeActive && st.active && st.fadeDirection < 0)
        {
            // Re-activated within the same step while fading out (e.g. a live pattern edit
            // toggling the step off and back on mid-step): recover the gain ramp; the effect's
            // internal state is still valid, no re-latch needed.
            st.fadeDirection = 1;
        }
        else if (! shouldBeActive && st.active && st.fadeDirection >= 0)
        {
            st.fadeDirection = -1;
        }
    }

    void advanceFade (LaneRuntimeState& st, LaneEffect* effect)
    {
        if (st.fadeDirection > 0)
        {
            st.gain += 1.0f / (float) crossfadeSamples;
            if (st.gain >= 1.0f)
            {
                st.gain = 1.0f;
                st.fadeDirection = 0;
            }
        }
        else if (st.fadeDirection < 0)
        {
            st.gain -= 1.0f / (float) crossfadeSamples;
            if (st.gain <= 0.0f)
            {
                st.gain = 0.0f;
                st.fadeDirection = 0;
                if (st.active)
                {
                    st.active = false;
                    st.currentStep = -1;
                    // Fade-out has fully completed (active -> inactive transition): let the
                    // effect know its step region has ended so it can reset any state that
                    // depends on onStepEnd (mirrors onStepStart's lifecycle contract).
                    if (effect != nullptr)
                        effect->onStepEnd();
                }
            }
        }
    }

    std::array<std::array<bool, numSteps>, numLanes> steps {};
    std::array<std::unique_ptr<LaneEffect>, numLanes> laneEffects;
    std::array<LaneRuntimeState, numLanes> laneState;

    double sampleRate = 44100.0;
    int numChannels = 2;
    int crossfadeSamples = 220; // ~5ms @44.1k
    bool sequencerEnabled = true;
    double freeRunPpq = 0.0;

    std::atomic<int> playheadStep { -1 };
};

} // namespace stutter
