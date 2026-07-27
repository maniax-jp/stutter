#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include "CaptureBuffer.h"
#include "ParamDescriptor.h"
#include "ParamIndex.h"

// NOTE ON THIS FILE'S NAME
// ------------------------
// This is the v2 LaneEffect contract, written as a separate header so WP1 can migrate the
// eight effects one at a time against a green build and a passing golden-baseline check,
// rather than in one atomic edit that leaves the tree unbuildable in between. Once every
// effect implements it, this file's contents replace LaneEffect.h and the old header (and
// this note) are deleted. Do not add a second, divergent effect base class here -- the whole
// point of WP1 is that there is exactly one.

#ifdef STUTTER_RANGE_CHECK
#include <atomic>
namespace stutter::debug
{
    /** Test-only instrumentation (compiled in only by the render_test harness, which defines
        STUTTER_RANGE_CHECK): counts every effect read whose absolute capture-buffer position
        falls outside the slice window the effect claims to be reading from. The shipped plugin
        never defines the macro, so this has zero footprint in release binaries. */
    inline std::atomic<juce::int64> sliceRangeViolations { 0 };

    inline void checkSliceRange (double absPos, double windowStart, double windowEnd) noexcept
    {
        constexpr double eps = 1.0e-6; // float slop tolerance
        if (absPos < windowStart - eps || absPos > windowEnd + eps)
            sliceRangeViolations.fetch_add (1, std::memory_order_relaxed);
    }
}
 #define STUTTER_CHECK_SLICE_RANGE(absPos, windowStart, windowEnd) \
     ::stutter::debug::checkSliceRange ((absPos), (windowStart), (windowEnd))
#else
 // No-op variant still "uses" its arguments so variables computed only for the range check
 // don't trigger -Wunused-variable in the (non-instrumented) plugin build.
 #define STUTTER_CHECK_SLICE_RANGE(absPos, windowStart, windowEnd) \
     juce::ignoreUnused ((absPos), (windowStart), (windowEnd))
#endif

namespace stutter
{

/**
    How a lane's output combines with the rest of the chain.

    v1 had two categories; v2 adds Send, which exists specifically so a feedback effect
    (Delay) can sit outside the serial Texture chain. Feeding a delay's output back into a
    chain that itself feeds the delay is re-entrant and produces runaway gain -- routing it
    as a send/return keeps the feedback path closed and bounded.
*/
enum class LaneCategory
{
    Buffer,   ///< Mutually exclusive: only the lowest-indexed active Buffer lane plays.
    Texture,  ///< Stacks serially on top of whatever Buffer lane (or dry signal) is active.
    Send      ///< Runs after the Texture chain; output sums into a return bus at the end.
};

/**
    Controls when BlockSequencer calls onBlockStart() for a lane that stays continuously
    active across adjacent blocks.

      - RetriggerEachBlock: onBlockStart() fires at every block, even when the lane was
        already active. Correct for effects whose musical identity *is* the retrigger
        (Stutter chopping every division).

      - ContinueThroughRun: onBlockStart() fires only at the start of an unbroken run of
        adjacent blocks; a lane still active from the previous block keeps running across the
        boundary without re-latching its anchor or envelope. Correct for directional one-shot
        envelopes (TapeStop decelerating, TapeStart spinning up) -- re-latching every division
        would mean they never reach their target speed.

      - RetriggerEachDivision: NEW in v2. onBlockStart() fires on every division boundary
        *inside* a block. Variable-length blocks make this necessary: a slice-based effect
        given a 16-division block would otherwise freeze on one slice for four bars. Lets
        Shuffler re-roll its slice and Stretcher re-seed its grain stream at a musical rate
        while still being driven by a single long block.

    v1's RetriggerEachStep is spelled RetriggerEachBlock here; the semantics are unchanged
    (a v1 step is a v2 block of length 1 division).
*/
enum class RetriggerPolicy
{
    RetriggerEachBlock,
    ContinueThroughRun,
    RetriggerEachDivision
};

/**
    Everything an effect needs at the moment it is (re)triggered.

    Replaces v1's onStepStart(capture, stepLengthSamples, nowAbs). The critical change is
    splitting v1's single stepLengthSamples into two distinct quantities, because v1
    conflated them and papered over the difference with a magic constant:

        // v1, StutterEffect.h:59
        baseLoopLenSamples = jmax (32.0, stepLengthSamples * fraction * 4.0);

    That `* 4.0` exists solely because `fraction` is a fraction of a *bar* while
    stepLengthSamples was hardcoded to a 16th note -- four 16ths per quarter, four quarters
    per bar. With variable divisions (2..8 per beat) a step is no longer a 16th and the
    constant is simply wrong. Separating "how long am I active" from "what is the musical
    unit" removes the fudge instead of rescaling it.
*/
struct BlockContext
{
    /** CaptureBuffer-absolute sample position ("total written" coordinate) of the trigger.
        Use as a fixed anchor for readInterpolatedAbsolute() rather than re-deriving position
        from the moving write head. */
    juce::int64 blockStartAbs = 0;

