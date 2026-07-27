#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestHelpers.h"
#include "FactoryScenes.h"

using namespace stutter;
using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------------------
// Scene schema: parsing, clamping, and the invariants the sequencer depends on.
// ---------------------------------------------------------------------------------------

TEST_CASE ("A malformed scene still yields a usable snapshot", "[schema]")
{
    SECTION ("an empty scene gets defaults")
    {
        const auto s = SceneSchema::sceneFromTree (juce::ValueTree (SceneIDs::scene));
        CHECK (s.beats == 4);
        CHECK (s.divisions == 4);
        CHECK (s.populated);
    }

    SECTION ("out-of-range geometry clamps rather than propagating")
    {
        juce::ValueTree wild (SceneIDs::scene);
        wild.setProperty (SceneIDs::beats, 999, nullptr);
        wild.setProperty (SceneIDs::divisions, -5, nullptr);
        wild.setProperty (SceneIDs::swing, 17.0f, nullptr);
        wild.setProperty (SceneIDs::loopPolicy, 42, nullptr);
        wild.setProperty (SceneIDs::releaseMode, -3, nullptr);

        const auto s = SceneSchema::sceneFromTree (wild);
        CHECK ((s.beats >= 1 && s.beats <= 8));
        CHECK ((s.divisions >= 2 && s.divisions <= 8));
        CHECK ((s.swing >= -1.0f && s.swing <= 1.0f));
        // Cast through int: an out-of-range enum value would be UB to compare directly.
        CHECK (((int) s.loopPolicy >= 0 && (int) s.loopPolicy <= 2));
        CHECK (((int) s.releaseMode >= 0 && (int) s.releaseMode <= 4));
    }

    SECTION ("a lane index past the end is ignored, not written")
    {
        juce::ValueTree sceneTree (SceneIDs::scene);
        juce::ValueTree laneParams (SceneIDs::laneParams);
        juce::ValueTree badLane (SceneIDs::lane);
        badLane.setProperty (SceneIDs::index, 999, nullptr);
        laneParams.appendChild (badLane, nullptr);
        sceneTree.appendChild (laneParams, nullptr);

        const auto s = SceneSchema::sceneFromTree (sceneTree);
        CHECK (s.populated);   // survived without corrupting anything
    }

    SECTION ("an invalid curve target is rejected and not counted as a route")
    {
        juce::ValueTree sceneTree (SceneIDs::scene);
        juce::ValueTree curves (SceneIDs::curvesNode);
        juce::ValueTree badCurve (SceneIDs::curve);
        badCurve.setProperty (SceneIDs::target, 99999, nullptr);
        badCurve.setProperty (SceneIDs::enabled, true, nullptr);
        curves.appendChild (badCurve, nullptr);
        sceneTree.appendChild (curves, nullptr);

        const auto s = SceneSchema::sceneFromTree (sceneTree);
        CHECK ((s.curves[0].targetParam == -1 || isValidParamIndex (s.curves[0].targetParam)));
        CHECK (s.numActiveCurves == 0);
    }
}

TEST_CASE ("Blocks come out sorted and non-overlapping", "[schema][invariant]")
{
    // BlockSequencer advances a forward-only cursor through each lane's blocks, so an
    // unsorted or overlapping array silently breaks playback in a way that is hard to trace
    // back here. sceneFromTree is the only place this invariant is established.
    juce::ValueTree sceneTree (SceneIDs::scene);
    sceneTree.setProperty (SceneIDs::beats, 4, nullptr);
    sceneTree.setProperty (SceneIDs::divisions, 4, nullptr);
    juce::ValueTree blocks (SceneIDs::blocksNode);

    auto addBlock = [&blocks] (int lane, int start, int len)
    {
        juce::ValueTree b (SceneIDs::block);
        b.setProperty (SceneIDs::laneRef, lane, nullptr);
        b.setProperty (SceneIDs::start, start, nullptr);
        b.setProperty (SceneIDs::length, len, nullptr);
        blocks.appendChild (b, nullptr);
    };

    addBlock (0, 12, 2);
    addBlock (0, 0, 4);
    addBlock (0, 2, 4);    // overlaps the previous
    addBlock (0, 8, 2);
    addBlock (0, 5, -3);   // nonsense length
    sceneTree.appendChild (blocks, nullptr);

    const auto s = SceneSchema::sceneFromTree (sceneTree);
    const auto& lane0 = s.lanes[0];

    REQUIRE (lane0.numBlocks >= 2);
    for (int i = 0; i < lane0.numBlocks; ++i)
    {
        const auto& blk = lane0.blocks[(size_t) i];
        CHECK (blk.lengthDiv > 0);
        CHECK (blk.startDiv >= 0);
        CHECK (blk.endDiv() <= s.totalDivisions());

        if (i > 0)
        {
            const auto& prev = lane0.blocks[(size_t) i - 1];
            CHECK (blk.startDiv >= prev.startDiv);      // sorted
            CHECK (blk.startDiv >= prev.endDiv());      // disjoint
        }
    }
}

