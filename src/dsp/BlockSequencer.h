#pragma once
#include "CaptureBuffer.h"
#include "LaneEffectV2.h"
#include "ParamDescriptor.h"
#include "ParamIndex.h"
#include "ModulationEngine.h"
#include "../state/SceneSnapshot.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

namespace stutter
{

/**
    Variable-length block sequencer. Replaces StepSequencer's fixed 16-cell grid.

    Why blocks rather than cells: v1 could only say "this lane is on during this step". An
    effect with a directional envelope -- TapeStop decelerating, a rate ramp -- has no way to
    know how long it is being held, so it either restarts every step (never reaching its
    target) or runs open-ended. A block carries a start and a length, so "slow to a stop over
    three divisions" is expressible.

    What is deliberately preserved from StepSequencer:
      - the per-sample loop. It is what makes sample-accurate block edges and swing cheap;
        splitting the buffer at block boundaries instead would complicate both.
      - the epsilon guard before the integer division (StepSequencer.h:214). Floating-point
        rounding can put a position a hair below an exact boundary and flicker between two
        divisions on consecutive samples. Swing makes boundaries land on non-representable
        positions far more often, so this matters MORE here, not less.
      - the 5ms equal-power (sin taper) crossfade and the whole lane lifecycle, including the
        subtle re-activation cases documented inline below.

    Blocks are assumed sorted by startDiv and non-overlapping within a lane. SceneSchema
    establishes that invariant when it parses; this class relies on it to advance a
    forward-only cursor instead of searching.
*/
class BlockSequencer
{
public:
    BlockSequencer() = default;

    void prepare (double newSampleRate, int newNumChannels)
    {
        sampleRate = newSampleRate;
        numChannels = newNumChannels;
        crossfadeSamples = juce::jmax (1, (int) (sampleRate * 0.005)); // ~5ms

        for (auto& e : laneEffects)
            if (e != nullptr)
                e->prepare (sampleRate, numChannels);

        reset();
    }

    void reset()
    {
        for (auto& e : laneEffects)
            if (e != nullptr)
                e->reset();

        for (auto& st : laneState)
            st = LaneRuntimeState {};

        playheadDivision.store (-1, std::memory_order_relaxed);
        playheadPhase.store (0.0f, std::memory_order_relaxed);
    }

    void setLaneEffect (int lane, std::unique_ptr<LaneEffect> effect)
    {
        if (lane < 0 || lane >= maxLanes)
            return;
        laneEffects[(size_t) lane] = std::move (effect);
        if (laneEffects[(size_t) lane] != nullptr)
            laneEffects[(size_t) lane]->prepare (sampleRate, numChannels);
    }

    LaneEffect* getLaneEffect (int lane) const noexcept
    {
        if (lane < 0 || lane >= maxLanes)
            return nullptr;
        return laneEffects[(size_t) lane].get();
    }

    void setEnabled (bool e) noexcept { sequencerEnabled = e; }
    bool isEnabled() const noexcept { return sequencerEnabled; }

    /** Current division index for the UI playhead, or -1 when idle. */
    int getPlayheadDivision() const noexcept { return playheadDivision.load (std::memory_order_relaxed); }

    /** Fractional position through the pattern (0..1) for a sub-division playhead. */
    float getPlayheadPhase() const noexcept { return playheadPhase.load (std::memory_order_relaxed); }

    /**
        Process one block against a scene snapshot.

        `scene` must outlive the call; the caller (the processor) holds it via SceneStore for
        the duration of processBlock and never across a block boundary, which is what lets
        SceneStore retire banks on a timer rather than refcount them on the audio thread.
    */
    void processBlock (juce::AudioBuffer<float>& buffer, const CaptureBuffer& capture,
                       const SceneSnapshot& scene, double ppqAtBlockStart, double ppqPerSample,
                       ModulationEngine* modulation = nullptr)
    {
        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0)
            return;

        if (! sequencerEnabled)
        {
            playheadDivision.store (-1, std::memory_order_relaxed);
            return;
        }

