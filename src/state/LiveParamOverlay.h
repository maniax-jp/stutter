#pragma once
#include "../dsp/ParamIndex.h"
#include <atomic>
#include <array>

namespace stutter
{

/**
    Lock-free overlay of scalar parameter edits on top of the published scene bank.

    Why this exists: publishing a bank copies ~10MB, which is fine on mouse-up after a
    structural edit but far too slow to do while a knob is being dragged. So scalar edits --
    every APVTS parameter change, whether it came from the UI or from host automation -- are
    written here instead, and the audio thread prefers an overlay value when one is present:

        value = overlay.isDirty(idx) ? overlay.get(idx) : snapshot.lanes[l].params[p]

    The overlay belongs to exactly one scene. That is the whole point of `overlayScene`:
    without it, dragging a knob in scene 7 and then switching to scene 8 would leak scene 7's
    edit into scene 8, because the dirty flags say nothing about which scene they came from.
    A mismatch invalidates the whole overlay rather than trying to reconcile it -- the edits
    have already been folded into the ValueTree by then, so nothing is lost.

    Threading:
      - get/isDirty/getScene: audio thread, wait-free.
      - set/clear/beginScene: message thread.

    relaxed ordering throughout is sufficient: each slot is an independent scalar, there is no
    invariant spanning two slots, and the worst case is that the audio thread reads a value
    one block later than it was written -- the same tolerance StepSequencer.h:410 already
    documents for its step array.
*/
class LiveParamOverlay
{
public:
    LiveParamOverlay() { clear(); }

    // ---- Audio thread ------------------------------------------------------------------

    /** Whether slot `index` currently overrides the snapshot. Returns false for any index
        when the overlay belongs to a different scene than the one being rendered. */
    bool isDirty (int index, int currentScene) const noexcept
    {
        if (index < 0 || index >= totalParamSlots)
            return false;
        if (overlayScene.load (std::memory_order_relaxed) != currentScene)
            return false;
        return dirty[(size_t) index].load (std::memory_order_relaxed);
    }

    float get (int index) const noexcept
    {
        if (index < 0 || index >= totalParamSlots)
            return 0.0f;
        return values[(size_t) index].load (std::memory_order_relaxed);
    }

    /** Which scene the current overlay entries belong to; -1 when empty. */
    int getScene() const noexcept { return overlayScene.load (std::memory_order_relaxed); }

    // ---- Message thread ----------------------------------------------------------------

    /**
        Point the overlay at `sceneIndex`, discarding any entries from a different scene.

        Called when the active scene changes. Discarding rather than migrating is correct:
        the outgoing scene's edits were already written into the ValueTree, so the only thing
        being dropped is a cache.
    */
    void beginScene (int sceneIndex) noexcept
    {
        if (overlayScene.load (std::memory_order_relaxed) != sceneIndex)
        {
            clear();
            overlayScene.store (sceneIndex, std::memory_order_relaxed);
        }
    }

    /** Override slot `index`. Ignored when the overlay is not pointed at a scene, so a stray
        parameter callback during construction cannot install an entry attributed to nothing. */
    void set (int index, float value) noexcept
    {
        if (index < 0 || index >= totalParamSlots)
            return;
        if (overlayScene.load (std::memory_order_relaxed) < 0)
            return;

        values[(size_t) index].store (value, std::memory_order_relaxed);
        dirty[(size_t) index].store (true, std::memory_order_relaxed);
    }

    /** Drop every entry and detach from any scene. Call after folding the overlay back into
        the ValueTree and republishing, so the snapshot becomes authoritative again. */
    void clear() noexcept
    {
        for (int i = 0; i < totalParamSlots; ++i)
        {
            dirty[(size_t) i].store (false, std::memory_order_relaxed);
            values[(size_t) i].store (0.0f, std::memory_order_relaxed);
        }
        overlayScene.store (-1, std::memory_order_relaxed);
    }

    /** Whether any slot is currently overriding. Lets the folding timer skip work. */
    bool hasAnyDirty() const noexcept
    {
        for (int i = 0; i < totalParamSlots; ++i)
            if (dirty[(size_t) i].load (std::memory_order_relaxed))
                return true;
        return false;
    }

private:
    std::array<std::atomic<float>, totalParamSlots> values {};
    std::array<std::atomic<bool>, totalParamSlots> dirty {};
    std::atomic<int> overlayScene { -1 };
};

} // namespace stutter