// ---------------------------------------------------------------------------------------
// Curve baking, including the anti-click point weights.
// ---------------------------------------------------------------------------------------

namespace
{
float maxAdjacentJump (const float* t, int n)
{
    float m = 0.0f;
    for (int i = 1; i < n; ++i)
        m = juce::jmax (m, std::abs (t[(size_t) i] - t[(size_t) i - 1]));
    return m;
}

std::vector<CurvePointV2> stepPoints (PointWeight w)
{
    // A "step": two points at almost the same position. This is the shape the weights exist
    // to tame, and the one that exposed Soft doing nothing at all.
    return { { 0.0f,    0.0f, 0.0f, w },
             { 0.5f,    0.0f, 0.0f, w },
             { 0.5001f, 1.0f, 0.0f, w },
             { 1.0f,    1.0f, 0.0f, w } };
}
} // namespace

TEST_CASE ("Point weights actually reduce the discontinuity they exist for", "[schema][curve]")
{
    std::vector<float> table ((size_t) CurveSnapshot::tableSize);

    SceneSchema::bakeCurveTable (stepPoints (PointWeight::Hard), table.data(), CurveSnapshot::tableSize);
    const float hardJump = maxAdjacentJump (table.data(), CurveSnapshot::tableSize);

    SceneSchema::bakeCurveTable (stepPoints (PointWeight::Medium), table.data(), CurveSnapshot::tableSize);
    const float mediumJump = maxAdjacentJump (table.data(), CurveSnapshot::tableSize);

    SceneSchema::bakeCurveTable (stepPoints (PointWeight::Soft), table.data(), CurveSnapshot::tableSize);
    const float softJump = maxAdjacentJump (table.data(), CurveSnapshot::tableSize);

    INFO ("Hard=" << hardJump << " Medium=" << mediumJump << " Soft=" << softJump);

    // These are an anti-click mechanism; if they do not reduce the jump they are decorative.
    // Soft in particular once measured identical to Hard because it warped t within a
    // segment, and a step has no span to warp.
    CHECK (mediumJump < hardJump * 0.5f);
    CHECK (softJump < hardJump * 0.5f);
    CHECK (softJump <= mediumJump + 1.0e-4f);
}

TEST_CASE ("Smoothing does not destroy an ordinary shape", "[schema][curve]")
{
    std::vector<float> table ((size_t) CurveSnapshot::tableSize);
    const std::vector<CurvePointV2> ramp {
        { 0.0f, 0.0f, 0.0f, PointWeight::Soft },
        { 1.0f, 1.0f, 0.0f, PointWeight::Soft }
    };

    SceneSchema::bakeCurveTable (ramp, table.data(), CurveSnapshot::tableSize);
    CHECK (table[0] < 0.05f);
    CHECK (table[(size_t) CurveSnapshot::tableSize - 1] > 0.95f);
}