        // capture.write() (called by the processor before this) already wrote this whole
        // block, so totalWritten reflects the position after its last sample. Sample n sits
        // at (totalWritten - numSamples + n) in absolute coordinates -- the fixed anchor
        // handed to effects so their reads do not drift with the moving write head.
        const juce::int64 blockStartAbs = capture.getTotalWritten() - (juce::int64) numSamples;

        const int totalDivs = juce::jlimit (1, maxLanes * maxBlocksPerLane, scene.totalDivisions());
        // A division is one subdivision of a beat, and a beat is a quarter note.
        const double ppqPerDivision = 1.0 / (double) juce::jmax (1, scene.divisions);
        const double patternLengthPpq = (double) juce::jmax (1, scene.beats);

        // Rounded to a whole sample, matching StepSequencer.h:219. Effects scale this value
        // (TapeStop/TapeStart multiply it by 0.25..3.0 to get an envelope duration), so a
        // fractional value here would be amplified into a shift in where the envelope lands.
        // At the tempo/rate the equivalence test uses, the division happens to be an exact
        // 6000 samples, so this rounding is a no-op there -- it matters at rates where it is
        // not, and matching v1's rounding keeps the two definitions from quietly drifting.
        const double divisionLenSamples = ppqPerSample > 0.0
            ? std::round (ppqPerDivision / ppqPerSample)
            : sampleRate;

        // Bar fraction of one division, so effects can convert a note value into samples
        // without knowing the tempo. A bar is 4 quarter notes in 4/4.
        const double divisionBarFraction = ppqPerDivision / 4.0;

        // Solo is evaluated once per block, not per sample: it cannot change mid-block.
        bool anySolo = false;
        for (int l = 0; l < maxLanes; ++l)
            if (scene.lanes[(size_t) l].solo)
            {
                anySolo = true;
                break;
            }

        const int chCount = juce::jmin (numChannels, buffer.getNumChannels());

