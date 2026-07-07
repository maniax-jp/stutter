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
        writePos.store (pos, std::memory_order_relaxed);
        totalWritten.fetch_add (numSamples, std::memory_order_relaxed);
    }

    /** Current write head position (samples into the ring). */
    int getWritePosition() const noexcept { return writePos.load (std::memory_order_relaxed); }

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

        channel = juce::jmin (channel, numCh - 1);
        samplesAgo = juce::jlimit (0.0, (double) (lengthSamples - 2), samplesAgo);

        const double posD = (double) writePos.load (std::memory_order_relaxed) - samplesAgo;
        double wrapped = std::fmod (posD, (double) lengthSamples);
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
        channel = juce::jmin (channel, numCh - 1);
        samplesAgo = juce::jlimit (0, lengthSamples - 1, samplesAgo);
        int idx = writePos.load (std::memory_order_relaxed) - samplesAgo;
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
