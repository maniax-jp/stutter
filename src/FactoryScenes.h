#pragma once
#include "state/SceneSchema.h"
#include <juce_data_structures/juce_data_structures.h>

namespace stutter
{

/**
    Factory scene banks for v2.

    These exist to demonstrate what the v1 grid could not express, so each one is chosen for
    a capability rather than for a sound: variable-length blocks that let an envelope
    complete, automated scene switching, a curve routed to an arbitrary parameter, and
    the seeded effects. A bank whose presets all sound different but exercise one code path
    would leave most of v2 unverified by anything a user actually loads.

    Built as ValueTrees in the v2 schema rather than as the v1 FactoryPresetDef structs,
    because a scene carries pattern geometry, blocks, curves and a seed that the flat
    param/step/curve triple has no room for.
*/
namespace FactoryScenes
{
    /** Number of factory banks. */
    int getNumBanks();

    /** Display name for a bank. */
    juce::String getBankName (int index);

    /** Build a bank as a <Scenes> tree ready for SceneDocument::replaceState. Returns an
        invalid tree for an out-of-range index. */
    juce::ValueTree createBank (int index);
}

} // namespace stutter
