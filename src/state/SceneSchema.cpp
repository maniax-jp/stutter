#include "SceneSchema.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <array>

namespace stutter
{

/** Registered per-lane defaults; see SceneSchema::setLaneDefaults. Written once at startup
    from the message thread and read during baking (also message thread), so no
    synchronisation is needed -- the audio thread never touches this, it only ever sees the
    baked snapshot. */
static std::array<std::array<float, maxParamsPerLane>, maxLanes> g_laneDefaults {};
static std::array<bool, maxLanes> g_laneDefaultsSet {};

/** Registered per-lane parameter ranges; see SceneSchema::setLaneRanges. Same lifetime and
    threading story as the defaults above: written once at startup, read-only afterwards. */
static std::array<std::array<float, maxParamsPerLane>, maxLanes> g_laneMin {};
static std::array<std::array<float, maxParamsPerLane>, maxLanes> g_laneMax {};
static std::array<bool, maxLanes> g_laneRangesSet {};

static float smoothstepFn (float t) noexcept
{
    return t * t * (3.0f - 2.0f * t);
}

static float clamp (float v, float lo, float hi) noexcept
{
    return std::max (lo, std::min (hi, v));
}

// Helper: read an int property with a default.
//
// Accepts strings as well as numbers, and that is not defensive padding: juce::ValueTree's
// XML round-trip stores every property as an attribute and reads it back as a *string*, so
// an int written in memory returns as "16" after a host save/reload. Testing only isInt()
// therefore made every integer in a saved scene -- lane, start, length, beats, divisions --
// silently revert to its default, which read as "the blocks moved to lane 0 and overlapped"
// rather than as a parse failure.
static int getPropInt (const juce::ValueTree& tree, const juce::Identifier& id, int def)
{
    const auto v = tree.getProperty (id, juce::var (def));
    if (v.isInt() || v.isInt64() || v.isDouble()) return static_cast<int> (v);
    if (v.isString())
    {
        const auto s = v.toString().trim();
        if (s.containsOnly ("0123456789+-") && s.isNotEmpty())
            return s.getIntValue();
    }
    return def;
}

// Helper: read a float property with a default.
//
// Accepts int as well as double: juce::var stores a whole-numbered float (a swing of exactly
// 0, a depth of exactly 1) as an int, so testing only isDouble would silently discard those
// values and fall back to the default. That is a real hazard here because the neutral values
// -- the ones most likely to be written -- are precisely the whole numbers.
static float getPropFloat (const juce::ValueTree& tree, const juce::Identifier& id, float def)
{
    const auto v = tree.getProperty (id, juce::var (def));
    if (v.isDouble() || v.isInt() || v.isInt64()) return static_cast<float> (v);
    // Strings arrive after an XML round-trip; see getPropInt.
    if (v.isString())
    {
        const auto s = v.toString().trim();
        if (s.containsOnly ("0123456789+-.eE") && s.isNotEmpty())
            return static_cast<float> (s.getDoubleValue());
    }
    return def;
}

// Helper: read a bool property.
static bool getPropBool (const juce::ValueTree& tree, const juce::Identifier& id, bool def)
{
    const auto v = tree.getProperty (id, juce::var (def));
    if (v.isBool() || v.isInt() || v.isInt64()) return static_cast<bool> (v);
    // Strings arrive after an XML round-trip; see getPropInt. JUCE writes bools as "1"/"0",
    // so accept those spellings as well as the textual ones.
    if (v.isString())
    {
        const auto s = v.toString().trim().toLowerCase();
        if (s == "1" || s == "true")  return true;
        if (s == "0" || s == "false") return false;
    }
    return def;
}

/* -------------------------------------------------------------------------- */
/*  bakeCurveTable                                                             */
/* -------------------------------------------------------------------------- */

void SceneSchema::bakeCurveTable (const std::vector<CurvePointV2>& points,
                                  float* table, int tableSize)
{
    if (tableSize <= 0) return;

    if (points.empty())
    {
        for (int i = 0; i < tableSize; ++i)
            table[i] = 0.5f;
        return;
    }
    if (points.size() == 1)
    {
        const float fill = points[0].value;
        for (int i = 0; i < tableSize; ++i)
            table[i] = fill;
        return;
    }

    std::vector<CurvePointV2> pts = points;
    std::sort (pts.begin(), pts.end(),
               [] (const CurvePointV2& a, const CurvePointV2& b)
               { return a.position < b.position; });

    for (auto& p : pts)
        p.position = clamp (p.position, 0.0f, 1.0f);

    if (pts.back().position < 1.0f)
    {
        CurvePointV2 endPt;
        endPt.position = 1.0f;
        endPt.value = pts.back().value;
        endPt.curvature = pts.back().curvature;
        endPt.weight = pts.back().weight;
        pts.push_back (endPt);
    }

    const int numPts = static_cast<int> (pts.size());

    for (int i = 0; i < tableSize; ++i)
    {
        const float phase = static_cast<float> (i) / static_cast<float> (tableSize - 1);

        int segIdx = 0;
        for (int j = 0; j < numPts - 1; ++j)
        {
            if (phase >= pts[(size_t) j].position && phase <= pts[(size_t) j + 1].position)
            {
                segIdx = j;
                break;
            }
            segIdx = j;
        }
        if (segIdx >= numPts - 1)
            segIdx = numPts - 2;

        const auto& a = pts[(size_t) segIdx];
        const auto& b = pts[(size_t) segIdx + 1];

        const float span = b.position - a.position;
        float t = span > 1.0e-6f ? (phase - a.position) / span : 0.0f;
        t = clamp (t, 0.0f, 1.0f);

        switch (a.weight)
        {
            case PointWeight::Hard:
            {
                const float c = a.curvature;
                float shapedT = t;
                if (std::abs (c) > 1.0e-3f)
                {
                    const float exponent = std::pow (10.0f, -c);
                    shapedT = std::pow (t, exponent);
                }
                table[i] = a.value + shapedT * (b.value - a.value);
                break;
            }

            case PointWeight::Medium:
            {
                const float blendRange = 0.01f;
                const float c = a.curvature;
                float shapedT = t;
                if (std::abs (c) > 1.0e-3f)
                {
                    const float exponent = std::pow (10.0f, -c);
                    shapedT = std::pow (t, exponent);
                }
                const float segValue = a.value + shapedT * (b.value - a.value);

                if (t < blendRange)
                {
                    float prevVal = a.value;
                    if (segIdx > 0)
                        prevVal = pts[(size_t) segIdx - 1].value;
                    const float st = smoothstepFn (t / blendRange);
                    table[i] = prevVal + st * (segValue - prevVal);
                }
                else if (t > 1.0f - blendRange)
                {
                    const float localT = (t - (1.0f - blendRange)) / blendRange;
                    const float st = smoothstepFn (localT);
                    table[i] = segValue + st * (b.value - segValue);
                }
                else
                {
                    table[i] = segValue;
                }
                break;
            }

            case PointWeight::Soft:
            default:
            {
                // Soft eases within the segment AND is smoothed across point boundaries by
                // the post-pass below. The in-segment easing alone is not enough: a "step"
                // is drawn as two points at almost the same position, so there is no span
                // to ease over and the jump survives untouched. Measured on a hard step,
                // in-segment easing alone left the max adjacent jump at 1.0 -- identical to
                // Hard, i.e. the smoothest weight was the one that clicked hardest.
                t = smoothstepFn (t);
                const float c = a.curvature;
                float shapedT = t;
                if (std::abs (c) > 1.0e-3f)
                {
                    const float exponent = std::pow (10.0f, -c);
                    shapedT = std::pow (t, exponent);
                }
                table[i] = a.value + shapedT * (b.value - a.value);
                break;
            }
        }
    }

    // Soft post-pass: box-blur the baked table across any region governed by a Soft point.
    //
    // This is what makes Soft actually soft. Blurring the *table* rather than warping the
    // parameterisation rounds a corner regardless of how the points that formed it are
    // arranged, which is the whole point of the weight. A window of ~2% of the cycle takes a
    // hard step's max adjacent jump from 1.0 to roughly 0.05 (a 20x reduction) while leaving
    // a smooth ramp essentially untouched (endpoints move by ~0.003).
    //
    // Only applied when at least one point asks for it, so Hard/Medium curves are bit-exact
    // to the un-blurred maths -- which is what keeps Hard identical to v1's evaluateAt.
    bool anySoft = false;
    for (const auto& p : pts)
        if (p.weight == PointWeight::Soft)
        {
            anySoft = true;
            break;
        }

    if (! anySoft)
        return;

    constexpr float softWindowFraction = 0.02f;
    const int halfWindow = std::max (1, (int) (softWindowFraction * (float) tableSize * 0.5f));

    std::vector<float> raw (table, table + (size_t) tableSize);
    for (int i = 0; i < tableSize; ++i)
    {
        // Only blur where the governing point is Soft, so mixing weights in one curve keeps
        // each point's character instead of smearing the whole shape.
        const float phase = static_cast<float> (i) / static_cast<float> (tableSize - 1);
        int segIdx = 0;
        for (int j = 0; j < numPts - 1; ++j)
        {
            if (phase >= pts[(size_t) j].position && phase <= pts[(size_t) j + 1].position) { segIdx = j; break; }
            segIdx = j;
        }
        if (segIdx >= numPts - 1)
            segIdx = numPts - 2;

        const bool nearSoftPoint = pts[(size_t) segIdx].weight == PointWeight::Soft
                                || pts[(size_t) segIdx + 1].weight == PointWeight::Soft;
        if (! nearSoftPoint)
            continue;

        float acc = 0.0f;
        int count = 0;
        for (int k = -halfWindow; k <= halfWindow; ++k)
        {
            int idx = i + k;
            // Clamp rather than wrap: wrapping would drag the cycle's start value into its
            // end (and vice versa), which is audible as a discontinuity at the loop point on
            // any curve whose endpoints differ.
            idx = idx < 0 ? 0 : (idx >= tableSize ? tableSize - 1 : idx);
            acc += raw[(size_t) idx];
            ++count;
        }
        table[i] = acc / (float) count;
    }
}

/* -------------------------------------------------------------------------- */
/*  pointsForTier                                                              */
/* -------------------------------------------------------------------------- */

std::vector<CurvePointV2> SceneSchema::pointsForTier (uint8_t tier, float lockedValue,
                                                       float splitLow, float splitHigh,
                                                       bool reversed)
{
    std::vector<CurvePointV2> result;

    if (tier == 0)
    {
        CurvePointV2 p0, p1;
        p0.position = 0.0f; p0.value = lockedValue; p0.curvature = 0.0f;
        p0.weight = PointWeight::Medium;
        p1.position = 1.0f; p1.value = lockedValue; p1.curvature = 0.0f;
        p1.weight = PointWeight::Medium;
        result.push_back (p0);
        result.push_back (p1);
    }
    else if (tier == 1)
    {
        CurvePointV2 p0, p1;
        p0.position = 0.0f; p0.curvature = 0.0f; p0.weight = PointWeight::Medium;
        p1.position = 1.0f; p1.curvature = 0.0f; p1.weight = PointWeight::Medium;
        if (reversed)
        {
            p0.value = splitHigh;
            p1.value = splitLow;
        }
        else
        {
            p0.value = splitLow;
            p1.value = splitHigh;
        }
        result.push_back (p0);
        result.push_back (p1);
    }

    return result;
}

/* -------------------------------------------------------------------------- */
/*  sceneFromTree                                                              */
/* -------------------------------------------------------------------------- */

void SceneSchema::setLaneDefaults (int lane, const float* values, int count)
{
    if (lane < 0 || lane >= maxLanes || values == nullptr)
        return;

    auto& dst = g_laneDefaults[static_cast<size_t> (lane)];
    dst.fill (0.0f);
    for (int i = 0; i < count && i < maxParamsPerLane; ++i)
        dst[static_cast<size_t> (i)] = values[i];

    g_laneDefaultsSet[static_cast<size_t> (lane)] = true;
}

void SceneSchema::setLaneRanges (int lane, const float* minValues, const float* maxValues, int count)
{
    if (lane < 0 || lane >= maxLanes || minValues == nullptr || maxValues == nullptr)
        return;

    auto& lo = g_laneMin[static_cast<size_t> (lane)];
    auto& hi = g_laneMax[static_cast<size_t> (lane)];
    lo.fill (0.0f);
    hi.fill (1.0f);

    for (int i = 0; i < count && i < maxParamsPerLane; ++i)
    {
        lo[static_cast<size_t> (i)] = minValues[i];
        hi[static_cast<size_t> (i)] = maxValues[i];
    }

    g_laneRangesSet[static_cast<size_t> (lane)] = true;
}

void SceneSchema::getLaneRange (int lane, int paramIndex, float& minOut, float& maxOut)
{
    // 0..1 is the safe fallback: it is what the old unconditional clamp used, so a lane whose
    // ranges were never registered behaves exactly as before rather than unclamped.
    minOut = 0.0f;
    maxOut = 1.0f;

    if (lane < 0 || lane >= maxLanes || paramIndex < 0 || paramIndex >= maxParamsPerLane)
        return;
    if (! g_laneRangesSet[static_cast<size_t> (lane)])
        return;

    minOut = g_laneMin[static_cast<size_t> (lane)][static_cast<size_t> (paramIndex)];
    maxOut = g_laneMax[static_cast<size_t> (lane)][static_cast<size_t> (paramIndex)];
}

void SceneSchema::clearLaneDefaults()
{
    for (auto& d : g_laneDefaults) d.fill (0.0f);
    g_laneDefaultsSet.fill (false);

    for (auto& d : g_laneMin) d.fill (0.0f);
    for (auto& d : g_laneMax) d.fill (1.0f);
    g_laneRangesSet.fill (false);
}

SceneSnapshot SceneSchema::sceneFromTree (const juce::ValueTree& sceneTree)
{
    SceneSnapshot scene {};

    // ---- Geometry ----
    scene.beats = getPropInt (sceneTree, SceneIDs::beats, scene.beats);
    scene.beats = std::max (1, std::min (8, scene.beats));
    scene.divisions = getPropInt (sceneTree, SceneIDs::divisions, scene.divisions);
    scene.divisions = std::max (2, std::min (8, scene.divisions));
    scene.swing = clamp (getPropFloat (sceneTree, SceneIDs::swing, scene.swing), -1.0f, 1.0f);

    // ---- Seed ----
    scene.seed = static_cast<uint32_t> (getPropInt (sceneTree, SceneIDs::seed, 0));

    // ---- Loop/Release ----
    {
        const int lp = getPropInt (sceneTree, SceneIDs::loopPolicy, 0);
        if (lp >= 0 && lp < 3)
            scene.loopPolicy = static_cast<LoopPolicy> (lp);
    }
    {
        const int rm = getPropInt (sceneTree, SceneIDs::releaseMode, 0);
        if (rm >= 0 && rm < 5)
            scene.releaseMode = static_cast<ReleaseMode> (rm);
    }

    scene.populated = true;
    const int totalDiv = scene.totalDivisions();

    // ---- Blocks ----
    struct RawBlock
    {
        int lane = 0;
        int startDiv = 0;
        int lengthDiv = 1;
        uint8_t tier = 0;
        uint8_t flags = 0;
        float probability = 1.0f;
        uint32_t seedOffset = 0;
    };
    std::vector<std::vector<RawBlock>> lanesBlocks (maxLanes);

    {
        const auto blocksNode = sceneTree.getChildWithName (SceneIDs::blocksNode);
        if (blocksNode.isValid())
        {
            for (int i = 0; i < blocksNode.getNumChildren(); ++i)
            {
                const auto b = blocksNode.getChild (i);
                if (! b.hasType (SceneIDs::block))
                    continue;

                RawBlock rb;
                rb.lane = getPropInt (b, SceneIDs::laneRef, 0);
                if (rb.lane < 0 || rb.lane >= maxLanes)
                    continue;

                rb.startDiv = getPropInt (b, SceneIDs::start, 0);
                rb.lengthDiv = getPropInt (b, SceneIDs::length, 1);
                rb.tier = static_cast<uint8_t> (getPropInt (b, SceneIDs::tier, 0));
                rb.flags = static_cast<uint8_t> (getPropInt (b, SceneIDs::flags, 0));
                rb.probability = getPropFloat (b, SceneIDs::probability, 1.0f);
                rb.seedOffset = static_cast<uint32_t> (getPropInt (b, SceneIDs::seed, 0));

                rb.startDiv = std::max (0, std::min (rb.startDiv, totalDiv));
                if (rb.lengthDiv < 1) rb.lengthDiv = 1;
                const int end = rb.startDiv + rb.lengthDiv;
                if (end > totalDiv)
                    rb.lengthDiv = totalDiv - rb.startDiv;
                if (rb.lengthDiv < 1)
                    continue;

                lanesBlocks[static_cast<size_t> (rb.lane)].push_back (std::move (rb));
            }
        }
    }

    // Sort and de-overlap per lane
    for (int lane = 0; lane < maxLanes; ++lane)
    {
        auto& blocks = lanesBlocks[static_cast<size_t> (lane)];
        std::sort (blocks.begin(), blocks.end(),
                   [] (const RawBlock& a, const RawBlock& b)
                   { return a.startDiv < b.startDiv; });

        std::vector<Block> kept;
        int lastEnd = 0;
        for (const auto& rb : blocks)
        {
            if (rb.startDiv < lastEnd)
                continue;

            Block blk {};
            blk.startDiv = static_cast<int16_t> (rb.startDiv);
            blk.lengthDiv = static_cast<int16_t> (rb.lengthDiv);
            blk.tier = rb.tier;
            blk.flags = rb.flags;
            blk.probability = rb.probability;
            blk.seedOffset = rb.seedOffset;
            kept.push_back (std::move (blk));
            lastEnd = blk.endDiv();
        }

        const int num = std::min (static_cast<int> (kept.size()), maxBlocksPerLane);
        for (int j = 0; j < num; ++j)
            scene.lanes[static_cast<size_t> (lane)].blocks[static_cast<size_t> (j)] = std::move (kept[static_cast<size_t> (j)]);
        scene.lanes[static_cast<size_t> (lane)].numBlocks = num;
    }

    // ---- Lanes ----
    {
        // Seed every lane with its declared defaults BEFORE applying the tree, so a scene
        // that omits a lane still gets a usable parameter set rather than all zeros.
        for (int l = 0; l < maxLanes; ++l)
            if (g_laneDefaultsSet[static_cast<size_t> (l)])
                scene.lanes[static_cast<size_t> (l)].params = g_laneDefaults[static_cast<size_t> (l)];

        const auto lpNode = sceneTree.getChildWithName (SceneIDs::laneParams);
        if (lpNode.isValid())
        {
            for (int i = 0; i < lpNode.getNumChildren(); ++i)
            {
                const auto lt = lpNode.getChild (i);
                if (! lt.hasType (SceneIDs::lane))
                    continue;

                // Accept either spelling. The schema doc and FactoryScenes write "index";
                // sceneToTree writes "lane". They disagreed silently for a while, and the
                // symptom was invisible -- a scene's lane parameters were simply ignored and
                // the defaults used instead, which sounds plausible rather than broken.
                // Reading both means an existing tree of either shape still loads.
                int idx = getPropInt (lt, SceneIDs::index, -1);
                if (idx < 0)
                    idx = getPropInt (lt, SceneIDs::laneRef, -1);
                if (idx < 0 || idx >= maxLanes)
                    continue;

                auto& lane = scene.lanes[static_cast<size_t> (idx)];
                lane.mix = clamp (getPropFloat (lt, SceneIDs::mix, lane.mix), 0.0f, 1.0f);
                lane.gain = clamp (getPropFloat (lt, SceneIDs::gain, lane.gain), 0.0f, 2.0f);
                lane.pan = clamp (getPropFloat (lt, SceneIDs::pan, lane.pan), -1.0f, 1.0f);
                lane.filterType = getPropInt (lt, SceneIDs::filterType, lane.filterType);
                lane.filterCutoff = clamp (getPropFloat (lt, SceneIDs::filterCutoff, lane.filterCutoff), 0.0f, 1.0f);
                lane.filterResonance = clamp (getPropFloat (lt, SceneIDs::filterResonance, lane.filterResonance), 0.0f, 1.0f);
                lane.chainPosition = getPropInt (lt, SceneIDs::chainPosition, lane.chainPosition);
                lane.mute = getPropBool (lt, SceneIDs::mute, lane.mute);
                lane.solo = getPropBool (lt, SceneIDs::solo, lane.solo);
                lane.enabled = getPropBool (lt, SceneIDs::enabled, lane.enabled);

                for (int j = 0; j < lt.getNumChildren(); ++j)
                {
                    const auto p = lt.getChild (j);
                    if (! p.hasType (SceneIDs::param))
                        continue;
                    const int pi = getPropInt (p, SceneIDs::paramIndexProp, -1);
                    if (pi < 0 || pi >= maxParamsPerLane)
                        continue;
                    lane.params[static_cast<size_t> (pi)] = getPropFloat (p, SceneIDs::value, lane.params[static_cast<size_t> (pi)]);
                }
            }
        }
    }

    // ---- Curves ----
    int numActive = 0;

    {
        const auto curvesNode = sceneTree.getChildWithName (SceneIDs::curvesNode);
        if (curvesNode.isValid())
        {
            for (int i = 0; i < curvesNode.getNumChildren() && i < maxCurves; ++i)
            {
                const auto ct = curvesNode.getChild (i);
                if (! ct.hasType (SceneIDs::curve))
                    continue;

                auto& curve = scene.curves[static_cast<size_t> (i)];

                const int target = getPropInt (ct, SceneIDs::target, -1);
                curve.targetParam = isValidParamIndex (target) ? static_cast<int16_t> (target) : -1;

                curve.speedMultiplier = clamp (getPropFloat (ct, SceneIDs::speed, curve.speedMultiplier), 0.25f, 4.0f);
                curve.depth = clamp (getPropFloat (ct, SceneIDs::depth, curve.depth), 0.0f, 2.0f);
                curve.tier = static_cast<uint8_t> (getPropInt (ct, SceneIDs::tier, curve.tier));
                curve.bipolar = getPropBool (ct, SceneIDs::bipolar, curve.bipolar);
                curve.enabled = getPropBool (ct, SceneIDs::enabled, curve.enabled);

                std::vector<CurvePointV2> pts;
                if (curve.tier >= 2)
                {
                    for (int j = 0; j < ct.getNumChildren(); ++j)
                    {
                        const auto pt = ct.getChild (j);
                        if (! pt.hasType (SceneIDs::point))
                            continue;
                        CurvePointV2 p;
                        p.position = getPropFloat (pt, SceneIDs::position, 0.0f);
                        p.value = getPropFloat (pt, SceneIDs::value, 0.5f);
                        p.curvature = clamp (getPropFloat (pt, SceneIDs::curvature, 0.0f), -1.0f, 1.0f);
                        const int w = getPropInt (pt, SceneIDs::pointWeight, 1);
                        p.weight = (w >= 0 && w < 3) ? static_cast<PointWeight> (w) : PointWeight::Medium;
                        pts.push_back (p);
                    }
                }
                else
                {
                    const bool rev = curve.tier == 1 && curve.bipolar;
                    pts = pointsForTier (curve.tier, 0.5f, 0.0f, 1.0f, rev);
                }

                bakeCurveTable (pts, curve.table.data(), static_cast<int> (curve.table.size()));

                if (curve.enabled && isValidParamIndex (curve.targetParam))
                {
                    if (numActive < maxCurves)
                        scene.activeCurves[static_cast<size_t> (numActive)] = static_cast<int16_t> (i);
                    ++numActive;
                }
            }
        }
    }

    scene.numActiveCurves = numActive;

    return scene;
}

/* -------------------------------------------------------------------------- */
/*  sceneToTree                                                                */
/* -------------------------------------------------------------------------- */

juce::ValueTree SceneSchema::sceneToTree (const SceneSnapshot& scene, int index)
{
    juce::ValueTree sceneTree (SceneIDs::scene);

    sceneTree.setProperty (SceneIDs::index, index, nullptr);
    sceneTree.setProperty (SceneIDs::beats, scene.beats, nullptr);
    sceneTree.setProperty (SceneIDs::divisions, scene.divisions, nullptr);
    sceneTree.setProperty (SceneIDs::swing, scene.swing, nullptr);
    sceneTree.setProperty (SceneIDs::seed, juce::var (static_cast<juce::int64> (scene.seed)), nullptr);
    sceneTree.setProperty (SceneIDs::loopPolicy, static_cast<int> (scene.loopPolicy), nullptr);
    sceneTree.setProperty (SceneIDs::releaseMode, static_cast<int> (scene.releaseMode), nullptr);

    // Lane params
    {
        juce::ValueTree lpNode (SceneIDs::laneParams);
        for (int lane = 0; lane < maxLanes; ++lane)
        {
            const auto& ls = scene.lanes[static_cast<size_t> (lane)];
            juce::ValueTree lt (SceneIDs::lane);
            lt.setProperty (SceneIDs::index, lane, nullptr);
            lt.setProperty (SceneIDs::mix, ls.mix, nullptr);
            lt.setProperty (SceneIDs::gain, ls.gain, nullptr);
            lt.setProperty (SceneIDs::pan, ls.pan, nullptr);
            lt.setProperty (SceneIDs::filterType, ls.filterType, nullptr);
            lt.setProperty (SceneIDs::filterCutoff, ls.filterCutoff, nullptr);
            lt.setProperty (SceneIDs::filterResonance, ls.filterResonance, nullptr);
            lt.setProperty (SceneIDs::chainPosition, ls.chainPosition, nullptr);
            lt.setProperty (SceneIDs::mute, ls.mute, nullptr);
            lt.setProperty (SceneIDs::solo, ls.solo, nullptr);
            lt.setProperty (SceneIDs::enabled, ls.enabled, nullptr);

            for (int p = 0; p < maxParamsPerLane; ++p)
            {
                if (std::abs (ls.params[static_cast<size_t> (p)]) > 1.0e-6f)
                {
                    juce::ValueTree pt (SceneIDs::param);
                    pt.setProperty (SceneIDs::paramIndexProp, p, nullptr);
                    pt.setProperty (SceneIDs::value, ls.params[static_cast<size_t> (p)], nullptr);
                    lt.appendChild (pt, nullptr);
                }
            }

            lpNode.appendChild (lt, nullptr);
        }
        sceneTree.appendChild (lpNode, nullptr);
    }

    // Blocks
    {
        juce::ValueTree blocksNode (SceneIDs::blocksNode);
        for (int lane = 0; lane < maxLanes; ++lane)
        {
            const auto& ls = scene.lanes[static_cast<size_t> (lane)];
            for (int b = 0; b < ls.numBlocks; ++b)
            {
                const auto& blk = ls.blocks[static_cast<size_t> (b)];
                juce::ValueTree bTree (SceneIDs::block);
                bTree.setProperty (SceneIDs::laneRef, lane, nullptr);
                bTree.setProperty (SceneIDs::start, blk.startDiv, nullptr);
                bTree.setProperty (SceneIDs::length, blk.lengthDiv, nullptr);
                bTree.setProperty (SceneIDs::tier, blk.tier, nullptr);
                bTree.setProperty (SceneIDs::flags, blk.flags, nullptr);
                bTree.setProperty (SceneIDs::probability, blk.probability, nullptr);
                bTree.setProperty (SceneIDs::seed, juce::var (static_cast<juce::int64> (blk.seedOffset)), nullptr);
                blocksNode.appendChild (bTree, nullptr);
            }
        }
        sceneTree.appendChild (blocksNode, nullptr);
    }

    // Curves -- structural data only, no <Pt> children
    {
        juce::ValueTree curvesNode (SceneIDs::curvesNode);
        for (int i = 0; i < maxCurves; ++i)
        {
            const auto& c = scene.curves[static_cast<size_t> (i)];
            if (c.targetParam < 0 && ! c.enabled)
                continue;

            juce::ValueTree cTree (SceneIDs::curve);
            cTree.setProperty (SceneIDs::target, static_cast<int> (c.targetParam), nullptr);
            cTree.setProperty (SceneIDs::speed, c.speedMultiplier, nullptr);
            cTree.setProperty (SceneIDs::depth, c.depth, nullptr);
            cTree.setProperty (SceneIDs::tier, c.tier, nullptr);
            cTree.setProperty (SceneIDs::bipolar, c.bipolar, nullptr);
            cTree.setProperty (SceneIDs::enabled, c.enabled, nullptr);
            curvesNode.appendChild (cTree, nullptr);
        }
        sceneTree.appendChild (curvesNode, nullptr);
    }

    return sceneTree;
}

} // namespace stutter