        for (int n = 0; n < numSamples; ++n)
        {
            const juce::int64 nowAbs = blockStartAbs + (juce::int64) n;
            const double ppq = ppqAtBlockStart + ppqPerSample * (double) n;

            double patternPos = std::fmod (ppq, patternLengthPpq);
            if (patternPos < 0.0)
                patternPos += patternLengthPpq;

            // Position in divisions, before swing.
            const double rawDivPos = patternPos / ppqPerDivision;

            // Swing shifts odd division boundaries. Applying it to the boundary positions
            // rather than warping the time axis keeps every effect's internal time base
            // linear and uniform -- StutterEffect accumulates readPos += playbackRate, and a
            // warped clock would make that drift.
            const double divPos = applySwing (rawDivPos, scene.swing);

            // Epsilon before the truncation, for the reason documented at the top of this
            // file. Carried over from StepSequencer.h:214.
            constexpr double divisionEpsilon = 1.0e-9;
            const int divIndex = juce::jlimit (0, totalDivs - 1, (int) (divPos + divisionEpsilon));
            const double divPhase = juce::jlimit (0.0, 1.0, divPos - (double) divIndex);
            const double patternPhase = patternLengthPpq > 0.0 ? patternPos / patternLengthPpq : 0.0;

            playheadDivision.store (divIndex, std::memory_order_relaxed);
            playheadPhase.store ((float) patternPhase, std::memory_order_relaxed);

            // Advance the modulation matrix once per sample, before any lane reads it, so
            // every lane in this sample sees the same modulated values.
            const float* modulated = modulation != nullptr
                ? modulation->nextSample (scene, patternPhase)
                : nullptr;


            // --- Which lanes are active on this sample, and under which block ---
            int activeBufferLane = -1;
            std::array<const Block*, maxLanes> activeBlock {};
            activeBlock.fill (nullptr);

            for (int l = 0; l < maxLanes; ++l)
            {
                auto* effect = laneEffects[(size_t) l].get();
                if (effect == nullptr)
                    continue;

                const auto& laneSnap = scene.lanes[(size_t) l];
                if (! laneSnap.enabled || laneSnap.mute)
                    continue;
                if (anySolo && ! laneSnap.solo)
                    continue;

                const Block* blk = findBlock (laneSnap, divIndex);
                if (blk == nullptr)
                    continue;

                activeBlock[(size_t) l] = blk;

                if (effect->getCategory() == LaneCategory::Buffer)
                {
                    // Exclusive by lane order, matching v1's "topmost active lane wins".
                    if (activeBufferLane < 0)
                        activeBufferLane = l;
                }
            }

            float* samplePtrs[8] = {};
            for (int c = 0; c < chCount; ++c)
                samplePtrs[c] = buffer.getWritePointer (c) + n;

            float working[8];
            for (int c = 0; c < chCount; ++c)
                working[c] = samplePtrs[c][0];

            // --- Buffer lanes (exclusive) ---
            for (int l = 0; l < maxLanes; ++l)
            {
                auto* effect = laneEffects[(size_t) l].get();
                if (effect == nullptr || effect->getCategory() != LaneCategory::Buffer)
                    continue;

                processLane (l, effect, scene, capture, working, chCount,
                             l == activeBufferLane ? activeBlock[(size_t) l] : nullptr,
                             divIndex, divPhase, patternPhase, nowAbs,
                             divisionLenSamples, divisionBarFraction, ppqPerSample, modulated);
            }

            // --- Texture lanes (additive, in chain order) ---
            for (int slot = 0; slot < maxLanes; ++slot)
            {
                const int l = textureOrder[(size_t) slot];
                if (l < 0)
                    continue;

                auto* effect = laneEffects[(size_t) l].get();
                if (effect == nullptr || effect->getCategory() != LaneCategory::Texture)
                    continue;

                processLane (l, effect, scene, capture, working, chCount,
                             activeBlock[(size_t) l], divIndex, divPhase, patternPhase,
                             nowAbs, divisionLenSamples, divisionBarFraction, ppqPerSample,
                             modulated);
            }

            for (int c = 0; c < chCount; ++c)
                samplePtrs[c][0] = working[c];
        }
    }

    /** Recompute the Texture chain order from a scene. Message thread; call after publishing
        a bank. Sorting here rather than per sample keeps the audio path free of it. */
    void updateChainOrder (const SceneSnapshot& scene)
    {
        std::array<int, maxLanes> order {};
        int count = 0;
        for (int l = 0; l < maxLanes; ++l)
        {
            auto* effect = laneEffects[(size_t) l].get();
            if (effect != nullptr && effect->getCategory() == LaneCategory::Texture)
                order[(size_t) count++] = l;
        }

        // Insertion sort by chainPosition, then by lane index for stability. Small n, and it
        // keeps lanes with equal chainPosition in a predictable order.
        for (int i = 1; i < count; ++i)
        {
            const int key = order[(size_t) i];
            const int keyPos = scene.lanes[(size_t) key].chainPosition;
            int j = i - 1;
            while (j >= 0
                   && (scene.lanes[(size_t) order[(size_t) j]].chainPosition > keyPos
                       || (scene.lanes[(size_t) order[(size_t) j]].chainPosition == keyPos
                           && order[(size_t) j] > key)))
            {
                order[(size_t) j + 1] = order[(size_t) j];
                --j;
            }
            order[(size_t) j + 1] = key;
        }

        for (int i = count; i < maxLanes; ++i)
            order[(size_t) i] = -1;

        textureOrder = order;
    }

private:
    struct LaneRuntimeState
    {
        bool active = false;
        int currentDivision = -1;
        const Block* currentBlock = nullptr;
        float gain = 0.0f;
        int fadeDirection = 0;   // +1 in, -1 out, 0 steady
    };