    /** Length of the whole active block in samples. Blocks are variable-length in v2, so
        this can be anything from one division to the full pattern. Use for envelopes that
        should span the entire held region (TapeStop's deceleration). */
    double blockLengthSamples = 0.0;

    /** Length of one division in samples -- the musical grid unit. Use for anything that
        should track note values regardless of how long the block happens to be (Stutter's
        loop length, Reverse's slice length). This is what v1 called stepLengthSamples, minus
        the 16th-note assumption. */
    double divisionLengthSamples = 0.0;

    /** Bar fraction of one division, i.e. divisionLengthSamples expressed musically. A
        division at beats=4/divisions=4 is a 16th, so this is 1/16. Lets an effect convert a
        note-value parameter into samples without knowing the tempo:
            samples = noteFraction / divisionBarFraction * divisionLengthSamples */
    double divisionBarFraction = 1.0 / 16.0;

    double ppqPerSample = 0.0;
    double sampleRate = 44100.0;

    /** Deterministic per-trigger seed, derived from (scene seed, trigger count, block index).
        Effects that randomise MUST seed a local generator from this and must never touch a
        global RNG -- a global would make output depend on how the host happened to chunk the
        buffer, breaking both offline-render reproducibility and the DeterminismTests check
        that identical seeds survive a change of block size. */
    juce::uint32 seed = 0;

    /** True when this is a re-trigger of an already-audible lane, false on a fresh start
        from silence. Effects use it to arm RetriggerSmoother: a fresh start is masked by the
        sequencer's own gain ramp, a retrigger is not. */
    bool isRetrigger = false;

    /** The triggering block's modulation tier (0 Locked, 1 Split, 2 Custom). Effects do not
        branch on this -- the tier is already resolved into a baked curve before the audio
        thread sees anything, which is what makes the three tiers cost the same. It is
        carried here only so an effect can expose tier-dependent *display* state; treat it as
        informational. */
    juce::uint8 tier = 0;
};

/**
    Per-sample state handed to processSample().

    Replaces v1's (progress, nowAbs) pair. `blockProgress` is the direct analogue of v1's
    `progress`, renamed because it now runs across a variable-length block rather than a
    fixed step.
*/
struct SampleContext
{
    /** This sample's CaptureBuffer-absolute position. */
    juce::int64 nowAbs = 0;

    /** 0..1 position through the active block. */
    double blockProgress = 0.0;

    /** 0..1 position through the whole pattern. Curves are evaluated against this, and an
        effect can use it to phase-align with the pattern rather than with its own block. */
    double patternPhase = 0.0;

    /** Post-modulation values for this lane, indexed by descriptor position -- the same
        indexing as LaneParams. Non-null for the lifetime of the call. Effects read this only
        for parameters whose descriptor sets latchAtBlockStart = false; latched parameters
        were already captured in onBlockStart(). */
    const float* modulatedParams = nullptr;

    /** True while a Palindrome pattern is playing its reverse pass. Direction-sensitive
        effects must honour it -- a Reverse block during a backward pass should play forward,
        or the palindrome reads as a bug rather than an effect. */
    bool reverseDirection = false;

    /** True when the gesture's Release mode is Stick and the note has been released: the
        effect should hold its current output rather than advance. Effects that cannot
        meaningfully freeze may ignore this; the gesture layer falls back to a gain ramp. */
    bool freeze = false;
};

/**
    Base class for a single sequencer lane's DSP effect.

    Lifecycle, per active block:
      1. onBlockStart() once when the block begins (or per division / not at all on
         continuation, per getRetriggerPolicy())
      2. processSample() for every sample in the active region
      3. onBlockEnd() once when the region ends

    Effects read from the shared CaptureBuffer and must not allocate, lock, or perform I/O in
    any of these calls.

    Effects must NOT read APVTS. All eight v1 effects had a private getParam() that hashed
    "lane{N}_{name}" through APVTS on the audio thread (e.g. StutterEffect.h:187-192); in v2
    APVTS merely mirrors the active scene, so reading it here would race with scene switching
    and would make modulation impossible to apply without writing back into APVTS and
    fighting host automation. Values arrive via LaneParams and SampleContext instead.
*/
class LaneEffect
{
public:
    explicit LaneEffect (LaneCategory cat) : category (cat) {}
    virtual ~LaneEffect() = default;

