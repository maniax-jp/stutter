#include "PresetManager.h"
#include "PluginProcessor.h"
#include "FactoryPresets.h"
#include "FactoryScenes.h"
#include "dsp/ParameterIDs.h"
#include "state/SceneSchema.h"
#include "ui/SceneBrowser.h"

namespace stutter
{

namespace
{
constexpr const char* kUserPresetFileExtension = ".xml";

// Curve names in the fixed order used by getStateInformation/setStateInformation.
const juce::Identifier curveNameIds[] = { { "Volume" }, { "Filter" }, { "Pan" } };

/** Builds a "PARAMETERS" (matches the APVTS ctor's root tag) ValueTree with every registered
    parameter set to its default value, as a PARAM child (id/value), exactly matching the shape
    AudioProcessorValueTreeState::copyState()/replaceState() expect. This is the base every
    factory preset def's overrides get applied on top of. */
juce::ValueTree buildDefaultParametersTree (juce::AudioProcessorValueTreeState& apvts)
{
    juce::ValueTree tree ("PARAMETERS");

    for (auto* p : apvts.processor.getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            juce::ValueTree param ("PARAM");
            param.setProperty ("id", ranged->paramID, nullptr);
            const float denormDefault = ranged->convertFrom0to1 (ranged->getDefaultValue());
            param.setProperty ("value", denormDefault, nullptr);
            tree.appendChild (param, nullptr);
        }
    }

