#pragma once
#include "SceneSchema.h"
#include "SceneStore.h"
#include <juce_data_structures/juce_data_structures.h>

namespace stutter
{

/**
    The editable side of the scene state.

    SceneStore holds baked, immutable snapshots for the audio thread. This holds the
    ValueTree those snapshots are baked FROM, and is what the UI mutates. Keeping the two
    apart is what lets the audio thread read without locking: the editor never touches a
    snapshot, and the audio thread never touches a tree.

    Every mutation goes through juce::UndoManager. That is a deliberate improvement over v1,
    where the step grid lived in a std::atomic<bool> array outside the tree and could not be
    undone at all (StepSequencer.h:487). Here a block drag is one undo step, because the
    caller opens a transaction on mouse-down and every intermediate move joins it.

    Republishing is explicit rather than automatic on every property change: a drag would
    otherwise rebuild the ~10MB bank on every mouse-move. Call publish() on mouse-up.
*/
class SceneDocument
{
public:
    SceneDocument (SceneStore& storeToFill, juce::UndoManager& undoManagerToUse)
        : store (storeToFill), undoManager (undoManagerToUse)
    {
        state = juce::ValueTree (SceneIDs::scenesNode);
        // Seed the scene the plugin actually opens on, not slot 0. The sceneSelect parameter
        // defaults to defaultSceneIndex and the audio path follows it, so seeding a different
        // slot would leave a fresh instance pointing at a scene that does not exist.
        ensureScene (defaultSceneIndex);
        publish();
    }

    juce::ValueTree& getState() noexcept { return state; }
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }

    /** Replace the whole document, e.g. on preset load. Not undoable -- loading a preset is
        a jump, not an edit, and mixing the two in one history confuses more than it helps. */
    void replaceState (const juce::ValueTree& newState)
    {
        if (! newState.isValid())
            return;
        state = newState.createCopy();
        publish();
    }

    /** Bake every scene and hand the bank to the audio thread. */
    void publish()
    {
        store.rebuildFromTree (state);
    }

    /**
        The <Scene> node for `index`, or an invalid tree when that slot has never been used.

        Use this from anything that only reads -- painting, hit-testing, populating a panel.
        ensureScene() below *creates* the node as a side effect, so calling it from a paint()
        materialises every slot the component happens to draw: the browser paints 24 cells at
        30Hz, which used to conjure 24 empty scenes the moment a preset loaded. Those scenes
        then counted as real, which made the mirror write a full set of default lane values
        into APVTS (marking a freshly loaded preset as edited) and made the sequencer treat an
        empty slot as playable.
    */
    juce::ValueTree findScene (int index) const
    {
        if (index < 0 || index >= maxScenes)
            return {};

        for (int i = 0; i < state.getNumChildren(); ++i)
        {
            const auto child = state.getChild (i);
            if (child.hasType (SceneIDs::scene)
                && (int) child.getProperty (SceneIDs::index, -1) == index)
                return child;
        }

        return {};
    }

    /** The <Scene> node for `index`, creating it if absent. Callers that only read should use
        findScene() -- see the warning there. */
    juce::ValueTree ensureScene (int index)
    {
        if (index < 0 || index >= maxScenes)
            return {};

        if (auto existing = findScene (index); existing.isValid())
            return existing;

        juce::ValueTree scene (SceneIDs::scene);
        scene.setProperty (SceneIDs::index, index, nullptr);
        scene.setProperty (SceneIDs::beats, 4, nullptr);
        scene.setProperty (SceneIDs::divisions, 4, nullptr);
        scene.setProperty (SceneIDs::swing, 0.0f, nullptr);
        scene.appendChild (juce::ValueTree (SceneIDs::blocksNode), nullptr);
        scene.appendChild (juce::ValueTree (SceneIDs::laneParams), nullptr);
        scene.appendChild (juce::ValueTree (SceneIDs::curvesNode), nullptr);

        // Not undoable: creating the backing node for a scene the user is about to edit is
        // bookkeeping, and putting it in the history would make the first edit take two
        // undos to reverse.
        state.appendChild (scene, nullptr);
        return scene;
    }

    // ---- Block editing -----------------------------------------------------------------

