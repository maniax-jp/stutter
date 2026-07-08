#include "PresetManager.h"
#include "PluginProcessor.h"
#include "FactoryPresets.h"
#include "dsp/ParameterIDs.h"

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
juce::ValueTree buildFullStateTree (juce::AudioProcessorValueTreeState& apvts, const FactoryPresetDef& def)
{
    auto tree = buildDefaultParametersTree (apvts);

    for (auto& pv : def.paramValues)
        setParamValue (tree, pv.paramId, pv.value);

    tree.appendChild (buildSequencerTree (def.stepsOn), nullptr);
    tree.appendChild (buildCurvesTree (def.curves), nullptr);

    return tree;
}
}

//==============================================================================
PresetManager::PresetManager (StutterAudioProcessor& processor) : proc (processor)
{
    rebuildPresetList();
    proc.getAPVTS().state.addListener (this);
}

void PresetManager::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&)
{
    if (! applyingPreset)
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

    presets.push_back ({ "Init", "Init", true, {} });

    for (auto& def : getFactoryPresetDefs())
        presets.push_back ({ def.name, def.category, true, {} });

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
        return buildFullStateTree (proc.getAPVTS(), initDef);
    }

    for (auto& def : getFactoryPresetDefs())
        if (def.name == entry.name)
            return buildFullStateTree (proc.getAPVTS(), def);

    return {};
}

void PresetManager::loadPreset (int index)
{
    if (index < 0 || index >= (int) presets.size())
        return;

    auto state = loadEntryState (presets[(size_t) index]);
    if (! state.isValid())
        return;

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

    if (onPresetLoaded)
        onPresetLoaded();
}

void PresetManager::loadNext()
{
    if (presets.empty())
        return;
    loadPreset ((currentIndex + 1) % (int) presets.size());
}

void PresetManager::loadPrevious()
{
    if (presets.empty())
        return;
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

    auto dir = getUserPresetDirectory();
    auto file = dir.getChildFile (juce::File::createLegalFileName (trimmed) + kUserPresetFileExtension);
    xml->writeTo (file);

    currentNameOverride = trimmed;
    rebuildPresetList();
    dirty = false;
}

} // namespace stutter