    return tree;
}

/**
    Parameters a preset must not carry.

    A preset describes a sound. `sceneSelect` and `active` describe where you are in a track --
    which scene is up next and whether this bar is glitched -- and the host's automation lane
    owns both. Baking them into a preset means choosing a sound silently reaches into the
    arrangement, and it also made merely toggling ACTIVE mark the preset as edited.

    They are stripped on save and ignored on load, so an existing preset that already contains
    them behaves the same as a new one that never did.
*/
bool isPerformanceParam (const juce::String& paramId)
{
    return paramId == ID::sceneSelect || paramId == ID::active;
}

/** Remove the performance parameters from a preset's PARAMETERS node, in place. */
void stripPerformanceParams (juce::ValueTree& state)
{
    for (int i = state.getNumChildren(); --i >= 0;)
    {
        auto child = state.getChild (i);
        if (child.hasType ("PARAM") && isPerformanceParam (child.getProperty ("id").toString()))
            state.removeChild (i, nullptr);
    }

    // Written by getStateInformation for project restore; a preset has no business moving the
    // scene the user is sitting on.
    state.removeProperty (SceneIDs::activeScene, nullptr);
}

void setParamValue (juce::ValueTree& parametersTree, const juce::String& paramId, float value)
{
    for (int i = 0; i < parametersTree.getNumChildren(); ++i)
    {
        auto child = parametersTree.getChild (i);
        if (child.hasType ("PARAM") && child.getProperty ("id").toString() == paramId)
        {
            child.setProperty ("value", value, nullptr);
            return;
        }
    }

    // Not found (shouldn't happen for valid IDs) -- append it so preset authoring mistakes are
    // at least visible/loadable rather than silently dropped.
    juce::ValueTree param ("PARAM");
    param.setProperty ("id", paramId, nullptr);
    param.setProperty ("value", value, nullptr);
    parametersTree.appendChild (param, nullptr);
}

/** Turn a v1 16-step grid into a single v2 scene.

    v1's fixed grid is exactly beats=4 x divisions=4, so a step maps to a division with no
    rounding: this is a re-encoding, not a re-interpretation. Runs of adjacent ON steps become
    one block rather than N one-division blocks, which is the whole point of the block model --
    under the old grid a lane restarted every 16th, so TapeStop never finished decelerating.
    Merging preserves that for lanes where it does not matter and repairs it where it does.

    Without this the patterns were simply dropped: the <Sequencer> node they used to live in is
    stripped on load and read by nothing, so 20 of the 28 presets played silence. */
juce::ValueTree buildScenesTreeFromSteps (StutterAudioProcessor& proc,
                                          const FactoryPresetDef& def)
{
    const auto& stepsOn = def.stepsOn;

    juce::ValueTree scenes (SceneIDs::scenesNode);
    if (stepsOn.empty())
        return scenes;

    std::array<std::array<bool, numSteps>, numLanes> grid {};
    for (auto& s : stepsOn)
        if (s.lane >= 0 && s.lane < numLanes && s.step >= 0 && s.step < numSteps)
            grid[(size_t) s.lane][(size_t) s.step] = true;

    juce::ValueTree scene (SceneIDs::scene);
    // Scene 60 (C4), not scene 0. These presets have no note mapping of their own, so the
    // index is free -- and C4 is both where the scene browser opens and where the factory
    // banks start, so the pattern is visible in the grid the moment the preset loads. Writing
    // it to scene 0 plays correctly but shows the user an empty grid, which reads as a preset
    // that failed to load.
    scene.setProperty (SceneIDs::index, ui::SceneBrowser::defaultScene, nullptr);
    scene.setProperty (SceneIDs::beats, 4, nullptr);
    scene.setProperty (SceneIDs::divisions, 4, nullptr);
    scene.setProperty (SceneIDs::swing, 0.0f, nullptr);

    juce::ValueTree blocks (SceneIDs::blocksNode);
    for (int l = 0; l < numLanes; ++l)
    {
        int step = 0;
        while (step < numSteps)
        {
            if (! grid[(size_t) l][(size_t) step]) { ++step; continue; }

            const int start = step;
            while (step < numSteps && grid[(size_t) l][(size_t) step])
                ++step;

            juce::ValueTree b (SceneIDs::block);
            b.setProperty (SceneIDs::laneRef, l, nullptr);
            b.setProperty (SceneIDs::start, start, nullptr);
            b.setProperty (SceneIDs::length, step - start, nullptr);
            blocks.appendChild (b, nullptr);
        }
    }

    scene.appendChild (blocks, nullptr);

    // Copy the preset's parameter values into the scene as well. The scene is the authority
    // once loaded: the mirror pushes its lane values into APVTS shortly after, so a scene that
    // carries blocks but no parameters would overwrite the preset's own settings with the
    // descriptor defaults -- the pattern would play with the wrong rate and decay.
    juce::ValueTree laneParams (SceneIDs::laneParams);
    for (int l = 0; l < numLanes; ++l)
    {
        auto* effect = proc.getBlockSequencerEffect (l);
        if (effect == nullptr)
            continue;

        const auto set = effect->getParamDescriptors();
        const auto prefix = ID::lanePrefix (l);

        juce::ValueTree laneNode;
        for (int p = 0; p < set.count && p < maxParamsPerLane; ++p)
        {
            const auto fullId = prefix + set[p].id;
            for (const auto& pv : def.paramValues)
            {
                if (pv.paramId != fullId)
                    continue;

                if (! laneNode.isValid())
                {
                    laneNode = juce::ValueTree (SceneIDs::lane);
                    laneNode.setProperty (SceneIDs::index, l, nullptr);
                }

                juce::ValueTree pn (SceneIDs::param);
                pn.setProperty (SceneIDs::paramIndexProp, p, nullptr);
                pn.setProperty (SceneIDs::value, pv.value, nullptr);
                laneNode.appendChild (pn, nullptr);
                break;
            }
        }

        if (laneNode.isValid())
            laneParams.appendChild (laneNode, nullptr);
    }

    if (laneParams.getNumChildren() > 0)
        scene.appendChild (laneParams, nullptr);

    scenes.appendChild (scene, nullptr);
    return scenes;
}

juce::ValueTree buildSequencerTree (const std::vector<FactoryPresetDef::StepOn>& stepsOn)
{
    juce::ValueTree seqTree (ID::sequencerNode);
    std::array<std::array<bool, numSteps>, numLanes> grid {};
    for (auto& s : stepsOn)
        if (s.lane >= 0 && s.lane < numLanes && s.step >= 0 && s.step < numSteps)
            grid[(size_t) s.lane][(size_t) s.step] = true;

    for (int l = 0; l < numLanes; ++l)
    {
        juce::ValueTree laneTree (ID::laneNode);
        laneTree.setProperty (ID::propIndex, l, nullptr);
        for (int s = 0; s < numSteps; ++s)
        {
            juce::ValueTree stepTree (ID::stepNode);
            stepTree.setProperty (ID::propIndex, s, nullptr);
            stepTree.setProperty (ID::propOn, grid[(size_t) l][(size_t) s], nullptr);
            laneTree.appendChild (stepTree, nullptr);
        }
        seqTree.appendChild (laneTree, nullptr);
    }
    return seqTree;
}

juce::ValueTree buildCurveTree (const juce::String& name, bool enabled, int syncDiv,
                                 const std::vector<FactoryPresetDef::CurvePointDef>& points)
{
    juce::ValueTree curveTree (ID::curveNode);
    curveTree.setProperty (ID::propName, name, nullptr);
    curveTree.setProperty (ID::propEnabled, enabled, nullptr);
    curveTree.setProperty (ID::propSyncDiv, syncDiv, nullptr);

    // Default flat line (matches CurveModulator's own neutral default) when a preset leaves a
    // curve untouched -- still needs >=2 points for CurveModulator::fromValueTree to accept it.
    if (points.empty())
    {
        const float neutral = ID::neutralValueForCurve (name);
        curveTree.appendChild ([&] {
            juce::ValueTree pt (ID::pointNode);
            pt.setProperty (ID::propPosition, 0.0f, nullptr);
            pt.setProperty (ID::propValue, neutral, nullptr);
            pt.setProperty (ID::propCurvature, 0.0f, nullptr);
            return pt;
        }(), nullptr);
        curveTree.appendChild ([&] {
            juce::ValueTree pt (ID::pointNode);
            pt.setProperty (ID::propPosition, 1.0f, nullptr);
            pt.setProperty (ID::propValue, neutral, nullptr);
            pt.setProperty (ID::propCurvature, 0.0f, nullptr);
            return pt;
        }(), nullptr);
    }
    else
    {
        for (auto& p : points)
        {
            juce::ValueTree pt (ID::pointNode);
            pt.setProperty (ID::propPosition, p.position, nullptr);
            pt.setProperty (ID::propValue, p.value, nullptr);
            pt.setProperty (ID::propCurvature, p.curvature, nullptr);
            curveTree.appendChild (pt, nullptr);
        }
    }

    return curveTree;
}

juce::ValueTree buildCurvesTree (const std::vector<FactoryPresetDef::CurveDef>& curveDefs)
{
    juce::ValueTree curvesTree (ID::curvesNode);

    for (auto& curveNameId : curveNameIds)
    {
        const juce::String name = curveNameId.toString();
        const FactoryPresetDef::CurveDef* match = nullptr;
        for (auto& c : curveDefs)
            if (c.name == name)
            {
                match = &c;
                break;
            }

        if (match != nullptr)
            curvesTree.appendChild (buildCurveTree (name, match->enabled, match->syncDiv, match->points), nullptr);
        else
            // Preset doesn't mention this curve at all -- leave it OFF (no audible effect either
            // way, since disabled short-circuits before the curve value is even read) but still
            // give it a fully-formed neutral-flat tree so no lane of the Curves node is ever
            // missing/incomplete.
            curvesTree.appendChild (buildCurveTree (name, false, 4 /* default 1/4-ish */, {}), nullptr);
    }

    return curvesTree;
}

/** Assembles one full preset state (PARAMETERS root + Sequencer + Curves children), in exactly
    the same shape getStateInformation()/setStateInformation() serialise/parse. */
juce::ValueTree buildFullStateTree (StutterAudioProcessor& proc, const FactoryPresetDef& def)
{
    auto& apvts = proc.getAPVTS();
    auto tree = buildDefaultParametersTree (apvts);

    // Without this every factory preset is indistinguishable from v1 state and gets rejected
    // by setStateInformation's version guard, which silently substitutes Init -- so the whole
    // factory bank loaded as silence while the browser showed the preset's name.
    tree.setProperty (SceneIDs::version, stateSchemaVersion, nullptr);

    for (auto& pv : def.paramValues)
        setParamValue (tree, pv.paramId, pv.value);

    // The v1 <Sequencer> node is kept so these trees still match what a v1 user preset on
    // disk looks like, but nothing reads it any more -- the blocks below are what plays.
    tree.appendChild (buildSequencerTree (def.stepsOn), nullptr);
    tree.appendChild (buildCurvesTree (def.curves), nullptr);

    // Always attach the Scenes node, even when it is empty. setStateInformation skips scene
    // restoration entirely when the node is missing, so omitting it does not mean "no scenes"
    // -- it means "leave the previous preset's scenes alone". Init is exactly the preset that
    // has none, and that is how loading it used to leave the old blocks playing.
    tree.appendChild (buildScenesTreeFromSteps (proc, def), nullptr);

    return tree;
}
}