    virtual void prepare (double sampleRate, int numChannels) = 0;
    virtual void reset() = 0;

    /** Called once when a block region begins (see RetriggerPolicy). `params` holds this
        lane's post-modulation values in descriptor order; latch whatever defines the block's
        structure here. */
    virtual void onBlockStart (const CaptureBuffer& capture, const LaneParams& params,
                               const BlockContext& ctx) = 0;

    /** Process one sample for each channel, in place. */
    virtual void processSample (const CaptureBuffer& capture, float* channelSamples, int numChannels,
                                const SampleContext& ctx) = 0;

    virtual void onBlockEnd() {}

    LaneCategory getCategory() const noexcept { return category; }

    virtual RetriggerPolicy getRetriggerPolicy() const noexcept
    {
        return RetriggerPolicy::RetriggerEachBlock;
    }

    /** This effect's parameter declarations, in the order LaneParams indexes them. Must
        return a pointer to storage with static lifetime (a function-local static constexpr
        array), and must return the same set every call -- the APVTS layout, the UI, and the
        scene schema are all generated from it. */
    virtual ParamDescriptorSet getParamDescriptors() const noexcept = 0;

    /** Display name, used for the lane header and the modulation target list. */
    virtual const char* getName() const noexcept = 0;

protected:
    LaneCategory category;
};

/**
    Smooths the output discontinuity when a RetriggerEachBlock buffer effect re-anchors while
    already audible.

    Why this exists: the sequencer's per-lane gain crossfade only ramps on activation from
    silence -- on a retrigger of an *already-active* lane the gain is already 1.0, so nothing
    in the outer mixer masks the jump from the old anchor's audio to the new anchor's audio
    on the first sample. This helper latches the last output sample at retrigger time and
    equal-power blends from that held value into the new slice's output over ~1.5ms. It
    performs no CaptureBuffer reads of its own, so it cannot violate any effect's
    slice-window read guarantee.

    Usage: prepare() from the effect's prepare(), reset() from reset()/onBlockEnd(),
    notifyRetrigger() at the top of onBlockStart(), and process() once per sample at the end
    of processSample() (after the effect has written its output into channelSamples).

    Carried over from v1 unchanged -- it is policy-agnostic, so RetriggerEachDivision works
    with it as-is.
*/
class RetriggerSmoother
{
public:
    void prepare (double sampleRate) noexcept
    {
        fadeLenSamples = juce::jmax (1.0, sampleRate * 0.0015); // 1.5ms
        reset();
    }

    void reset() noexcept
    {
        playing = false;
        fadeActive = false;
        fadePos = 0.0;
    }

    /** Call from onBlockStart(). Arms the blend only when the effect was already producing
        output (a genuine retrigger); a fresh start from silence is masked by the outer
        sequencer's gain ramp instead. */
    void notifyRetrigger() noexcept
    {
        if (playing)
        {
            fadeActive = true;
            fadePos = 0.0;
            heldSample = lastSample;
        }
    }

    /** Call once per sample after the effect computed its output. */
    void process (float* channelSamples, int numCh) noexcept
    {
        if (fadeActive)
        {
            const float t = (float) juce::jlimit (0.0, 1.0, fadePos / fadeLenSamples);
            const float gIn = std::sin (0.5f * juce::MathConstants<float>::pi * t);
            const float gOut = std::cos (0.5f * juce::MathConstants<float>::pi * t);
            for (int c = 0; c < numCh && c < maxChannels; ++c)
                channelSamples[c] = heldSample[(size_t) c] * gOut + channelSamples[c] * gIn;

            fadePos += 1.0;
            if (fadePos >= fadeLenSamples)
                fadeActive = false;
        }

        for (int c = 0; c < numCh && c < maxChannels; ++c)
            lastSample[(size_t) c] = channelSamples[c];
        playing = true;
    }

private:
    static constexpr int maxChannels = 8;

    std::array<float, maxChannels> lastSample {};
    std::array<float, maxChannels> heldSample {};
    double fadeLenSamples = 66.0;
    double fadePos = 0.0;
    bool fadeActive = false;
    bool playing = false;
};

} // namespace stutter
