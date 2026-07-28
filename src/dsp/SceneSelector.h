#pragma once
#include "../state/SceneSnapshot.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace stutter
{

/**
    Holds which scene is playing and whether it is heard.

    Both come from the host's automation lane. The plugin is built in the editor -- lanes,
    blocks, curves, parameters -- and *called* from automation: sceneSelect says which of the
    built scenes plays, active says where. That is the whole interface, deliberately. A glitch
    effect is off for most bars of a track, and deciding where it is on is the one thing the
    timeline is better at expressing than the editor is.

    The gate is NOT applied by muting the sequencer. It is a gain the processor multiplies into
    the wet signal, so a transition is a ramp rather than a step and is click-free by
    construction rather than by care. It also collapses wet toward *dry*, never toward silence:
    an inactive bar has to pass the source through, not punch a hole in the track.

    setActiveScene and the gate are written from the audio thread (processChunk polls the
    parameters); getActiveScene and consumePendingMirror are read from the message thread.
    Nothing here allocates.
*/
class SceneSelector
{
public:
    SceneSelector() = default;

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        // ~5ms, matching the sequencer's lane crossfade. Long enough to be inaudible, short
        // enough that a one-division stab still sounds like a stab.
        gateRampSamples = juce::jmax (1, (int) (sampleRate * 0.005));
        reset();
    }

    void reset()
    {
        // Open by default, matching the `active` parameter's default. Starting closed would
        // silence the plugin on any path that never reaches applyAutomation.
        gateGain = 1.0f;
        gateTarget = 1.0f;
    }

    // ---- Scene selection ---------------------------------------------------------------

    /** Select a scene. Flags a mirror so the editor's knobs follow what is being heard;
        skipped when the value is unchanged, which matters because hosts re-send the same
        automation value on every block. */
    void setActiveScene (int index) noexcept
    {
        const int clamped = juce::jlimit (firstSceneIndex, lastSceneIndex, index);
        const int previous = activeScene.exchange (clamped, std::memory_order_relaxed);
        if (previous != clamped)
            pendingMirrorScene.store (clamped, std::memory_order_release);
    }

    int getActiveScene() const noexcept { return activeScene.load (std::memory_order_relaxed); }

    /** Set when the scene changes; the processor's timer reads it to mirror the new scene's
        parameters into APVTS. -1 means nothing pending. */
    int consumePendingMirror() noexcept
    {
        return pendingMirrorScene.exchange (-1, std::memory_order_acq_rel);
    }

    // ---- Audio thread ------------------------------------------------------------------

    /**
        Apply this block's automation state.

        Polled from processChunk rather than driven by a parameterChanged listener: listeners
        fire on whatever thread the host feels like using, so touching the active scene from
        one would let a change land mid-render. Polling also keeps the guarantee that the scene
        is settled before the chunk it applies to is rendered.
    */
    void applyAutomation (int requestedScene, bool isActive) noexcept
    {
        // 0 means "unspecified", not "scene 0" -- an automation lane the user never wrote
        // reads as 0, and that must leave whatever they picked in the editor alone rather
        // than dragging it to the first slot the moment playback starts.
        if (requestedScene != noSceneIndex)
            setActiveScene (requestedScene);

        gateTarget = isActive ? 1.0f : 0.0f;
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

private:
    std::atomic<int> activeScene { defaultSceneIndex };
    std::atomic<int> pendingMirrorScene { -1 };

    double sampleRate = 44100.0;
    int gateRampSamples = 220;
    float gateGain = 1.0f;
    float gateTarget = 1.0f;
};

} // namespace stutter