//==============================================================================
PresetManager::PresetManager (StutterAudioProcessor& processor) : proc (processor)
{
    rebuildPresetList();
    proc.getAPVTS().state.addListener (this);
}

void PresetManager::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (applyingPreset)
        return;

    // The performance parameters are not part of a preset, so moving them cannot make one
    // modified. Without this, merely toggling ACTIVE -- or letting the host's automation move
    // the scene -- put a "*" next to a preset the user had not touched.
    if (tree.hasType ("PARAM") && isPerformanceParam (tree.getProperty ("id").toString()))
        return;

    dirty = true;
}

void PresetManager::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&)
{
    if (! applyingPreset)
        dirty = true;
}

void PresetManager::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int)
{
    if (! applyingPreset)
        dirty = true;
}

void PresetManager::valueTreeRedirected (juce::ValueTree&)
{
    // replaceState() during our own preset load redirects the tree; guarded by applyingPreset.
    if (! applyingPreset)
        dirty = true;
}

void PresetManager::rebuildPresetList()
{
    presets.clear();

    presets.push_back ({ "Init", "Init", true, {}, -1 });

    // v2 scene banks first: these are what the plugin is now, and a user scrolling from the
    // top should reach a playable multi-scene set before the single-patch legacy entries.
    for (int b = 0; b < FactoryScenes::getNumBanks(); ++b)
        presets.push_back ({ FactoryScenes::getBankName (b), "Scenes", true, {}, b });

    for (auto& def : getFactoryPresetDefs())
        presets.push_back ({ def.name, def.category, true, {}, -1 });

    refreshUserPresets();
}

