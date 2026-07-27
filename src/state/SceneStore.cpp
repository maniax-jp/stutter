#include "SceneStore.h"
#include "SceneSchema.h"

namespace stutter
{

SceneStore::~SceneStore()
{
    // Drop the published pointer before the owning unique_ptrs go away, so a stray
    // concurrent read cannot observe a dangling bank during teardown. By this point the
    // processor is being destroyed and the audio thread is stopped, but making the ordering
    // explicit costs nothing and documents the invariant.
    live.store (nullptr, std::memory_order_release);
    retired.clear();
    liveOwned.reset();
}

void SceneStore::publish (std::unique_ptr<Bank> bank)
{
    if (bank == nullptr)
        return;

    const std::lock_guard<std::mutex> lock (writeMutex);

    const Bank* raw = bank.get();

    // Retire the outgoing bank BEFORE republishing, so it is always tracked: if the store
    // were reversed and an exception (or an early return) intervened, the old bank would be
    // unreachable from both `liveOwned` and `retired` and would leak.
    if (liveOwned != nullptr)
        retired.push_back ({ std::move (liveOwned), juce::Time::currentTimeMillis() });

    liveOwned = std::move (bank);

    // Release store pairs with the acquire load in get(): every write that built the bank
    // happens-before any audio-thread read of it.
    live.store (raw, std::memory_order_release);
}

void SceneStore::collectGarbage()
{
    const std::lock_guard<std::mutex> lock (writeMutex);

    if (retired.empty())
        return;

    const juce::int64 now = juce::Time::currentTimeMillis();
    for (auto it = retired.begin(); it != retired.end();)
    {
        if (now - it->retiredAtMs >= retireGraceMillis)
            it = retired.erase (it);
        else
            ++it;
    }
}

int SceneStore::getPendingRetireCount() const
{
    const std::lock_guard<std::mutex> lock (writeMutex);
    return (int) retired.size();
}

void SceneStore::rebuildFromTree (const juce::ValueTree& scenesTree)
{
    auto bank = createBank();

    // Every slot starts as a default-constructed, unpopulated scene. Malformed or missing
    // entries therefore read as "empty slot" rather than as garbage, which is what lets the
    // gesture layer ignore notes mapped to nothing instead of triggering silence.
    for (int i = 0; i < scenesTree.getNumChildren(); ++i)
    {
        const auto sceneTree = scenesTree.getChild (i);
        if (! sceneTree.hasType (SceneIDs::scene))
            continue;

        const int index = (int) sceneTree.getProperty (SceneIDs::index, -1);
        if (index < 0 || index >= maxScenes)
            continue;

        bank->scenes[(size_t) index] = SceneSchema::sceneFromTree (sceneTree);
    }

    publish (std::move (bank));
}

} // namespace stutter