    /** Add a block, or extend/merge if it touches an existing one. Returns false when the
        lane is full or the request is degenerate. */
    bool addBlock (int sceneIndex, int lane, int startDiv, int lengthDiv)
    {
        if (lane < 0 || lane >= maxLanes || lengthDiv <= 0 || startDiv < 0)
            return false;

        auto scene = ensureScene (sceneIndex);
        if (! scene.isValid())
            return false;

        auto blocks = scene.getOrCreateChildWithName (SceneIDs::blocksNode, nullptr);

        // Remove anything this block overlaps; the new block wins. Dragging over an existing
        // block should replace it, not silently fail or produce an invalid overlap that
        // SceneSchema would then have to discard.
        removeOverlapping (blocks, lane, startDiv, startDiv + lengthDiv);

        if (countBlocksInLane (blocks, lane) >= maxBlocksPerLane)
            return false;

        juce::ValueTree b (SceneIDs::block);
        b.setProperty (SceneIDs::laneRef, lane, nullptr);
        b.setProperty (SceneIDs::start, startDiv, nullptr);
        b.setProperty (SceneIDs::length, lengthDiv, nullptr);
        blocks.appendChild (b, &undoManager);
        return true;
    }

    /** Remove whatever block covers `divIndex` in `lane`. Returns true if one was removed. */
    bool removeBlockAt (int sceneIndex, int lane, int divIndex)
    {
        auto scene = ensureScene (sceneIndex);
        if (! scene.isValid())
            return false;

        auto blocks = scene.getChildWithName (SceneIDs::blocksNode);
        if (! blocks.isValid())
            return false;

        for (int i = blocks.getNumChildren(); --i >= 0;)
        {
            const auto b = blocks.getChild (i);
            if ((int) b.getProperty (SceneIDs::laneRef, -1) != lane)
                continue;

            const int s = (int) b.getProperty (SceneIDs::start, 0);
            const int len = (int) b.getProperty (SceneIDs::length, 1);
            if (divIndex >= s && divIndex < s + len)
            {
                blocks.removeChild (i, &undoManager);
                return true;
            }
        }
        return false;
    }

    /** True when a block covers `divIndex`. */
    bool hasBlockAt (int sceneIndex, int lane, int divIndex)
    {
        auto scene = ensureScene (sceneIndex);
        if (! scene.isValid())
            return false;
        auto blocks = scene.getChildWithName (SceneIDs::blocksNode);
        if (! blocks.isValid())
            return false;

        for (int i = 0; i < blocks.getNumChildren(); ++i)
        {
            const auto b = blocks.getChild (i);
            if ((int) b.getProperty (SceneIDs::laneRef, -1) != lane)
                continue;
            const int s = (int) b.getProperty (SceneIDs::start, 0);
            const int len = (int) b.getProperty (SceneIDs::length, 1);
            if (divIndex >= s && divIndex < s + len)
                return true;
        }
        return false;
    }

    /** Delete every block in a lane. */
    void clearLane (int sceneIndex, int lane)
    {
        auto scene = ensureScene (sceneIndex);
        if (! scene.isValid())
            return;
        auto blocks = scene.getChildWithName (SceneIDs::blocksNode);
        if (! blocks.isValid())
            return;

        for (int i = blocks.getNumChildren(); --i >= 0;)
            if ((int) blocks.getChild (i).getProperty (SceneIDs::laneRef, -1) == lane)
                blocks.removeChild (i, &undoManager);
    }

    // ---- Scene geometry ------------------------------------------------------------------

    // Read-only, and deliberately so: these are called from paint() by way of
    // totalDivisions(), so reaching for ensureScene here would materialise a scene for every
    // slot the grid happens to draw. An absent scene reports the defaults, which is what an
    // empty slot would have been created with anyway.
    int getBeats (int sceneIndex) const
    {
        auto s = findScene (sceneIndex);
        return s.isValid() ? juce::jlimit (1, 8, (int) s.getProperty (SceneIDs::beats, 4)) : 4;
    }

    int getDivisions (int sceneIndex) const
    {
        auto s = findScene (sceneIndex);
        return s.isValid() ? juce::jlimit (2, 8, (int) s.getProperty (SceneIDs::divisions, 4)) : 4;
    }

    void setBeats (int sceneIndex, int beats)
    {
        auto s = ensureScene (sceneIndex);
        if (s.isValid())
            s.setProperty (SceneIDs::beats, juce::jlimit (1, 8, beats), &undoManager);
    }

