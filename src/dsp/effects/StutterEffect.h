#pragma once
#include "../LaneEffect.h"
#include "../ParameterIDs.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace stutter
{

/**
    Stutter: repeats a short slice of the capture buffer in a loop, with the loop
    optionally shrinking on each repeat (Stutter-Edit style gradient) and an optional
    pitch slide across repeats (achieved via changing playback rate over time).
*/
class StutterEffect : public LaneEffect
{
public:
    StutterEffect (juce::AudioProcessorValueTreeState& state, int laneIdx)
        : LaneEffect (LaneCategory::Buffer), apvts (state), laneIndex (laneIdx) {}

    void prepare (double sampleRateIn, int numChannelsIn) override
    {
        sampleRate = sampleRateIn;
        numChannels = numChannelsIn;
        retrigSmoother.prepare (sampleRate);
        reset();
    }

    void reset() override
    {
        readPosSamples = 0.0;
        loopLenSamples = 0.0;
        repeatCount = 0;
        currentRateSemitoneOffset = 0.0f;
        anchorAbs = 0;
        retrigSmoother.reset();
    }

    void onStepEnd() override
    {
        // Step region fully ended (outer gain faded to silence): the next onStepStart is a
        // fresh start, not a retrigger -- don't blend from stale held output.
        retrigSmoother.reset();
    }

    void onStepStart (const CaptureBuffer& capture, int stepLengthSamples, juce::int64 nowAbs) override
    {
        juce::ignoreUnused (capture);

        // If we were already audible (per-step retrigger of an active lane), arm a short blend
        // from the previous output into the newly-anchored slice: the outer sequencer's gain
        // crossfade cannot mask this transition (gain is already 1.0 on a retrigger).
        retrigSmoother.notifyRetrigger();

        const float rateParam = getParam (ID::stutterRate, 4.0f);   // divisions index, see rateToFraction
        const float decayParam = getParam (ID::stutterDecay, 0.0f); // 0..1: how much loop shrinks per repeat
        pitchSlideSemis = getParam (ID::stutterPitchSlide, 0.0f);

        const double fraction = rateToFraction ((int) rateParam);
        baseLoopLenSamples = juce::jmax (32.0, stepLengthSamples * fraction * 4.0); // stepLength = 1/16, scale to musical fraction of a bar
        loopLenSamples = baseLoopLenSamples;
        decayAmount = decayParam;
        readPosSamples = 0.0;
        repeatCount = 0;
        currentRateSemitoneOffset = 0.0f;
        // Anchor: the slice being looped is the loopLenSamples immediately before "now", fixed
        // for the lifetime of this trigger. All reads below are expressed as an absolute
        // CaptureBuffer position derived from this anchor, so they no longer drift as the ring
        // buffer's write head advances on subsequent blocks.
        anchorAbs = nowAbs;
    }

    void processSample (const CaptureBuffer& capture, float* channelSamples, int numCh, double progress,
                         juce::int64 nowAbs) override
    {
        juce::ignoreUnused (progress, nowAbs);

        // Pitch slide: ramps semitone offset from 0 to pitchSlideSemis across repeats (capped at ~8 repeats span)
        const float slideProgress = juce::jlimit (0.0f, 1.0f, (float) repeatCount / 8.0f);
        currentRateSemitoneOffset = pitchSlideSemis * slideProgress;
        const double playbackRate = std::pow (2.0, currentRateSemitoneOffset / 12.0);

        // Fixed-anchor absolute read: position = anchor - loopLen + readPos, i.e. readPos steps
        // forward from the start of the frozen slice (anchor - loopLen) toward the anchor itself.
        const double fadeLen = loopCrossfadeSamples();
        const double sliceStart = (double) anchorAbs - loopLenSamples;
        const double absPos = sliceStart + readPosSamples;

        // Warp crossfade into the *next revolution* across the loop wrap point: during the last
        // fadeLen samples of this revolution, the outgoing tail is equal-power blended with the
        // next revolution's opening samples, starting exactly at the next revolution's slice
        // start (anchor - nextLoopLength(); with decay the next slice is shorter, so its start
        // sits later in time than this one's). After the wrap, readPosSamples resumes at fadeLen
        // (see below) so the head samples the fade partner already played are not played twice --
        // the blend hands off seamlessly into the new revolution. Both read positions stay inside
        // [anchorAbs - loopLenSamples, anchorAbs] by construction: the main read because
        // readPos in [0, loopLen), the partner because it spans
        // [anchor - nextLoopLen, anchor - nextLoopLen + fadeLen) and nextLoopLen <= loopLen while
        // fadeLen <= 0.1 * loopLen <= nextLoopLen.
        const bool inFade = readPosSamples >= loopLenSamples - fadeLen;
        double wrapAbsPos = 0.0;
        float gOut = 1.0f, gIn = 0.0f;
        if (inFade)
        {
            const double fadeProgress = readPosSamples - (loopLenSamples - fadeLen);
            const double fadeT = juce::jlimit (0.0, 1.0, fadeLen > 0.0 ? fadeProgress / fadeLen : 1.0);
            wrapAbsPos = ((double) anchorAbs - nextLoopLength()) + fadeProgress;
            gOut = std::cos (0.5f * juce::MathConstants<float>::pi * (float) fadeT);
            gIn = std::sin (0.5f * juce::MathConstants<float>::pi * (float) fadeT);
        }

        STUTTER_CHECK_SLICE_RANGE (absPos, sliceStart, (double) anchorAbs);
        if (inFade)
            STUTTER_CHECK_SLICE_RANGE (wrapAbsPos, sliceStart, (double) anchorAbs);

        for (int c = 0; c < numCh; ++c)
        {
            float sample = capture.readInterpolatedAbsolute (c, absPos);
            if (inFade)
                sample = sample * gOut + capture.readInterpolatedAbsolute (c, wrapAbsPos) * gIn;
            channelSamples[c] = sample;
        }

        retrigSmoother.process (channelSamples, numCh);

        readPosSamples += playbackRate;

        if (readPosSamples >= loopLenSamples)
        {
            const double overshoot = readPosSamples - loopLenSamples;
            // Loop-length decay: shrink loop by decayAmount each repeat (gradient toward shorter
            // stutters). The anchor itself (the frozen slice's *end* point) never moves -- only
            // the slice's start point (anchorAbs - loopLenSamples) creeps later in time as
            // loopLenSamples shrinks, so every repeat still plays a contiguous chunk of the same
            // originally-captured audio, just a progressively shorter tail of it. Must match
            // what nextLoopLength() predicted during the fade above (both derive the shrink from
            // the same pre-increment repeatCount).
            loopLenSamples = nextLoopLength();
            ++repeatCount;
            // Resume past the head samples the fade partner already played (see fade comment).
            readPosSamples = fadeLen + overshoot;
        }
    }

private:
    static double rateToFraction (int index)
    {
        // index -> fraction of a bar; includes straight, triplet, dotted variants
        static const double table[] = {
            1.0 / 4.0,          // 0: 1/4
            1.0 / 8.0,          // 1: 1/8
            1.0 / 16.0,         // 2: 1/16
            1.0 / 32.0,         // 3: 1/32
            1.0 / 64.0,         // 4: 1/64
            (1.0 / 4.0) * (2.0 / 3.0),  // 5: 1/4 triplet
            (1.0 / 8.0) * (2.0 / 3.0),  // 6: 1/8 triplet
            (1.0 / 16.0) * (2.0 / 3.0), // 7: 1/16 triplet
            (1.0 / 4.0) * 1.5,  // 8: 1/4 dotted
            (1.0 / 8.0) * 1.5,  // 9: 1/8 dotted
            (1.0 / 16.0) * 1.5, // 10: 1/16 dotted
        };
        constexpr int n = (int) (sizeof (table) / sizeof (double));
        index = juce::jlimit (0, n - 1, index);
        return table[index];
    }

    // Loop-boundary warp crossfade length: 2-5ms, capped at 10% of the loop so short/decayed
    // loops (Stutter-Edit decay shrinking the loop toward 32 samples) never fade more than a
    // tenth of their own length.
    double loopCrossfadeSamples() const noexcept
    {
        const double msSamples = sampleRate * 0.003; // 3ms
        return juce::jmin (msSamples, loopLenSamples * 0.1);
    }

    // Deterministic preview of the loop length the *next* revolution will use once the current
    // one wraps (decay shrink for repeat repeatCount+1). Used both by the warp crossfade (so the
    // fade partner starts exactly at the next revolution's slice start) and by the wrap handler
    // itself -- the two must agree or the crossfade would hand off to the wrong position.
    double nextLoopLength() const noexcept
    {
        if (decayAmount <= 0.0f)
            return loopLenSamples;
        const double shrink = 1.0 - (0.35 * decayAmount * (1.0 / (1.0 + (repeatCount + 1) * 0.5)));
        return juce::jmax (32.0, loopLenSamples * shrink);
    }

    float getParam (const juce::String& name, float fallback) const
    {
        if (auto* p = apvts.getRawParameterValue (ID::lanePrefix (laneIndex) + name))
            return p->load();
        return fallback;
    }

    juce::AudioProcessorValueTreeState& apvts;
    int laneIndex;
    double sampleRate = 44100.0;
    int numChannels = 2;

    double readPosSamples = 0.0;
    double loopLenSamples = 0.0;
    double baseLoopLenSamples = 0.0;
    float decayAmount = 0.0f;
    int repeatCount = 0;
    float pitchSlideSemis = 0.0f;
    float currentRateSemitoneOffset = 0.0f;
    juce::int64 anchorAbs = 0;
    RetriggerSmoother retrigSmoother;
};

} // namespace stutter