    /** Find the block covering `divIndex`, or nullptr. Linear over a lane's blocks, which are
        sorted and capped at maxBlocksPerLane; a cursor would be faster but would need
        invalidating on every transport jump, and this is already trivial at these sizes. */
    static const Block* findBlock (const LaneSnapshot& lane, int divIndex) noexcept
    {
        for (int i = 0; i < lane.numBlocks && i < maxBlocksPerLane; ++i)
        {
            const auto& b = lane.blocks[(size_t) i];
            if (divIndex < b.startDiv)
                break;                    // sorted: no later block can match either
            if (divIndex < b.endDiv())
                return &b;
        }
        return nullptr;
    }

    /** Map a linear division position to its swung position.

        Odd-numbered division boundaries shift by up to half a division at full swing. Even
        boundaries stay put, so the pattern's downbeats never move. */
    static double applySwing (double divPos, float swing) noexcept
    {
        if (std::abs (swing) < 1.0e-6f)
            return divPos;

        // Swing displaces ODD boundaries only: boundary i sits at i + (i odd ? shift : 0).
        // Every even boundary -- including the pattern's end -- stays exactly where it was,
        // so a pair of divisions still spans exactly two and the pattern length is unchanged.
        //
        // That invariant is worth stating because the obvious implementation (rescaling each
        // division independently) silently breaks it: measured, it left the first boundary
        // fixed and shortened the whole pattern from 2.000 to 1.500 divisions at full swing,
        // which is a tempo change wearing a groove costume. Operating per PAIR rather than
        // per division is what pins the ends.
        const double shift = (double) swing * 0.5;

        const int pair = (int) std::floor (divPos * 0.5);
        const double inPair = divPos - (double) (pair * 2);

        const double firstLen = 1.0 + shift;    // on-beat division, lengthened
        const double secondLen = 1.0 - shift;   // off-beat division, shortened

        double out;
        if (inPair < firstLen)
            out = firstLen > 1.0e-9 ? inPair / firstLen : inPair;
        else
            out = 1.0 + (secondLen > 1.0e-9 ? (inPair - firstLen) / secondLen : 0.0);

        return (double) (pair * 2) + out;
    }

    void processLane (int lane, LaneEffect* effect, const SceneSnapshot& scene,
                      const CaptureBuffer& capture, float* working, int chCount,
                      const Block* block, int divIndex, double divPhase, double patternPhase,
                      juce::int64 nowAbs, double divisionLenSamples, double divisionBarFraction,
                      double ppqPerSample, const float* modulated)
    {
        auto& st = laneState[(size_t) lane];

        updateLaneLifecycle (st, effect, lane, block, divIndex, capture, scene,
                             divisionLenSamples, divisionBarFraction, nowAbs, ppqPerSample,
                             modulated);

        if (st.gain > 0.0f)
        {
            float wet[8];
            for (int c = 0; c < chCount; ++c)
                wet[c] = working[c];

            SampleContext sctx;
            sctx.nowAbs = nowAbs;
            sctx.blockProgress = blockProgress (st, divIndex, divPhase);
            sctx.patternPhase = patternPhase;
            sctx.modulatedParams = modulated != nullptr
                ? modulated + paramIndex (lane, 0)
                : laneParams[(size_t) lane].params.data();
            sctx.reverseDirection = false;
            sctx.freeze = false;
            effect->processSample (capture, wet, chCount, sctx);

            // Equal-power (sin) taper rather than a linear gain blend, so a hand-off between
            // two lanes sums closer to unity power through the transition instead of dipping.
            // Same curve for Buffer and Texture -- see StepSequencer.h:287 and :326.
            const float eqPowerGain = std::sin (st.gain * juce::MathConstants<float>::halfPi);
            const float laneMix = scene.lanes[(size_t) lane].mix;
            for (int c = 0; c < chCount; ++c)
                working[c] = working[c] + eqPowerGain * laneMix * (wet[c] - working[c]);
        }

        advanceFade (st, effect);
    }

