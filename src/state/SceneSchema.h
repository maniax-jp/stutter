#pragma once
#include "SceneSnapshot.h"
#include <juce_data_structures/juce_data_structures.h>
#include <vector>

namespace stutter
{

/**
    ValueTree identifiers for the v2 state schema.

    Layout:

        <StutterState version="2" activeScene>
          <PARAMETERS>                 <!-- APVTS: globals + active-scene mirror -->
          <Scenes>
            <Scene index name note seed beats divisions swing loopPolicy releaseMode>
              <LaneParams>
                <Lane index mute solo enabled mix gain pan
                      filterType filterCutoff filterResonance chainPosition>
                  <P i v/>
                </Lane>
              </LaneParams>
              <Blocks>
                <B lane start len tier flags prob seed/>
              </Blocks>
              <Curves>
                <Curve target speed depth tier bipolar enabled>
                  <Pt p v c w/>
                </Curve>
              </Curves>
              <Weights><W lane v/></Weights>
            </Scene>
          </Scenes>

    Block positions are integer divisions, not PPQ, so changing beats/divisions re-times a
    pattern rather than corrupting it. Curve targets are integer ParamIndex values, which is
    what makes a modulation route storable as data.
*/
namespace SceneIDs
{
    // Root and top-level nodes
    static const juce::Identifier stateRoot   { "StutterState" };
    /** Unused. The globals it was meant to hold are written as root properties instead, so
        nothing reads or writes a <Globals> element -- kept only so a reader of old notes does
        not go looking for it. */
    static const juce::Identifier globalsNode { "Globals" };
    static const juce::Identifier scenesNode  { "Scenes" };
    static const juce::Identifier scene       { "Scene" };

    // Scene children
    static const juce::Identifier laneParams  { "LaneParams" };
    static const juce::Identifier lane        { "Lane" };
    static const juce::Identifier param       { "P" };
    static const juce::Identifier blocksNode  { "Blocks" };
    static const juce::Identifier block       { "B" };
    static const juce::Identifier curvesNode  { "Curves" };
    static const juce::Identifier curve       { "Curve" };
    static const juce::Identifier point       { "Pt" };
    static const juce::Identifier weightsNode { "Weights" };
    static const juce::Identifier weight      { "W" };

    // Shared properties
    static const juce::Identifier version     { "version" };
    static const juce::Identifier index       { "index" };
    static const juce::Identifier name        { "name" };
    static const juce::Identifier note        { "note" };
    static const juce::Identifier seed        { "seed" };

    // Globals
    static const juce::Identifier activeScene { "activeScene" };

    /** Retired with the MIDI performance layer: which scene plays and whether it is heard are
        now the sceneSelect and active parameters, so automation can drive them. Sessions saved
        before that still carry these three, and nothing reads them -- the identifiers stay so
        the compatibility test can write a period-accurate tree, and so that a property found
        in an old project file can be traced to what it once meant. */
    static const juce::Identifier sceneLock   { "sceneLock" };
    static const juce::Identifier playMode    { "playMode" };
    static const juce::Identifier triggerQuantize { "triggerQuantize" };

    // Scene geometry
    static const juce::Identifier beats       { "beats" };
    static const juce::Identifier divisions   { "divisions" };
    static const juce::Identifier swing       { "swing" };
    static const juce::Identifier loopPolicy  { "loopPolicy" };
    static const juce::Identifier releaseMode { "releaseMode" };

    // Lane
    static const juce::Identifier mute            { "mute" };
    static const juce::Identifier solo            { "solo" };
    static const juce::Identifier enabled         { "enabled" };
    static const juce::Identifier mix             { "mix" };
    static const juce::Identifier gain            { "gain" };
    static const juce::Identifier pan             { "pan" };
    static const juce::Identifier filterType      { "filterType" };
    static const juce::Identifier filterCutoff    { "filterCutoff" };
    static const juce::Identifier filterResonance { "filterResonance" };
    static const juce::Identifier chainPosition   { "chainPosition" };

