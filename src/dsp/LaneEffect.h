#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "CaptureBuffer.h"

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
    Base class for a single step-sequencer lane's DSP effect.

    Lifecycle per block, per sample, while a step is active:
      1. onStepStart() is called once when the step region begins (or when re-triggered)
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

    /** Called once, on the first sample of an active step region.
        stepLengthSamples = length of the *current* step (for tempo-scaled envelopes). */
    virtual void onStepStart (const CaptureBuffer& capture, int stepLengthSamples) = 0;

    /** Process one sample for each channel in place. `dry` holds the current dry/captured
        input sample (already-captured live signal) if the effect needs to blend with it.
        `progress` is 0..1 position through the active step region. */
    virtual void processSample (const CaptureBuffer& capture, float* channelSamples, int numChannels,
                                 double progress) = 0;

    virtual void onStepEnd() {}

    LaneCategory getCategory() const noexcept { return category; }

protected:
    LaneCategory category;
};

} // namespace stutter
