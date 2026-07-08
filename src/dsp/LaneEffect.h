#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include "CaptureBuffer.h"

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

/** Buffer-type lanes are mutually exclusive (only the highest-priority active one plays).
    Texture-type lanes stack additively on top of whatever buffer lane (or dry signal) is active. */
enum class LaneCategory
{
    Buffer,
    Texture
};

/**
    Controls when StepSequencer calls onStepStart() for a lane that stays continuously active
    across consecutive steps (same lane, back-to-back steps both ON):

      - RetriggerEachStep: onStepStart() fires on every step boundary, even if the lane was
        already active on the previous step. Correct for effects whose musical identity *is*
        the retrigger (e.g. Stutter chopping every 16th).
      - ContinueThroughRun: onStepStart() fires only at the start of a continuous run of ON
        steps; a lane that is still active from the previous step (same unbroken ON region)
        keeps running through the step boundary without re-latching its anchor/envelope. Correct
        for effects with a directional one-shot envelope across the whole ON region (e.g.
        TapeStop decelerating, TapeStart spinning up) -- restarting every 16th would mean they
        never actually reach their target speed within a busy pattern.
*/
enum class RetriggerPolicy
{
    RetriggerEachStep,
    ContinueThroughRun
};

/**
    Base class for a single step-sequencer lane's DSP effect.

    Lifecycle per block, per sample, while a step is active:
      1. onStepStart() is called once when the step region begins (or when re-triggered,
         subject to getRetriggerPolicy())
      2. processSample() is called for every sample in the active region
      3. onStepEnd() is called once when the step region ends

    Effects read from the shared CaptureBuffer and must not allocate or lock in
    processSample()/onStepStart()/onStepEnd().
*/
class LaneEffect
{
public:
    explicit LaneEffect (LaneCategory cat) : category (cat) {}
    virtual ~LaneEffect() = default;

    virtual void prepare (double sampleRate, int numChannels) = 0;
    virtual void reset() = 0;

    /** Called once, on the first sample of an active step region (or on every step for
        RetriggerEachStep lanes; see RetriggerPolicy). `nowAbs` is the CaptureBuffer-absolute
        sample position ("total written" coordinate) of this exact sample -- use it as a fixed
        anchor for any CaptureBuffer::readInterpolatedAbsolute() calls in processSample(), rather
        than re-deriving position from a moving write head.
        stepLengthSamples = length of the *current* step (for tempo-scaled envelopes). */
    virtual void onStepStart (const CaptureBuffer& capture, int stepLengthSamples, juce::int64 nowAbs) = 0;

    /** Process one sample for each channel in place. `dry` holds the current dry/captured
        input sample (already-captured live signal) if the effect needs to blend with it.
        `progress` is 0..1 position through the active step region. `nowAbs` is this sample's
        CaptureBuffer-absolute position (see onStepStart). */
    virtual void processSample (const CaptureBuffer& capture, float* channelSamples, int numChannels,
                                 double progress, juce::int64 nowAbs) = 0;

    virtual void onStepEnd() {}

    LaneCategory getCategory() const noexcept { return category; }

    /** Whether this lane re-latches (calls onStepStart again) on every step boundary even while
        continuously active, or only at the start of a continuous ON run. Defaults to
        RetriggerEachStep (matches every existing lane's prior behaviour except where overridden
        below for the buffer lanes that need a continuous one-shot envelope). */
    virtual RetriggerPolicy getRetriggerPolicy() const noexcept { return RetriggerPolicy::RetriggerEachStep; }

protected:
    LaneCategory category;
};

/**
    Smooths the output discontinuity when a RetriggerEachStep buffer effect re-anchors on a step
    boundary while already audible.

    Why this exists: StepSequencer's per-lane gain crossfade only ramps on activation from
    silence -- on a retrigger of an *already-active* lane the gain is already 1.0, so nothing in
    the outer mixer masks the jump from the old anchor's audio to the new anchor's audio on the
    first sample of the new step. This helper latches the last output sample at retrigger time
    and equal-power blends from that held value into the new slice's output over a short window
    (~1.5ms). It performs no CaptureBuffer reads of its own, so it cannot violate any effect's
    slice-window read guarantee.

    Usage: call prepare() from the effect's prepare(), reset() from reset()/onStepEnd(),
    notifyRetrigger() at the top of onStepStart(), and process() once per sample at the end of
    processSample() (after the effect has written its output into channelSamples).
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

    /** Call from onStepStart(). Arms the blend only when the effect was already producing
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