void PresetManager::refreshUserPresets()
{
    // Preserve factory entries (already in presets from rebuildPresetList's initial pass, or
    // from a previous refresh), replace only the trailing "User" block.
    std::vector<PresetEntry> nonUser;
    nonUser.reserve (presets.size());
    for (auto& e : presets)
        if (e.category != "User")
            nonUser.push_back (e);
    presets = std::move (nonUser);

    auto dir = getUserPresetDirectory();
    if (dir.isDirectory())
    {
        juce::Array<juce::File> files;
        dir.findChildFiles (files, juce::File::findFiles, false, "*" + juce::String (kUserPresetFileExtension));
        files.sort();

        for (auto& f : files)
        {
            PresetEntry e;
            e.name = f.getFileNameWithoutExtension();
            e.category = "User";
            e.isFactory = false;
            e.userFile = f;
            presets.push_back (e);
        }
    }

    // Clamp / re-find currentIndex by name so a refresh (e.g. right after saving) doesn't yank
    // the browser back to preset 0.
    const juce::String keepName = currentNameOverride.isNotEmpty() ? currentNameOverride : getCurrentPresetName();
    int found = -1;
    for (int i = 0; i < (int) presets.size(); ++i)
        if (presets[(size_t) i].name == keepName)
        {
            found = i;
            break;
        }
    if (found >= 0)
        currentIndex = found;
    else
        currentIndex = juce::jlimit (0, (int) presets.size() - 1, currentIndex);

    currentNameOverride.clear();
}

juce::String PresetManager::getCurrentPresetName() const noexcept
{
    if (deletedCurrentPresetName.isNotEmpty())
        return deletedCurrentPresetName;
    if (currentIndex >= 0 && currentIndex < (int) presets.size())
        return presets[(size_t) currentIndex].name;
    return "Init";
}

juce::File PresetManager::getUserPresetDirectory()
{
    juce::File f (juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("Library/Audio/Presets/Maniax/Stutter"));
    if (! f.isDirectory())
        f.createDirectory();
    return f;
}

juce::ValueTree PresetManager::loadEntryState (const PresetEntry& entry) const
{
    if (! entry.isFactory)
    {
        if (auto xml = juce::parseXML (entry.userFile))
            return juce::ValueTree::fromXml (*xml);
        return {};
    }

    if (entry.name == "Init")
    {
        // Init: every parameter at its default, sequencer fully OFF, and -- per spec -- all
        // three curves (Volume/Filter/Pan) ON but each flat at its own neutral value, so Init is
        // acoustically transparent (identical to a freshly-instantiated plugin) rather than
        // silently applying a coloring effect (e.g. Filter flat at 0.5 previously meant an
        // audible ~2kHz lowpass despite looking "off").
        FactoryPresetDef initDef;
        initDef.name = "Init";
        initDef.category = "Init";
        initDef.curves = {
            { "Volume", true, 4, {} },
            { "Filter", true, 4, {} },
            { "Pan",    true, 4, {} },
        };
        return buildFullStateTree (proc, initDef);
    }

    if (entry.sceneBankIndex >= 0)
    {
        // A scene bank carries no parameter values of its own: everything it needs lives in
        // the <Scenes> node, and the per-lane values are mirrored out of the active scene
        // once it loads. Start from the defaults so any previous patch's dialled-in settings
        // do not leak through underneath.
        FactoryPresetDef bare;
        bare.name = entry.name;
        bare.category = entry.category;
        auto tree = buildFullStateTree (proc, bare);

        auto scenes = FactoryScenes::createBank (entry.sceneBankIndex);
        if (! scenes.isValid())
            return {};

        // buildFullStateTree always attaches a Scenes node (empty here, since `bare` has no
        // steps). Drop it before adding the bank's: getChildWithName returns the first match,
        // so leaving both would hand the restore the empty one and load a silent preset.
        tree.removeChild (tree.getChildWithName (SceneIDs::scenesNode), nullptr);
        tree.appendChild (scenes, nullptr);
        return tree;
    }

    for (auto& def : getFactoryPresetDefs())
        if (def.name == entry.name)
            return buildFullStateTree (proc, def);

    return {};
}

