#pragma once
#include <juce_core/juce_core.h>

namespace stutter
{

// ---- Capacity constants ----------------------------------------------------------------
//
// These are the hard upper bounds the whole v2 architecture is sized against. They exist as
// compile-time constants (not runtime values) because SceneSnapshot must be a POD of fixed
// extent -- it is memcpy'd into a double-buffered bank and read from the audio thread through
// an atomic pointer, so it can hold no heap-owning members. Raising any of these changes the
// size of every scene in the bank; see src/state/SceneSnapshot.h for the memory arithmetic.

/** Lane count. v1 shipped 8 (Stutter..Crush); v2 reserves 4 more for the effects added in
    WP10 (Stretcher, Shuffler, Delay, Distortion). */
inline constexpr int maxLanes = 12;

/** Per-lane parameter slots. The widest v1 effect is Filter at 5 params; 12 leaves headroom
    for the Common Strip (mix/filter/gain/pan) plus growth without a schema break. */
inline constexpr int maxParamsPerLane = 12;

/** Modulation curves per scene. v1 had exactly 3 (Volume/Filter/Pan) hardwired to fixed
    targets; in v2 any curve can route to any ParamIndex, so the count is a budget rather
    than a structural fact. */
inline constexpr int maxCurves = 16;

/** Sequencer blocks per lane. At the finest grid (8 beats x 8 divisions = 64 divisions) a
    lane cannot hold more than 64 non-adjacent blocks, so this bound cannot be hit by any
    representable pattern. */
inline constexpr int maxBlocksPerLane = 64;

// ---- Flat parameter addressing ---------------------------------------------------------
//
// v1 addressed lane parameters by building a string ("lane3_" + "cutoff") and hashing it
// through APVTS on every audio-thread read (see the getParam() helper duplicated across all
// eight v1 effects, e.g. StutterEffect.h:187-192). That cannot survive v2 for three reasons:
//
//   1. A modulation matrix has to name a target as data. "Lane 3, param 2" must be storable
//      in a ValueTree attribute and comparable in a hot loop -- an integer, not a string.
//   2. Scene snapshots must be POD. A juce::String member would put the heap inside a
//      structure the audio thread reads through an atomic pointer.
//   3. APVTS is no longer authoritative for lane params in v2 (it mirrors the active scene),
//      so reading it from the audio thread would race with scene switching.
//
// So parameters live in a flat index space: lane-local slots first, then the globals that
// are worth modulating. The layout is deliberately simple arithmetic rather than a lookup
// table -- ModulationEngine indexes a scratch array by ParamIndex once per sample.

/** Number of index slots consumed by per-lane parameters. */
inline constexpr int laneParamSlots = maxLanes * maxParamsPerLane;

/** Global parameters that a curve is allowed to target. These occupy the tail of the index
    space, immediately after the lane slots.

    Note this is a subset of the APVTS globals: hostSync, sceneSelect and active are discrete
    transport or performance switches, and modulating them from a curve would be nonsense (and
    would fight the automation lane for control). Only the continuous output-stage values are
    routable, which is what makes SE2's "modulated output gain IS your sidechain pump" trick
    available here. */
enum class GlobalParam
{
    dryWet = 0,
    outputGain,
    count
};

/** Total size of the parameter index space. ModulationEngine's scratch array is this long. */
inline constexpr int totalParamSlots = laneParamSlots + (int) GlobalParam::count;

/** Address a per-lane parameter. Callers are expected to pass in-range values; the
    arithmetic is unguarded because this sits in the per-sample modulation loop. Use
    isValidParamIndex() when the input came from deserialized state. */
constexpr int paramIndex (int lane, int param) noexcept
{
    return lane * maxParamsPerLane + param;
}

/** Address a routable global parameter. */
constexpr int paramIndex (GlobalParam g) noexcept
{
    return laneParamSlots + (int) g;
}

/** True if `index` names a real slot. Deserialization must gate on this: a curve whose
    stored target came from a future build (or a corrupted file) would otherwise index out
    of the scratch array. */
constexpr bool isValidParamIndex (int index) noexcept
{
    return index >= 0 && index < totalParamSlots;
}

/** Decompose a lane parameter index. Returns false for global (or invalid) indices, in which
    case the out-params are untouched. Used by the UI to label a modulation route and by
    state code to map an index back onto an APVTS parameter ID. */
constexpr bool decomposeLaneParam (int index, int& laneOut, int& paramOut) noexcept
{
    if (index < 0 || index >= laneParamSlots)
        return false;

    laneOut = index / maxParamsPerLane;
    paramOut = index % maxParamsPerLane;
    return true;
}

} // namespace stutter
