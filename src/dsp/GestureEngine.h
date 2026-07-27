#pragma once
#include "../state/SceneSnapshot.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>

namespace stutter
{

/** How the plugin decides which scene is playing. */
enum class PlayMode
{
    /** Free-run the active scene continuously; MIDI only selects which one. This is v1's
        behaviour and the mode to edit in -- Stutter Edit 2 makes the same recommendation,
        because a UI that keeps re-triggering is hard to edit against. */
    Auto = 0,

    /** Wet output only while a note is held (or latched). Turns the plugin into an
        instrument rather than an always-on insert. */
    Midi
};

/**
    Maps MIDI notes onto scenes and decides when the wet path is audible.

    Everything here runs on the audio thread inside processBlock. It allocates nothing: the
    pending-trigger queue is a fixed array, and the note->scene map is a plain 128-entry table
    published by the message thread.

    The gate is deliberately NOT applied by muting the sequencer. It is a gain the processor
    multiplies into the wet signal, so every transition is a ramp and is click-free by
    construction rather than by care.
*/
class GestureEngine
{
public:
    GestureEngine() = default;

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        // ~5ms, matching the sequencer's lane crossfade. Long enough to be inaudible, short
        // enough that a staccato note still sounds staccato.
        gateRampSamples = juce::jmax (1, (int) (sampleRate * 0.005));
        reset();
    }

    void reset()
    {
        // Open by default. Auto mode -- the default and the only mode v1 had -- is meant to
        // be always-on, so starting closed would silence the plugin until the first
        // processMidi call ramped it up, and would silence it permanently on any path that
        // does not call processMidi at all. MIDI mode closes it explicitly on the first
        // block, which is early enough that no note is ever missed.
        gateGain = 1.0f;
        gateTarget = 1.0f;
        pendingCount = 0;
        heldNotes.fill (false);
        heldCount = 0;
        latchedScene = -1;
        releasePendingPpq = -1.0;
        releaseArmed = false;
        triggerCount = 0;
    }

    // ---- Message-thread configuration --------------------------------------------------

    void setPlayMode (PlayMode m) noexcept { playMode.store (m, std::memory_order_relaxed); }
    PlayMode getPlayMode() const noexcept { return playMode.load (std::memory_order_relaxed); }

    /** When set, incoming notes stop selecting scenes (they still gate). Lets a scene be
        edited while a track plays notes that would otherwise keep stealing focus -- Glitch 2
        calls this Scene Lock, and it is a better answer than telling the user to switch
        modes. */
    void setSceneLock (bool locked) noexcept { sceneLock.store (locked, std::memory_order_relaxed); }
    bool isSceneLocked() const noexcept { return sceneLock.load (std::memory_order_relaxed); }

    /** Quantize grid in quarter notes; 0 disables. */
    void setTriggerQuantize (double ppq) noexcept { quantizePpq.store (ppq, std::memory_order_relaxed); }

    /** Note -> scene index, or -1 for an unmapped key. Written by the message thread while
        the audio thread reads; entries are independent bytes with no cross-entry invariant,
        so a stale read costs at most one block of the previous mapping. */
    void setNoteMapping (int note, int sceneIndex) noexcept
    {
        if (note >= 0 && note < 128)
            noteToScene[(size_t) note].store ((int8_t) juce::jlimit (-1, maxScenes - 1, sceneIndex),
                                              std::memory_order_relaxed);
    }

    /** Default 1:1 mapping: note N selects scene N. */
    void setIdentityMapping() noexcept
    {
        for (int i = 0; i < 128; ++i)
            noteToScene[(size_t) i].store ((int8_t) i, std::memory_order_relaxed);
    }

    /** Select a scene directly, e.g. from the scene browser. Flags a mirror like a MIDI
        trigger does -- clicking a scene in the UI must update the visible parameters for the
        same reason playing a note does, and forgetting that here would make the browser and
        the keyboard behave differently for no reason the user could see. */
    void setActiveScene (int index) noexcept
    {
        const int clamped = juce::jlimit (0, maxScenes - 1, index);
        const int previous = activeScene.exchange (clamped, std::memory_order_relaxed);
        if (previous != clamped)
            pendingMirrorScene.store (clamped, std::memory_order_release);
    }

    int getActiveScene() const noexcept { return activeScene.load (std::memory_order_relaxed); }