void PresetManager::loadPreset (int index)
{
    if (index < 0 || index >= (int) presets.size())
        return;

    auto state = loadEntryState (presets[(size_t) index]);
    if (! state.isValid())
        return;

    // Ignore them on load as well as on save, so presets written before this rule -- and the
    // factory definitions, which are built from a full default parameter tree -- cannot move
    // the scene or flip ACTIVE either.
    stripPerformanceParams (state);

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (*xml, block);

    // Route through the exact same entry point the host uses to restore state (message-thread
    // only; setStateInformation itself only touches apvts.replaceState() + the two structural
    // fromValueTree() calls, none of which are called from processBlock).
    applyingPreset = true;
    proc.setStateInformation (block.getData(), (int) block.getSize());
    applyingPreset = false;

    currentIndex = index;
    dirty = false;
    deletedCurrentPresetName.clear();

    // Mirror now rather than waiting for the processor's timer. Both are message-thread work,
    // and doing it here means onPresetLoaded below -- which repaints the editor -- reads the
    // new preset's values instead of the previous one's for the next tick.
    proc.pumpSceneMirror();

    if (onPresetLoaded)
        onPresetLoaded();
}

void PresetManager::loadNext()
{
    if (presets.empty())
        return;
    // currentIndex == -1 (nothing selected, e.g. right after deleting the current user preset --
    // see deleteUserPreset()) resumes from the front of the list rather than indexing with a
    // negative value.
    if (currentIndex < 0)
    {
        loadPreset (0);
        return;
    }
    loadPreset ((currentIndex + 1) % (int) presets.size());
}

void PresetManager::loadPrevious()
{
    if (presets.empty())
        return;
    // currentIndex == -1 (see loadNext()) resumes from the back of the list.
    if (currentIndex < 0)
    {
        loadPreset ((int) presets.size() - 1);
        return;
    }
    loadPreset ((currentIndex - 1 + (int) presets.size()) % (int) presets.size());
}

void PresetManager::saveUserPreset (const juce::String& presetName)
{
    const juce::String trimmed = presetName.trim();
    if (trimmed.isEmpty())
        return;

    juce::MemoryBlock block;
    proc.getStateInformation (block);

    std::unique_ptr<juce::XmlElement> xml (juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize()));
    if (xml == nullptr)
        return;

    // A preset is a sound, not a position in the arrangement: strip the performance
    // parameters so recalling it never moves the scene or silences the plugin.
    {
        auto tree = juce::ValueTree::fromXml (*xml);
        if (tree.isValid())
        {
            stripPerformanceParams (tree);
            xml.reset (tree.createXml().release());
        }
    }

    auto dir = getUserPresetDirectory();
    auto file = dir.getChildFile (juce::File::createLegalFileName (trimmed) + kUserPresetFileExtension);
    xml->writeTo (file);

    currentNameOverride = trimmed;
    rebuildPresetList();
    dirty = false;
    deletedCurrentPresetName.clear();
}

bool PresetManager::deleteUserPreset (int index)
{
    if (index < 0 || index >= (int) presets.size())
        return false;

    const auto entry = presets[(size_t) index]; // copy: rebuildPresetList() below invalidates iterators/refs
    if (entry.isFactory || ! entry.userFile.existsAsFile())
        return false;

    const bool wasCurrent = (index == currentIndex);
    const bool deleted = entry.userFile.deleteFile();
    if (! deleted)
        return false;

    rebuildPresetList();

    if (wasCurrent)
    {
        // The named preset no longer exists on disk (and thus can't be found by name in the
        // refreshed list), but keep showing its name -- don't yank the display to whatever
        // preset happens to now occupy this index -- while marking the current state as
        // unsaved/dirty, since it's no longer backed by a file.
        deletedCurrentPresetName = entry.name;
        dirty = true;

        // rebuildPresetList()'s name-based re-find can't locate the now-deleted preset, so it
        // clamps currentIndex to whatever neighbour now occupies that slot -- that would make
        // getCurrentIndex() (and the preset-menu highlight) silently point at an unrelated
        // preset that doesn't match deletedCurrentPresetName. Use -1 as an explicit
        // "unselected" sentinel instead; getCurrentIndex()/loadNext()/loadPrevious() all treat
        // -1 safely (see their comments).
        currentIndex = -1;
    }

    return true;
}

} // namespace stutter