    /** 0..1 through the whole block, not just the current division. An effect holding across
        four divisions sees a single ramp, which is the point of variable-length blocks. */
    static double blockProgress (const LaneRuntimeState& st, int divIndex, double divPhase) noexcept
    {
        if (st.currentBlock == nullptr || st.currentBlock->lengthDiv <= 0)
            return divPhase;

        const double elapsed = (double) (divIndex - st.currentBlock->startDiv) + divPhase;
        return juce::jlimit (0.0, 1.0, elapsed / (double) st.currentBlock->lengthDiv);
    }

    /** Latch this lane's parameters for a trigger.

        Reads the MODULATED values when they are available, not the raw scene values, so a
        curve driving a latched parameter (Stutter's rate, Reverse's slice length) is sampled
        at the trigger instant. That is the whole point of the latched/continuous split: a
        latched parameter still responds to modulation, it just does so once per trigger
        rather than per sample, which is what keeps a loop length from tearing mid-loop. */
    void fillLaneParams (int lane, LaneEffect* effect, const SceneSnapshot& scene,
                         const float* modulated)
    {
        const auto set = effect->getParamDescriptors();
        auto& out = laneParams[(size_t) lane];
        const auto& snap = scene.lanes[(size_t) lane];
        for (int i = 0; i < set.count && i < maxParamsPerLane; ++i)
            out.set (i, modulated != nullptr ? modulated[paramIndex (lane, i)]
                                             : snap.params[(size_t) i]);
    }

    void triggerBlockStart (LaneRuntimeState& st, LaneEffect* effect, int lane,
                            const Block* block, int divIndex, const CaptureBuffer& capture,
                            const SceneSnapshot& scene, double divisionLenSamples,
                            double divisionBarFraction, juce::int64 nowAbs,
                            double ppqPerSample, bool isRetrigger, const float* modulated)
    {
        st.currentDivision = divIndex;
        st.currentBlock = block;
        fillLaneParams (lane, effect, scene, modulated);

        BlockContext ctx;
        ctx.blockStartAbs = nowAbs;
        ctx.blockLengthSamples = block != nullptr
            ? divisionLenSamples * (double) block->lengthDiv
            : divisionLenSamples;
        ctx.divisionLengthSamples = divisionLenSamples;
        ctx.divisionBarFraction = divisionBarFraction;
        ctx.ppqPerSample = ppqPerSample;
        ctx.sampleRate = sampleRate;
        ctx.seed = deriveSeed (scene.seed, (uint32_t) triggerCount[(size_t) lane]++,
                               block != nullptr ? (int) block->seedOffset : 0);
        ctx.isRetrigger = isRetrigger;
        ctx.tier = block != nullptr ? block->tier : 0;

        effect->onBlockStart (capture, laneParams[(size_t) lane], ctx);
    }