    // Param / block / curve / point
    static const juce::Identifier paramIndexProp { "i" };
    static const juce::Identifier value           { "v" };
    static const juce::Identifier laneRef         { "lane" };
    static const juce::Identifier start           { "start" };
    static const juce::Identifier length          { "len" };
    static const juce::Identifier tier            { "tier" };
    static const juce::Identifier flags           { "flags" };
    static const juce::Identifier probability     { "prob" };
    static const juce::Identifier target          { "target" };
    static const juce::Identifier speed           { "speed" };
    static const juce::Identifier depth           { "depth" };
    static const juce::Identifier bipolar         { "bipolar" };
    static const juce::Identifier position        { "p" };
    static const juce::Identifier curvature       { "c" };
    static const juce::Identifier pointWeight     { "w" };
} // namespace SceneIDs

/** Schema version written by this build. setStateInformation rejects anything older: v1
    state has no version property at all, so getProperty(version, 1) yields 1 and the load
    falls back to Init rather than half-applying an incompatible tree. */
inline constexpr int stateSchemaVersion = 2;

/**
    Anti-click weighting for a curve breakpoint, from ShaperBox 3.

    Making this explicit per point beats a single global smoothing knob, because the user
    places the clicks exactly where they want them: Hard is an instantaneous jump (and can
    click), Medium rounds the corner just enough to avoid clicking, Soft is a smooth curve.
*/
enum class PointWeight : uint8_t
{
    Hard = 0,
    Medium,
    Soft
};

/** One editable breakpoint. Lives in the ValueTree and the editor; baked away before the
    audio thread ever sees it. */
struct CurvePointV2
{
    float position = 0.0f;   ///< 0..1 within the cycle
    float value = 0.5f;      ///< 0..1
    float curvature = 0.0f;  ///< -1..1, 0 = linear
    PointWeight weight = PointWeight::Medium;
};

/**
    Conversion between the ValueTree schema and the baked snapshots.

    All of this runs on the message thread: sceneFromTree bakes curve tables, which is exactly
    the work that must not happen on the audio thread.
*/
namespace SceneSchema
{
    /**
        Per-lane parameter defaults, seeded before a scene's <Lane> nodes are applied.

        Without this a scene that omits a lane gets all-zero parameters rather than the
        declared defaults -- a Filter with cutoff 0 and a Gate with duty 0 are both silent,
        so an unconfigured lane would produce nothing instead of its neutral sound. Scenes
        legitimately omit lanes they do not use, so this is the common case, not an edge one.

        SceneSchema cannot reach the effects (they live in dsp/, which depends on state/, not
        the reverse), so the processor registers the descriptor defaults here at startup.
    */
    void setLaneDefaults (int lane, const float* values, int count);

    /** Clear all registered defaults. Tests use this to isolate. */
    void clearLaneDefaults();

    /** Build a baked snapshot from one <Scene> node. Missing or malformed properties fall
        back to the SceneSnapshot defaults, so a truncated tree yields a usable scene rather
        than garbage. */
    SceneSnapshot sceneFromTree (const juce::ValueTree& sceneTree);

    /** Serialise a snapshot's structural data back to a <Scene> node.

        Note this is NOT a round-trip of sceneFromTree: a snapshot holds baked tables, not
        breakpoints, so the curve shapes cannot be recovered from it. The editable tree is the
        authority and is what gets saved; this exists for tests and for building factory
        presets programmatically. */
    juce::ValueTree sceneToTree (const SceneSnapshot& scene, int index);

    /** Bake breakpoints into a curve table. Shared by the tier resolution below and by the
        editor's preview rendering, so the drawn curve and the heard curve cannot drift --
        v1 had two copies of this maths (CurveModulator.h:246 and CurveEditor.cpp:302). */
    void bakeCurveTable (const std::vector<CurvePointV2>& points,
                         float* table, int tableSize);

    /** Resolve a Locked or Split tier into the breakpoints a Custom curve would need, so all
        three tiers cost the same at run time. Locked yields a flat line; Split yields a
        two-point ramp, reversed when `reversed` is set. */
    std::vector<CurvePointV2> pointsForTier (uint8_t tier, float lockedValue,
                                             float splitLow, float splitHigh, bool reversed);
} // namespace SceneSchema

} // namespace stutter