    void setDivisions (int sceneIndex, int divisions)
    {
        auto s = ensureScene (sceneIndex);
        if (s.isValid())
            s.setProperty (SceneIDs::divisions, juce::jlimit (2, 8, divisions), &undoManager);
    }

    void setSwing (int sceneIndex, float swing)
    {
        auto s = ensureScene (sceneIndex);
        if (s.isValid())
            s.setProperty (SceneIDs::swing, juce::jlimit (-1.0f, 1.0f, swing), &undoManager);
    }

    int totalDivisions (int sceneIndex) const
    {
        return getBeats (sceneIndex) * getDivisions (sceneIndex);
    }

    /** Per-lane mute/solo. Stored on the scene's <Lane> node beside its parameters, so each
        scene keeps its own mix -- muting a lane to audition one key does not silence it
        everywhere. */
    bool isLaneMuted (int sceneIndex, int lane) { return laneFlag (sceneIndex, lane, SceneIDs::mute); }
    bool isLaneSoloed (int sceneIndex, int lane) { return laneFlag (sceneIndex, lane, SceneIDs::solo); }

    void toggleLaneMute (int sceneIndex, int lane)
    {
        setLaneFlag (sceneIndex, lane, SceneIDs::mute, ! isLaneMuted (sceneIndex, lane));
    }

    void toggleLaneSolo (int sceneIndex, int lane)
    {
        setLaneFlag (sceneIndex, lane, SceneIDs::solo, ! isLaneSoloed (sceneIndex, lane));
    }

private:
    /** The scene's <Lane> node for `lane`, created if absent. Shared by the flag accessors so
        they agree with mirrorActiveSceneToApvts about where lane state lives.

        createIfMissing governs the scene as well as the lane: a reader must not conjure either,
        since the flag accessors below run from paint(). */
    juce::ValueTree laneNode (int sceneIndex, int lane, bool createIfMissing)
    {
        auto scene = createIfMissing ? ensureScene (sceneIndex) : findScene (sceneIndex);
        if (! scene.isValid() || lane < 0 || lane >= maxLanes)
            return {};

        auto laneParams = createIfMissing
                            ? scene.getOrCreateChildWithName (SceneIDs::laneParams, nullptr)
                            : scene.getChildWithName (SceneIDs::laneParams);
        if (! laneParams.isValid())
            return {};

        for (int i = 0; i < laneParams.getNumChildren(); ++i)
            if ((int) laneParams.getChild (i).getProperty (SceneIDs::index, -1) == lane)
                return laneParams.getChild (i);

        if (! createIfMissing)
            return {};

        juce::ValueTree n (SceneIDs::lane);
        n.setProperty (SceneIDs::index, lane, nullptr);
        laneParams.appendChild (n, nullptr);
        return n;
    }

    bool laneFlag (int sceneIndex, int lane, const juce::Identifier& id)
    {
        auto n = laneNode (sceneIndex, lane, false);
        return n.isValid() && (bool) n.getProperty (id, false);
    }

    void setLaneFlag (int sceneIndex, int lane, const juce::Identifier& id, bool value)
    {
        auto n = laneNode (sceneIndex, lane, true);
        if (n.isValid())
            n.setProperty (id, value, &undoManager);
    }

    static int countBlocksInLane (const juce::ValueTree& blocks, int lane)
    {
        int n = 0;
        for (int i = 0; i < blocks.getNumChildren(); ++i)
            if ((int) blocks.getChild (i).getProperty (SceneIDs::laneRef, -1) == lane)
                ++n;
        return n;
    }

    void removeOverlapping (juce::ValueTree& blocks, int lane, int from, int to)
    {
        for (int i = blocks.getNumChildren(); --i >= 0;)
        {
            const auto b = blocks.getChild (i);
            if ((int) b.getProperty (SceneIDs::laneRef, -1) != lane)
                continue;

            const int s = (int) b.getProperty (SceneIDs::start, 0);
            const int e = s + (int) b.getProperty (SceneIDs::length, 1);
            if (s < to && e > from)
                blocks.removeChild (i, &undoManager);
        }
    }

    SceneStore& store;
    juce::UndoManager& undoManager;
    juce::ValueTree state;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SceneDocument)
};

} // namespace stutter