    /** Set by the audio thread when a trigger changes the scene; the processor's timer reads
        it to mirror the new scene's parameters into APVTS. -1 means nothing pending. */
    int consumePendingMirror() noexcept
    {
        return pendingMirrorScene.exchange (-1, std::memory_order_acq_rel);
    }

    /** How many times the active scene has been (re)triggered. Feeds deriveSeed so a
        retrigger produces a different-but-reproducible random stream. */
    uint32_t getTriggerCount() const noexcept { return triggerCount; }

    // ---- Audio thread ------------------------------------------------------------------

    /**
        Consume this chunk's MIDI and advance the gate.

        `releaseMode` and `loopPolicy` come from the scene that is active now, so changing
        scenes changes the release behaviour with it -- Stutter Edit 2 stores Release per
        gesture for the same reason.
    */
    void processMidi (const juce::MidiBuffer& midi, int numSamples,
                      double ppqAtChunkStart, double ppqPerSample,
                      ReleaseMode releaseMode)
    {
        const bool midiMode = getPlayMode() == PlayMode::Midi;

        for (const auto meta : midi)
        {
            const auto msg = meta.getMessage();

            if (msg.isNoteOn())
                handleNoteOn (msg.getNoteNumber(), ppqAtChunkStart, ppqPerSample,
                              meta.samplePosition);
            else if (msg.isNoteOff())
                handleNoteOff (msg.getNoteNumber(), ppqAtChunkStart, ppqPerSample,
                               meta.samplePosition, releaseMode);
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            {
                heldNotes.fill (false);
                heldCount = 0;
                latchedScene = -1;
                gateTarget = midiMode ? 0.0f : 1.0f;
            }
        }

        // Fire any quantized triggers whose target position falls inside this chunk.
        firePendingTriggers (ppqAtChunkStart, ppqPerSample, numSamples);

        // Resolve an armed release whose boundary has now passed.
        if (releaseArmed && releasePendingPpq >= 0.0)
        {
            const double ppqAtEnd = ppqAtChunkStart + ppqPerSample * (double) numSamples;
            if (ppqAtEnd >= releasePendingPpq)
            {
                gateTarget = 0.0f;
                releaseArmed = false;
                releasePendingPpq = -1.0;
            }
        }

        if (! midiMode)
            gateTarget = 1.0f;
    }

    /** Per-sample wet gain. Ramps toward the target so no transition is a step. */
    float nextGateGain() noexcept
    {
        const float step = 1.0f / (float) gateRampSamples;
        if (gateGain < gateTarget)
            gateGain = juce::jmin (gateTarget, gateGain + step);
        else if (gateGain > gateTarget)
            gateGain = juce::jmax (gateTarget, gateGain - step);
        return gateGain;
    }

    float getGateGain() const noexcept { return gateGain; }

    /** True when the gesture is releasing under Stick, so effects hold rather than advance. */
    bool isFrozen() const noexcept { return frozen; }

private:
    void handleNoteOn (int note, double ppqAtChunkStart, double ppqPerSample, int sampleOffset)
    {
        if (note < 0 || note >= 128)
            return;

        if (! heldNotes[(size_t) note])
        {
            heldNotes[(size_t) note] = true;
            ++heldCount;
        }

        frozen = false;
        releaseArmed = false;
        releasePendingPpq = -1.0;

        const int scene = noteToScene[(size_t) note].load (std::memory_order_relaxed);
        const double notePpq = ppqAtChunkStart + ppqPerSample * (double) sampleOffset;
        const double grid = quantizePpq.load (std::memory_order_relaxed);

        if (grid <= 0.0)
        {
            fireTrigger (scene);
            return;
        }

        // Quantize, but let a note that arrives slightly EARLY still land on the boundary it
        // was aiming at rather than waiting a whole grid unit. Stutter Edit 2 makes a point
        // of this: it is what lets a player anticipate the beat instead of chasing it.
        const double next = std::ceil (notePpq / grid) * grid;
        const double prev = std::floor (notePpq / grid) * grid;
        const double earlyWindow = grid * 0.5;
        const double target = (next - notePpq <= earlyWindow) ? next : prev;

        if (target <= notePpq)
        {
            fireTrigger (scene);
            return;
        }

        if (pendingCount < (int) pending.size())
            pending[(size_t) pendingCount++] = { scene, target };
    }