    void updateLaneLifecycle (LaneRuntimeState& st, LaneEffect* effect, int lane,
                              const Block* block, int divIndex, const CaptureBuffer& capture,
                              const SceneSnapshot& scene, double divisionLenSamples,
                              double divisionBarFraction, juce::int64 nowAbs, double ppqPerSample,
                              const float* modulated)
    {
        const bool shouldBeActive = block != nullptr;

        if (shouldBeActive && ! st.active)
        {
            st.active = true;
            st.fadeDirection = 1;
            triggerBlockStart (st, effect, lane, block, divIndex, capture, scene,
                               divisionLenSamples, divisionBarFraction, nowAbs, ppqPerSample, false, modulated);
        }
        else if (shouldBeActive && st.active && st.currentBlock != block)
        {
            // Moved into a different block while still active.
            //
            // Whether that is a fresh trigger depends on adjacency, not just on identity.
            // Two blocks that touch (previous end == next start) form one unbroken run, and
            // v1 treated exactly that case -- consecutive ON steps -- as "keep going" for
            // ContinueThroughRun lanes. Re-latching here instead would restart TapeStop's
            // deceleration at every division, so it would never reach a stop: precisely the
            // bug ContinueThroughRun exists to prevent (StepSequencer.h:407-436).
            //
            // A gap between the blocks IS a fresh run, because the lane went silent in
            // between, so it re-triggers for every policy.
            // Adjacency has to account for the pattern wrap: at the loop point the previous
            // block ends at totalDivisions() while the next starts at 0, so a plain
            // endDiv() == startDiv comparison reports "not adjacent" and re-triggers. v1 had
            // no such seam -- its run simply continued across the wrap -- and without this
            // the divergence shows up precisely at the first loop boundary.
            const int totalDivs = scene.totalDivisions();
            const bool adjacent = st.currentBlock != nullptr
                               && block != nullptr
                               && (st.currentBlock->endDiv() == block->startDiv
                                   || (st.currentBlock->endDiv() >= totalDivs && block->startDiv == 0));
            const bool continuesRun = adjacent
                && effect->getRetriggerPolicy() == RetriggerPolicy::ContinueThroughRun;

            st.currentDivision = divIndex;

            if (continuesRun)
            {
                // Adopt the new block for progress bookkeeping without re-latching the
                // effect's anchor or envelope.
                st.currentBlock = block;
                if (st.fadeDirection < 0)
                    st.fadeDirection = 1;
            }
            else
            {
                st.fadeDirection = 1;
                triggerBlockStart (st, effect, lane, block, divIndex, capture, scene,
                                   divisionLenSamples, divisionBarFraction, nowAbs, ppqPerSample, true, modulated);
            }
        }
        else if (shouldBeActive && st.active && st.currentDivision != divIndex)
        {
            // Crossed a division boundary inside the SAME block. Whether this re-latches
            // depends on the effect's policy:
            //  - RetriggerEachDivision: re-latch (Shuffler re-picks its slice, Stretcher
            //    re-seeds) so a long block stays alive instead of freezing on one choice.
            //  - RetriggerEachBlock / ContinueThroughRun: keep running. For a block-scoped
            //    effect the block IS the unit, and for a directional envelope re-latching
            //    would mean never reaching the target -- which is the bug ContinueThroughRun
            //    exists to prevent (see StepSequencer.h:407-436).
            st.currentDivision = divIndex;
            if (effect->getRetriggerPolicy() == RetriggerPolicy::RetriggerEachDivision)
            {
                st.fadeDirection = 1;
                triggerBlockStart (st, effect, lane, block, divIndex, capture, scene,
                                   divisionLenSamples, divisionBarFraction, nowAbs, ppqPerSample, true, modulated);
            }
            else if (st.fadeDirection < 0)
            {
                // Re-activated while still fading out (possible when the ~5ms crossfade
                // outlasts a division at extreme BPM, or on a live pattern edit): cancel the
                // fade-out and ramp back in WITHOUT re-latching, or the lane would finish
                // fading to silence and deactivate even though its block is still active.
                st.fadeDirection = 1;
            }
        }
        else if (shouldBeActive && st.active && st.fadeDirection < 0)
        {
            // Re-activated within the same division while fading out (e.g. a live edit
            // toggling the block off and back on): recover the ramp; the effect's internal
            // state is still valid, so no re-latch.
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
                    st.currentDivision = -1;
                    st.currentBlock = nullptr;
                    if (effect != nullptr)
                        effect->onBlockEnd();
                }
            }
        }
    }

    std::array<std::unique_ptr<LaneEffect>, maxLanes> laneEffects;
    std::array<LaneRuntimeState, maxLanes> laneState;
    std::array<LaneParams, maxLanes> laneParams {};
    std::array<int, maxLanes> triggerCount {};

    /** Texture lanes in chain order; -1 terminates. Rebuilt by updateChainOrder(). */
    std::array<int, maxLanes> textureOrder { { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } };

    double sampleRate = 44100.0;
    int numChannels = 2;
    int crossfadeSamples = 220; // ~5ms @44.1k
    bool sequencerEnabled = true;

    std::atomic<int> playheadDivision { -1 };
    std::atomic<float> playheadPhase { 0.0f };
};

} // namespace stutter
