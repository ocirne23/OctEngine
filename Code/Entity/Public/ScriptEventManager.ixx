export module Entity:ScriptEventManager;

import Core;
import Core.glm;

import :ScriptContext;
import :Entity;
import :ScriptEventManagerState; // the event keys, the listener registry and the change queue
import Script;

// Global named script events (Globals::scriptEvents) and the deferred, thread-safe queue of script-driven entity changes (destroy, reparent, spawn requests)
// that the main loop drains through takeEntityChanges(). The state is in ScriptEventManagerState.ixx; the multi-line bodies are below the class.
export class ScriptEventManager final : public ScriptEventManagerState
{
public:

    using EventKey = ScriptEventManagerState::EventKey;

    void initialize(); // from main, before any scripted entity spawns

    // ---- Events ----

    // A name maps to a uint32 key, so a fire is one key lookup plus a dispatch - not a string compare per listener.
    EventKey getEventKeyForName(const oc::string& eventName); // interns
    EventKey findEventKey(const oc::string& eventName) const; // 0 = never interned
    void fireEvent(EventKey key);
    void fireEvent(const oc::string& eventName)
    {
        if (const EventKey key = findEventKey(eventName))
            fireEvent(key);
    }

    // ---- Deferred entity changes (any thread; the script thunks queue, main drains) ----

    oc::vector<EntityChange> takeEntityChanges();
    void addDestroyRequest(EntityPtr&& entity);
    void addReparentRequest(EntityPtr&& entity, EntityPtr&& newParent);
    void addSpawnRequest(oc::string path, const glm::vec3& position);
};

export namespace Globals
{
// The deferred EntityChange queue holds EntityPtrs (requests queued after the last drain) - releasing them needs World's spawn templates, so ~World (XCU8,
// destructs first) empties the queue; this object itself must outlive ~World, whose dying ScriptComponents unregister here.
OC_INIT_SEG(OC_SEG_SCRIPT_EVENTS)
    ScriptEventManager scriptEvents;
}

// ---- Inline bodies ----

inline void ScriptEventManager::initialize()
{
    Globals::scriptHost.m_scriptLoadedCallback = [](const ScriptModule* script, const oc::vector<oc::string>& oldNames) { Globals::scriptEvents.onScriptLoadedCallback(script, oldNames); };
}

inline ScriptEventManager::EventKey ScriptEventManager::getEventKeyForName(const oc::string& eventName)
{
    {
        const std::shared_lock<std::shared_mutex> read(m_eventKeyMutex);
        if (auto it = m_eventNameKeyLookup.find(eventName); it != m_eventNameKeyLookup.end())
            return it->second;
    }
    const std::lock_guard<std::shared_mutex> write(m_eventKeyMutex);
    const auto [it, inserted] = m_eventNameKeyLookup.emplace(eventName, m_nextEventKey);
    if (inserted)
        ++m_nextEventKey;
    return it->second;
}

inline ScriptEventManager::EventKey ScriptEventManager::findEventKey(const oc::string& eventName) const
{
    const std::shared_lock<std::shared_mutex> read(m_eventKeyMutex);
    const auto it = m_eventNameKeyLookup.find(eventName);
    return it != m_eventNameKeyLookup.end() ? it->second : 0;
}

inline oc::vector<EntityChange> ScriptEventManager::takeEntityChanges()
{
    if (m_entityChanges.empty())
        return {};
    oc::vector<EntityChange> changes;
    const std::lock_guard<std::mutex> lock(m_entityChangeMutex);
    changes.swap(m_entityChanges);
    return changes;
}

inline void ScriptEventManager::addDestroyRequest(EntityPtr&& entity)
{
    const std::lock_guard<std::mutex> lock(m_entityChangeMutex);
    m_entityChanges.emplace_back(EntityChange::Delete{ oc::move(entity) });
}

inline void ScriptEventManager::addReparentRequest(EntityPtr&& entity, EntityPtr&& newParent)
{
    const std::lock_guard<std::mutex> lock(m_entityChangeMutex);
    m_entityChanges.emplace_back(EntityChange::Reparent{ oc::move(entity), oc::move(newParent) });
}

inline void ScriptEventManager::addSpawnRequest(oc::string path, const glm::vec3& position)
{
    const std::lock_guard<std::mutex> lock(m_entityChangeMutex);
    m_entityChanges.emplace_back(EntityChange::SpawnAtPosition{ oc::move(path), position });
}
