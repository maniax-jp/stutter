#include "FactoryScenes.h"
#include "dsp/ParamIndex.h"
#include "dsp/ParameterIDs.h"

namespace stutter
{
namespace
{

/** Append a block to a scene's <Blocks> node. */
void addBlock (juce::ValueTree& scene, int lane, int start, int length)
{
    auto blocks = scene.getOrCreateChildWithName (SceneIDs::blocksNode, nullptr);
    juce::ValueTree b (SceneIDs::block);
    b.setProperty (SceneIDs::laneRef, lane, nullptr);
    b.setProperty (SceneIDs::start, start, nullptr);
    b.setProperty (SceneIDs::length, length, nullptr);
    blocks.appendChild (b, nullptr);
}

/** Set one of a lane's parameters by descriptor position. */
void setLaneParam (juce::ValueTree& scene, int lane, int paramIdx, float value)
{
    auto laneParams = scene.getOrCreateChildWithName (SceneIDs::laneParams, nullptr);

    juce::ValueTree laneNode;
    for (int i = 0; i < laneParams.getNumChildren(); ++i)
        if ((int) laneParams.getChild (i).getProperty (SceneIDs::index, -1) == lane)
            laneNode = laneParams.getChild (i);

    if (! laneNode.isValid())
    {
        laneNode = juce::ValueTree (SceneIDs::lane);
        laneNode.setProperty (SceneIDs::index, lane, nullptr);
        laneParams.appendChild (laneNode, nullptr);
    }

    juce::ValueTree p (SceneIDs::param);
    p.setProperty (SceneIDs::paramIndexProp, paramIdx, nullptr);
    p.setProperty (SceneIDs::value, value, nullptr);
    laneNode.appendChild (p, nullptr);
}

/** Route a curve to a parameter, with breakpoints. */
void addCurve (juce::ValueTree& scene, int targetParam, float depth, float speed,
               const std::vector<CurvePointV2>& points)
{
    auto curves = scene.getOrCreateChildWithName (SceneIDs::curvesNode, nullptr);

    juce::ValueTree c (SceneIDs::curve);
    c.setProperty (SceneIDs::target, targetParam, nullptr);
    c.setProperty (SceneIDs::depth, depth, nullptr);
    c.setProperty (SceneIDs::speed, speed, nullptr);
    c.setProperty (SceneIDs::enabled, true, nullptr);
    c.setProperty (SceneIDs::tier, 2, nullptr);   // Custom: use the stored points

    for (const auto& pt : points)
    {
        juce::ValueTree p (SceneIDs::point);
        p.setProperty (SceneIDs::position, pt.position, nullptr);
        p.setProperty (SceneIDs::value, pt.value, nullptr);
        p.setProperty (SceneIDs::curvature, pt.curvature, nullptr);
        p.setProperty (SceneIDs::pointWeight, (int) pt.weight, nullptr);
        c.appendChild (p, nullptr);
    }

    curves.appendChild (c, nullptr);
}

juce::ValueTree makeScene (int index, const juce::String& name, int beats, int divisions,
                           float swing, juce::uint32 seed)
{
    juce::ValueTree s (SceneIDs::scene);
    s.setProperty (SceneIDs::index, index, nullptr);
    s.setProperty (SceneIDs::name, name, nullptr);
    s.setProperty (SceneIDs::beats, beats, nullptr);
    s.setProperty (SceneIDs::divisions, divisions, nullptr);
    s.setProperty (SceneIDs::swing, swing, nullptr);
    s.setProperty (SceneIDs::seed, (int) seed, nullptr);
    return s;
}

// ---- Bank 0: Held Envelopes ----------------------------------------------------------
//
// The case v1 structurally could not make. Its grid restarted TapeStop every 16th, so the
// deceleration never completed; here each scene holds one block long enough for the envelope
// to actually arrive somewhere.
juce::ValueTree makeHeldEnvelopesBank()
{
    juce::ValueTree bank (SceneIDs::scenesNode);

    {
        auto s = makeScene (60, "Full Bar Brake", 4, 4, 0.0f, 1001);
        addBlock (s, lanes::tapeStop, 0, 16);      // one block across the whole bar
        setLaneParam (s, lanes::tapeStop, 0, 0.7f);  // curve: mostly exponential
        setLaneParam (s, lanes::tapeStop, 1, 0.9f);  // time: long
        bank.appendChild (s, nullptr);
    }
    {
        auto s = makeScene (61, "Half Brake, Half Spin", 4, 4, 0.0f, 1002);
        addBlock (s, lanes::tapeStop, 0, 8);
        addBlock (s, lanes::tapeStart, 8, 8);
        bank.appendChild (s, nullptr);
    }
    {
        auto s = makeScene (62, "Slow Grind", 4, 4, 0.0f, 1003);
        addBlock (s, lanes::stretcher, 0, 16);
        setLaneParam (s, lanes::stretcher, 0, 0.25f);  // speed: quarter time
        setLaneParam (s, lanes::stretcher, 1, 0.12f);  // grain: long, less metallic
        setLaneParam (s, lanes::stretcher, 2, 0.3f);   // jitter: mask the resonance
        bank.appendChild (s, nullptr);
    }
    return bank;
}

// ---- Bank 1: Swing and Odd Grids ------------------------------------------------------
//
// Beats x divisions and swing, neither of which v1 had. Divisions of 3 and 6 give triplet
// and polyrhythmic feels without a separate triplet rate table.
juce::ValueTree makeGrooveBank()
{
    juce::ValueTree bank (SceneIDs::scenesNode);

    {
        auto s = makeScene (60, "Swung Sixteenths", 4, 4, 0.55f, 2001);
        for (int d = 0; d < 16; d += 2)
            addBlock (s, lanes::gate, d, 1);
        bank.appendChild (s, nullptr);
    }
    {
        auto s = makeScene (61, "Triplet Chop", 4, 3, 0.0f, 2002);
        for (int d = 0; d < 12; d += 3)
            addBlock (s, lanes::stutterLane, d, 2);
        setLaneParam (s, lanes::stutterLane, 0, 2.0f);   // 1/64 in corrected labelling
        bank.appendChild (s, nullptr);
    }
    {
        auto s = makeScene (62, "Five Over Four", 5, 4, 0.0f, 2003);
        for (int d = 0; d < 20; d += 5)
            addBlock (s, lanes::reverse, d, 2);
        bank.appendChild (s, nullptr);
    }
    return bank;
}

// ---- Bank 2: Routed Modulation ---------------------------------------------------------
//
// Curves driving parameters v1 had no way to reach. v1's three curves were welded to
// Volume/Filter/Pan; these target lane parameters directly.
juce::ValueTree makeModulationBank()
{
    juce::ValueTree bank (SceneIDs::scenesNode);

    {
        auto s = makeScene (60, "Filter Sweep Hold", 4, 4, 0.0f, 3001);
        addBlock (s, lanes::filter, 0, 16);
        setLaneParam (s, lanes::filter, 0, 0.0f);    // lowpass
        setLaneParam (s, lanes::filter, 2, 0.6f);    // resonance
        // Cutoff (param 1) swept across the bar -- the parameter is marked continuous
        // precisely so a curve can do this.
        addCurve (s, paramIndex (lanes::filter, 1), 1.0f, 1.0f,
                  { { 0.0f, 0.05f, 0.0f, PointWeight::Soft },
                    { 0.5f, 0.9f,  0.3f, PointWeight::Soft },
                    { 1.0f, 0.05f, -0.3f, PointWeight::Soft } });
        bank.appendChild (s, nullptr);
    }
    {
        auto s = makeScene (61, "Crush Ramp", 4, 4, 0.0f, 3002);
        addBlock (s, lanes::crush, 0, 16);
        addCurve (s, paramIndex (lanes::crush, 0), 1.0f, 2.0f,
                  { { 0.0f, 1.0f, 0.0f, PointWeight::Hard },
                    { 1.0f, 0.1f, 0.0f, PointWeight::Hard } });
        bank.appendChild (s, nullptr);
    }
    {
        auto s = makeScene (62, "Drive Pulse", 4, 4, 0.0f, 3003);
        addBlock (s, lanes::distort, 0, 16);
        setLaneParam (s, lanes::distort, 0, 2.0f);   // Fold
        addCurve (s, paramIndex (lanes::distort, 1), 1.0f, 4.0f,
                  { { 0.0f, 0.1f, 0.0f, PointWeight::Medium },
                    { 0.3f, 1.0f, 0.0f, PointWeight::Medium },
                    { 1.0f, 0.1f, 0.0f, PointWeight::Medium } });
        bank.appendChild (s, nullptr);
    }
    return bank;
}

// ---- Bank 3: Playable Set ----------------------------------------------------------------
//
// Laid out for performance: adjacent notes give escalating intensity, so a player can sweep
// up the keyboard. This is the bank that demonstrates the MIDI layer.
juce::ValueTree makePlayableBank()
{
    juce::ValueTree bank (SceneIDs::scenesNode);

    {
        auto s = makeScene (60, "C - Light Gate", 4, 4, 0.0f, 4001);
        for (int d = 0; d < 16; d += 4)
            addBlock (s, lanes::gate, d, 1);
        bank.appendChild (s, nullptr);
    }
    {
        auto s = makeScene (62, "D - Stutter Roll", 4, 4, 0.0f, 4002);
        addBlock (s, lanes::stutterLane, 8, 8);
        setLaneParam (s, lanes::stutterLane, 1, 0.6f);   // decay: shrink toward a roll
        bank.appendChild (s, nullptr);
    }
    {
        auto s = makeScene (64, "E - Shuffle Chop", 4, 4, 0.0f, 4003);
        addBlock (s, lanes::shuffler, 0, 16);
        setLaneParam (s, lanes::shuffler, 2, 0.85f);  // shuffle chance
        setLaneParam (s, lanes::shuffler, 4, 0.4f);   // reverse chance
        bank.appendChild (s, nullptr);
    }
    {
        auto s = makeScene (65, "F - Dub Throw", 4, 4, 0.0f, 4004);
        addBlock (s, lanes::delay, 0, 16);
        setLaneParam (s, lanes::delay, 1, 0.75f);  // feedback
        setLaneParam (s, lanes::delay, 4, 0.1f);   // slew: low, so the tail warps
        bank.appendChild (s, nullptr);
    }
    {
        auto s = makeScene (67, "G - Total Collapse", 4, 4, 0.0f, 4005);
        addBlock (s, lanes::tapeStop, 0, 16);
        addBlock (s, lanes::distort, 0, 16);
        setLaneParam (s, lanes::distort, 1, 12.0f);
        bank.appendChild (s, nullptr);
    }
    return bank;
}

struct BankDef
{
    const char* name;
    juce::ValueTree (*build)();
};

const BankDef banks[] = {
    { "Held Envelopes",   &makeHeldEnvelopesBank },
    { "Swing & Odd Grids", &makeGrooveBank },
    { "Routed Modulation", &makeModulationBank },
    { "Playable Set",     &makePlayableBank },
};

} // namespace

int FactoryScenes::getNumBanks()
{
    return (int) (sizeof (banks) / sizeof (banks[0]));
}

juce::String FactoryScenes::getBankName (int index)
{
    if (index < 0 || index >= getNumBanks())
        return {};
    return banks[index].name;
}

juce::ValueTree FactoryScenes::createBank (int index)
{
    if (index < 0 || index >= getNumBanks())
        return {};
    return banks[index].build();
}

} // namespace stutter