    void handleNoteOff (int note, double ppqAtChunkStart, double ppqPerSample,
                        int sampleOffset, ReleaseMode releaseMode)
    {
        if (note < 0 || note >= 128)
            return;

        if (heldNotes[(size_t) note])
        {
            heldNotes[(size_t) note] = false;
            heldCount = juce::jmax (0, heldCount - 1);
        }

        // Only the last note released ends the gesture; releasing one of several held notes
        // leaves the others sounding, which is what a player expects.
        if (heldCount > 0)
            return;

        if (getPlayMode() != PlayMode::Midi)
            return;

        const double offPpq = ppqAtChunkStart + ppqPerSample * (double) sampleOffset;

        switch (releaseMode)
        {
            case ReleaseMode::Latch:
                // Note-off is ignored entirely; the scene stays until another note arrives.
                latchedScene = getActiveScene();
                break;

            case ReleaseMode::Instant:
                gateTarget = 0.0f;
                break;

            case ReleaseMode::Stick:
                // Freeze on the final state rather than releasing. TapeStopEffect already
                // holds its last sample once its envelope completes, so this mostly means
                // "stop advancing and keep the gate open".
                frozen = true;
                break;

            case ReleaseMode::FullGesture:
            {
                // Run to the end of the current pattern loop. Expressed in PPQ so it survives
                // a tempo change mid-gesture.
                const double patternPpq = juce::jmax (1.0, (double) currentPatternBeats);
                const double endOfLoop = std::ceil (offPpq / patternPpq) * patternPpq;
                releasePendingPpq = endOfLoop;
                releaseArmed = true;
                break;
            }

            case ReleaseMode::OnGrid:
            default:
            {
                const double grid = quantizePpq.load (std::memory_order_relaxed);
                if (grid <= 0.0)
                {
                    gateTarget = 0.0f;
                }
                else
                {
                    releasePendingPpq = std::ceil (offPpq / grid) * grid;
                    releaseArmed = true;
                }
                break;
            }
        }
    }

    void firePendingTriggers (double ppqAtChunkStart, double ppqPerSample, int numSamples)
    {
        if (pendingCount == 0)
            return;

        const double ppqAtEnd = ppqAtChunkStart + ppqPerSample * (double) numSamples;

        int write = 0;
        for (int i = 0; i < pendingCount; ++i)
        {
            if (pending[(size_t) i].targetPpq <= ppqAtEnd)
                fireTrigger (pending[(size_t) i].scene);
            else
                pending[(size_t) write++] = pending[(size_t) i];
        }
        pendingCount = write;
    }

    void fireTrigger (int scene)
    {
        ++triggerCount;
        gateTarget = 1.0f;
        frozen = false;

        // Scene Lock keeps the note as a gate but refuses to change the selection.
        if (scene >= 0 && ! isSceneLocked())
        {
            const int previous = activeScene.exchange (scene, std::memory_order_relaxed);
            if (previous != scene)
                pendingMirrorScene.store (scene, std::memory_order_release);
        }
    }

    struct PendingTrigger
    {
        int scene = -1;
        double targetPpq = 0.0;
    };

    std::array<std::atomic<int8_t>, 128> noteToScene {};
    std::array<bool, 128> heldNotes {};
    int heldCount = 0;

    /** Fixed capacity: a player cannot meaningfully queue more than this within one grid
        unit, and overflowing silently drops the newest rather than allocating. */
    std::array<PendingTrigger, 16> pending {};
    int pendingCount = 0;

    std::atomic<PlayMode> playMode { PlayMode::Auto };
    std::atomic<bool> sceneLock { false };
    std::atomic<double> quantizePpq { 0.0 };
    std::atomic<int> activeScene { 0 };
    std::atomic<int> pendingMirrorScene { -1 };

    double sampleRate = 44100.0;
    int gateRampSamples = 220;
    float gateGain = 0.0f;
    float gateTarget = 0.0f;

    int latchedScene = -1;
    double releasePendingPpq = -1.0;
    bool releaseArmed = false;
    bool frozen = false;
    uint32_t triggerCount = 0;

public:
    /** Pattern length in beats, for FullGesture. Set from the active scene each block. */
    int currentPatternBeats = 4;
};

} // namespace stutter