TEST_CASE ("Degenerate curve input bakes to something finite", "[schema][curve]")
{
    std::vector<float> table ((size_t) CurveSnapshot::tableSize);

    SECTION ("no points at all")
    {
        SceneSchema::bakeCurveTable ({}, table.data(), CurveSnapshot::tableSize);
        for (int i = 0; i < CurveSnapshot::tableSize; ++i)
            REQUIRE (std::isfinite (table[(size_t) i]));
    }

    SECTION ("a single point bakes flat at its value")
    {
        const std::vector<CurvePointV2> single { { 0.3f, 0.7f, 0.0f, PointWeight::Hard } };
        SceneSchema::bakeCurveTable (single, table.data(), CurveSnapshot::tableSize);
        CHECK_THAT (table[0], WithinAbs (0.7f, 1.0e-4f));
        CHECK_THAT (table[(size_t) CurveSnapshot::tableSize / 2], WithinAbs (0.7f, 1.0e-4f));
    }
}

TEST_CASE ("Tier resolution", "[schema][curve]")
{
    SECTION ("Locked yields a flat pair")
    {
        const auto pts = SceneSchema::pointsForTier (0, 0.42f, 0.0f, 1.0f, false);
        REQUIRE (pts.size() == 2);
        CHECK_THAT (pts[0].value, WithinAbs (0.42f, 1.0e-6f));
        CHECK_THAT (pts[1].value, WithinAbs (0.42f, 1.0e-6f));
    }

    SECTION ("Split ramps, and reverses on request")
    {
        const auto fwd = SceneSchema::pointsForTier (1, 0.0f, 0.2f, 0.9f, false);
        REQUIRE (fwd.size() == 2);
        CHECK (fwd[0].value < fwd[1].value);

        const auto rev = SceneSchema::pointsForTier (1, 0.0f, 0.2f, 0.9f, true);
        REQUIRE (rev.size() == 2);
        CHECK (rev[0].value > rev[1].value);
    }

    SECTION ("Custom defers to the stored points")
    {
        CHECK (SceneSchema::pointsForTier (2, 0.0f, 0.0f, 1.0f, false).empty());
    }
}

// ---------------------------------------------------------------------------------------
// SceneStore lifecycle.
// ---------------------------------------------------------------------------------------

TEST_CASE ("SceneStore publish and retire", "[store]")
{
    SceneStore store;
    CHECK (store.get (0) == nullptr);

    auto bank = SceneStore::createBank();
    bank->scenes[5].populated = true;
    bank->scenes[5].beats = 7;
    store.publish (std::move (bank));

    REQUIRE (store.get (5) != nullptr);
    CHECK (store.get (5)->beats == 7);
    CHECK (store.get (maxScenes) == nullptr);
    CHECK (store.get (-1) == nullptr);
    CHECK (store.getPendingRetireCount() == 0);

    auto bank2 = SceneStore::createBank();
    bank2->scenes[5].populated = true;
    bank2->scenes[5].beats = 3;
    store.publish (std::move (bank2));

    CHECK (store.get (5)->beats == 3);
    CHECK (store.getPendingRetireCount() == 1);

    // Freeing inside the grace window is exactly the use-after-free the queue prevents.
    store.collectGarbage();
    CHECK (store.getPendingRetireCount() == 1);
}

// ---------------------------------------------------------------------------------------
// SceneDocument: the editable side, and undo.
// ---------------------------------------------------------------------------------------

