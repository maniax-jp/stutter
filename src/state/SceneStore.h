#pragma once
#include "SceneSnapshot.h"
#include <juce_data_structures/juce_data_structures.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace stutter
{

/**
    Owns the bank of scene snapshots and publishes it to the audio thread.

    The problem this solves: a MIDI note arriving on the audio thread selects a scene, but the
    scene data is built on the message thread from a ValueTree. The audio thread must never
    see a half-built scene, must never allocate, and must never block -- and the message thread
    must never free a bank the audio thread is still reading.

    The mechanism is the one already proven in this codebase by CurveModulator::bakeTable
    (CurveModulator.h:234-244): build into an inactive buffer, then publish with a single
    release store that the reader picks up with an acquire load. This generalises it from one
    1024-entry table to the whole 128-scene bank.

    Why swap the entire bank rather than individual scenes: editing scene 5 while scene 3
    plays would otherwise need a per-scene atomic and a 128-entry retire list. One atomic and
    one retire queue is a much smaller thing to get right. The cost is that a structural edit
    rebuilds ~10MB, which is why scalar knob edits go through LiveParamOverlay instead (see
    LiveParamOverlay.h) and never touch this path.

    Threading contract:
      - get() / getActive(): audio thread, wait-free, no allocation.
      - rebuildFromTree() / publish(): message thread (or whatever thread the host calls
        setStateInformation on -- JUCE does not promise those are the same, hence the mutex,
        which only ever serialises writers against each other).
      - collectGarbage(): message thread, on a timer.
*/
class SceneStore
{
public:
    /** A complete bank of scenes. Heap-allocated and published by pointer; never copied
        onto the audio thread. */
    struct Bank
    {
        std::array<SceneSnapshot, maxScenes> scenes {};
    };

    SceneStore() = default;
    ~SceneStore();

    /** Build an empty bank the caller can fill before publishing. */
    static std::unique_ptr<Bank> createBank() { return std::make_unique<Bank>(); }

    // ---- Audio thread ------------------------------------------------------------------

    /**
        Return a scene, or nullptr if the index is out of range or nothing is published yet.

        Wait-free. The returned pointer stays valid for the duration of the current
        processBlock: retired banks are held until collectGarbage() runs on the message
        thread, at least retireGraceMillis after being replaced, and the audio thread never
        holds a pointer across a block boundary.
    */
    const SceneSnapshot* get (int sceneIndex) const noexcept
    {
        const Bank* b = live.load (std::memory_order_acquire);
        if (b == nullptr || sceneIndex < 0 || sceneIndex >= maxScenes)
            return nullptr;
        return &b->scenes[(size_t) sceneIndex];
    }

    /** The whole published bank, or nullptr if nothing is published. Prefer get(). */
    const SceneSnapshot* getBankScenes() const noexcept
    {
        const Bank* b = live.load (std::memory_order_acquire);
        return b != nullptr ? b->scenes.data() : nullptr;
    }

    // ---- Message thread ----------------------------------------------------------------

    /**
        Rebuild the whole bank from a v2 <Scenes> tree and publish it atomically.

        Missing, malformed, or out-of-range entries fall back to a default-constructed scene
        with populated=false rather than being skipped, so the bank always has exactly
        maxScenes entries and the audio thread never needs a bounds check beyond get()'s.
    */
    void rebuildFromTree (const juce::ValueTree& scenesTree);

    /** Publish a bank built by the caller. Takes ownership; the previously live bank is
        retired rather than freed immediately. */
    void publish (std::unique_ptr<Bank> bank);

    /**
        Free banks retired long enough ago that no processBlock can still be reading them.

        Call from a message-thread timer. Deliberately time-based rather than reference
        counted: a refcount would put an atomic increment on the audio thread's read path,
        and the audio thread already never holds a snapshot pointer beyond one block, so a
        grace period an order of magnitude longer than any plausible block is sufficient.
    */
    void collectGarbage();

    /** Number of banks awaiting collection. Tests assert this returns to zero. */
    int getPendingRetireCount() const;

private:
    struct RetiredBank
    {
        std::unique_ptr<Bank> bank;
        juce::int64 retiredAtMs = 0;
    };

    /** How long a retired bank is kept before being freed. 2 seconds is far longer than any
        block at any sane buffer size, and the memory is reclaimed either way. */
    static constexpr juce::int64 retireGraceMillis = 2000;

    std::atomic<const Bank*> live { nullptr };

    /** Owns whatever `live` points at. Message thread only. */
    std::unique_ptr<Bank> liveOwned;

    std::vector<RetiredBank> retired;

    /** Serialises writers (message thread vs. the host's setStateInformation thread) against
        each other. The audio thread never touches it, so it cannot affect RT safety. */
    mutable std::mutex writeMutex;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SceneStore)
};

} // namespace stutter
