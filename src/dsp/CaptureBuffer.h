#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace stutter
{

/**
    Always-on circular capture buffer. The audio thread continuously writes the
    incoming dry signal into this ring buffer; effect lanes read slices out of it
    (often looking slightly into the past). Holds at least 2 seconds of stereo audio.

    Real-time safe: no allocation happens after prepare().

    Note: both write() and the read*() methods are called from the audio thread only in this
    plugin's current design (there is no separate writer/reader thread split); the
    release/acquire ordering on writePos below is kept anyway for documentation clarity and as
    a safety margin should that assumption ever change.
*/
class CaptureBuffer
{
public:
    CaptureBuffer() = default;

    void prepare (double sampleRate, int numChannels, double bufferSeconds = 2.5)
    {
        sr = sampleRate;
        numCh = juce::jmax (1, numChannels);
        lengthSamples = juce::jmax (1, (int) std::ceil (sampleRate * bufferSeconds));

        buffer.setSize (numCh, lengthSamples, false, true, true);
        buffer.clear();
        writePos.store (0);
        totalWritten.store (0);
    }

    void reset()
    {
        buffer.clear();
        writePos.store (0);
        totalWritten.store (0);
    }

    /** Write a block from the processor's input into the ring buffer. Call once per processBlock. */
    void write (const juce::AudioBuffer<float>& input)
    {
        const int numSamples = input.getNumSamples();
        if (numSamples <= 0 || lengthSamples <= 0)
            return;

        int pos = writePos.load (std::memory_order_relaxed);

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* src = input.getReadPointer (juce::jmin (ch, input.getNumChannels() - 1));
            int remaining = numSamples;
            int srcOffset = 0;
            int localPos = pos;

            while (remaining > 0)
            {
                const int spaceToEnd = lengthSamples - localPos;
                const int n = juce::jmin (remaining, spaceToEnd);
                buffer.copyFrom (ch, localPos, src + srcOffset, n);
                localPos = (localPos + n) % lengthSamples;
                srcOffset += n;
                remaining -= n;
            }
        }

        pos = (pos + numSamples) % lengthSamples;
        // Release: publishes the ring-buffer writes above so that any thread doing an acquire
        // load of writePos is guaranteed to see the sample data that was just written. On
        // weakly-ordered architectures (e.g. Apple Silicon/ARM) relaxed would not provide this
        // guarantee; the cost of release/acquire here is negligible.
        writePos.store (pos, std::memory_order_release);
        totalWritten.fetch_add (numSamples, std::memory_order_relaxed);
    }

    /** Current write head position (samples into the ring). */
    int getWritePosition() const noexcept { return writePos.load (std::memory_order_acquire); }

    /** Total number of samples ever written (monotonic, not wrapped). */
    juce::int64 getTotalWritten() const noexcept { return totalWritten.load (std::memory_order_relaxed); }

    int getLength() const noexcept { return lengthSamples; }
    int getNumChannels() const noexcept { return numCh; }
    bool hasEnoughHistory (int samplesNeeded) const noexcept
    {
        return totalWritten.load (std::memory_order_relaxed) >= samplesNeeded;
    }

    /** Read a single interpolated sample (linear) for a channel at `samplesAgo` samples before the
        current write head. samplesAgo may be fractional. */
    float readInterpolated (int channel, double samplesAgo) const noexcept
    {
        if (lengthSamples <= 0)
            return 0.0f;

        channel = juce::jlimit (0, numCh - 1, channel);
        samplesAgo = juce::jlimit (0.0, (double) (lengthSamples - 2), samplesAgo);

        const double posD = (double) writePos.load (std::memory_order_acquire) - samplesAgo;
        double wrapped = std::fmod (posD, (double) lengthSamples);
        if (wrapped < 0)
            wrapped += (double) lengthSamples;

        const int i0 = (int) wrapped;
        const int i1 = (i0 + 1) % lengthSamples;
        const float frac = (float) (wrapped - (double) i0);

        const float* data = buffer.getReadPointer (channel);
        return data[i0] + frac * (data[i1] - data[i0]);
    }

    /** Read a single interpolated sample (linear) for a channel at an absolute position in the
        capture buffer's monotonic time coordinate (same units as getTotalWritten(): "samples
        written since prepare()/reset()"). Unlike readInterpolated() (which is relative to the
        *current* write head and therefore silently shifts every block as writePos advances),
        this reads a position that is fixed once computed -- callers that need a stable anchor
        (e.g. a Buffer-category lane effect that latched a slice at onStepStart) must use this
        instead of recomputing "samplesAgo" against a moving write head.

        absolutePos is clamped to the valid history window: no newer than the last sample
        actually written (can't read the future) and no older than one buffer-length behind it
        (can't read already-overwritten data). Out-of-range requests are clamped to the nearest
        valid edge rather than wrapping, so a caller that mis-tracks its anchor degrades to a
        held/repeated edge sample rather than jumping to unrelated audio. */
    float readInterpolatedAbsolute (int channel, double absolutePos) const noexcept
    {
        if (lengthSamples < 3)
            return 0.0f;

        channel = juce::jlimit (0, numCh - 1, channel);

        const juce::int64 written = totalWritten.load (std::memory_order_relaxed);
        if (written <= 0)
            return 0.0f; // nothing captured yet

        // Valid window: [written - (lengthSamples - 2), written - 1]. The upper bound is the
        // last sample actually written -- position `written` itself has NOT been written yet,
        // and its ring slot still holds the *oldest* sample (one full buffer-length in the
        // past), so clamping there would silently return audio from a whole revolution ago.
        // The lower bound prevents reading samples already overwritten by the ring wrapping
        // around; its extra sample of headroom (-2 rather than -1) keeps i1 = i0 + 1 inside
        // valid data for the interpolation below.
        const double upperBound = (double) written - 1.0;
        const double lowerBound = (double) written - (double) (lengthSamples - 2);
        absolutePos = juce::jlimit (lowerBound, upperBound, absolutePos);

        double wrapped = std::fmod (absolutePos, (double) lengthSamples);
        if (wrapped < 0)
            wrapped += (double) lengthSamples;

        const int i0 = (int) wrapped;
        const int i1 = (i0 + 1) % lengthSamples;
        const float frac = (float) (wrapped - (double) i0);

        const float* data = buffer.getReadPointer (channel);
        return data[i0] + frac * (data[i1] - data[i0]);
    }

    /** Direct (non-interpolated) read, `samplesAgo` samples before the write head. */
    float readSample (int channel, int samplesAgo) const noexcept
    {
        if (lengthSamples <= 0)
            return 0.0f;
        channel = juce::jlimit (0, numCh - 1, channel);
        samplesAgo = juce::jlimit (0, lengthSamples - 1, samplesAgo);
        int idx = writePos.load (std::memory_order_acquire) - samplesAgo;
        idx %= lengthSamples;
        if (idx < 0)
            idx += lengthSamples;
        return buffer.getReadPointer (channel)[idx];
    }

private:
    juce::AudioBuffer<float> buffer;
    std::atomic<int> writePos { 0 };
    std::atomic<juce::int64> totalWritten { 0 };
    int lengthSamples = 0;
    int numCh = 2;
    double sr = 44100.0;
};

} // namespace stutter