TEST_CASE ("Block editing through the document", "[document]")
{
    SceneStore store;
    juce::UndoManager undo;
    SceneDocument doc (store, undo);

    SECTION ("create, query, remove")
    {
        CHECK_FALSE (doc.hasBlockAt (0, 0, 0));

        doc.addBlock (0, 0, 4, 3);
        CHECK (doc.hasBlockAt (0, 0, 4));
        CHECK (doc.hasBlockAt (0, 0, 6));
        CHECK_FALSE (doc.hasBlockAt (0, 0, 3));
        CHECK_FALSE (doc.hasBlockAt (0, 0, 7));
        CHECK_FALSE (doc.hasBlockAt (0, 1, 4));   // other lanes untouched

        doc.removeBlockAt (0, 0, 5);   // from the middle removes the whole block
        CHECK_FALSE (doc.hasBlockAt (0, 0, 4));
    }

    SECTION ("overlapping edits resolve by replacement")
    {
        // Leaving an overlap would be worse than it sounds: SceneSchema discards overlapping
        // blocks when baking, so the edit would appear to work and then vanish on publish.
        doc.addBlock (0, 2, 0, 4);
        doc.addBlock (0, 2, 2, 4);
        doc.publish();

        const auto* scene = store.get (0);
        REQUIRE (scene != nullptr);
        const auto& lane = scene->lanes[2];
        REQUIRE (lane.numBlocks >= 1);
        for (int i = 1; i < lane.numBlocks; ++i)
        {
            CHECK (lane.blocks[(size_t) i].startDiv >= lane.blocks[(size_t) i - 1].startDiv);
            CHECK (lane.blocks[(size_t) i].startDiv >= lane.blocks[(size_t) i - 1].endDiv());
        }
    }

    SECTION ("a drag is one undo step")
    {
        doc.clearLane (0, 3);
        undo.beginNewTransaction();

        // Several mutations inside one transaction, as a drag produces.
        doc.addBlock (0, 3, 0, 1);
        doc.removeBlockAt (0, 3, 0);
        doc.addBlock (0, 3, 0, 2);
        doc.removeBlockAt (0, 3, 0);
        doc.addBlock (0, 3, 0, 3);
        REQUIRE (doc.hasBlockAt (0, 3, 2));

        undo.undo();
        CHECK_FALSE (doc.hasBlockAt (0, 3, 0));

        undo.redo();
        CHECK (doc.hasBlockAt (0, 3, 2));
    }

    SECTION ("geometry clamps")
    {
        doc.setBeats (0, 99);
        doc.setDivisions (0, -4);
        CHECK ((doc.getBeats (0) >= 1 && doc.getBeats (0) <= 8));
        CHECK ((doc.getDivisions (0) >= 2 && doc.getDivisions (0) <= 8));

        doc.setBeats (0, 4);
        doc.setDivisions (0, 4);
        CHECK (doc.totalDivisions (0) == 16);
    }

    SECTION ("clearLane affects only its lane")
    {
        doc.addBlock (0, 5, 0, 2);
        doc.addBlock (0, 5, 4, 2);
        doc.addBlock (0, 6, 0, 2);

        doc.clearLane (0, 5);
        CHECK_FALSE (doc.hasBlockAt (0, 5, 0));
        CHECK_FALSE (doc.hasBlockAt (0, 5, 4));
        CHECK (doc.hasBlockAt (0, 6, 0));
    }

    SECTION ("publishing makes an edit visible to the audio thread")
    {
        doc.clearLane (0, 7);
        doc.publish();
        const int before = store.get (0)->lanes[7].numBlocks;

        doc.addBlock (0, 7, 8, 4);
        doc.publish();
        CHECK (store.get (0)->lanes[7].numBlocks == before + 1);
    }
}

// ---------------------------------------------------------------------------------------
// Factory scene banks.
// ---------------------------------------------------------------------------------------

TEST_CASE ("Every factory bank parses and bakes", "[presets]")
{
    // Presets are data, and data nothing loads is data nobody notices is broken. A schema
    // change that invalidates one should fail here rather than surface as a silent scene.
    const int numBanks = FactoryScenes::getNumBanks();
    REQUIRE (numBanks > 0);

    for (int b = 0; b < numBanks; ++b)
    {
        const auto name = FactoryScenes::getBankName (b);
        INFO ("bank " << b << " (" << name << ")");

        const auto tree = FactoryScenes::createBank (b);
        REQUIRE (tree.isValid());
        REQUIRE (tree.getNumChildren() > 0);

        SceneStore store;
        store.rebuildFromTree (tree);

        int populated = 0, withBlocks = 0;
        for (int i = 0; i < maxScenes; ++i)
        {
            const auto* s = store.get (i);
            if (s == nullptr || ! s->populated)
                continue;
            ++populated;

            int blocks = 0;
            for (int l = 0; l < maxLanes; ++l)
                blocks += s->lanes[(size_t) l].numBlocks;
            if (blocks > 0)
                ++withBlocks;

            CHECK ((s->beats >= 1 && s->beats <= 8));
            CHECK ((s->divisions >= 2 && s->divisions <= 8));
        }

        CHECK (populated > 0);
        CHECK (withBlocks > 0);
    }
}
