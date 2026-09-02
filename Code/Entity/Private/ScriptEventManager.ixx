export module Entity:ScriptEventManager;

import Core;
import Core.LPMultiMap;

import :ScriptContext;
import :Entity;
import Script;

class Entity;
export class ScriptEventManager
{
public:

    using EventKey = uint32;

    void initialize();

    EventKey getEventKeyForName(const oc::string& eventName)
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

    EventKey findEventKey(const oc::string& eventName) const
    {
        const std::shared_lock<std::shared_mutex> read(m_eventKeyMutex);
        const auto it = m_eventNameKeyLookup.find(eventName);
        return it != m_eventNameKeyLookup.end() ? it->second : 0;
    }

    void fireEvent(EventKey key);

	void fireEvent(const oc::string& eventName)
	{
		if (const EventKey key = findEventKey(eventName))
			fireEvent(key);
	}

    oc::vector<EntityChange> takeEntityChanges()
    {
        if (m_entityChanges.empty())
            return {};

		oc::vector<EntityChange> changes;
		const std::lock_guard<std::mutex> lock(m_entityChangeMutex);
		changes.swap(m_entityChanges);
		return changes;
    }

	inline void addDestroyRequest(EntityPtr&& entity)
	{
		const std::lock_guard<std::mutex> lock(m_entityChangeMutex);
		m_entityChanges.emplace_back(EntityChange::Delete{ oc::move(entity) });
	}

	inline void addReparentRequest(EntityPtr&& entity, EntityPtr&& newParent)
	{
		const std::lock_guard<std::mutex> lock(m_entityChangeMutex);
		m_entityChanges.emplace_back(EntityChange::Reparent{ oc::move(entity), oc::move(newParent) });
	}

	inline void addSpawnRequest(oc::string path, const glm::vec3& position)
	{
		const std::lock_guard<std::mutex> lock(m_entityChangeMutex);
		m_entityChanges.emplace_back(EntityChange::SpawnAtPosition{ oc::move(path), position });
	}

private:

    void onScriptLoadedCallback(const ScriptModule* script, const oc::vector<oc::string>& oldNames);

	friend class ScriptComponent;
    void registerListener(const ScriptModule* script, Entity* entity, void* scriptData)
    {
        const std::lock_guard lock(m_listenerMutex); // parallel entity spawning
        m_listenersByScript.insert(script, { entity, scriptData });
    }

    // waitForDispatches: the ENTITY DESTROY path (Entity::destroy calls this before any component
    // is torn down). fireEvent invokes its snapshot outside the lock, so a dispatch that already
    // captured this entity may still be running on another worker (parallel destruction): wait
    // until every in-flight dispatch has drained before the teardown proceeds — the entity is
    // then either invoked while still fully alive, or never. Never set from inside a dispatch (a
    // script's re-registration through syncScriptDataLive passes false); destroys are deferred
    // requests, so no dispatch ever reaches a destroy on its own thread.
    void unregisterListener(const ScriptModule* script, Entity* entity, bool waitForDispatches = false)
    {
        {
            const std::lock_guard lock(m_listenerMutex); // parallel entity spawning
            auto range = m_listenersByScript.equalRange(script);
            for (auto it = range.begin(); it != range.end();)
            {
                if (it->second.entity == entity)
                {
                    m_listenersByScript.eraseOne(it);
                    break;
                }
                else
                    ++it;
            }
        }
        if (waitForDispatches)
            while (m_dispatching.load(oc::memory_order_acquire) != 0)
                std::this_thread::yield();
    }

private:

    oc::vector<EntityChange> m_entityChanges;
    std::mutex m_entityChangeMutex;

    struct Entry
    {
		Entity* entity = nullptr;
		void* scriptData = nullptr;
    };

	oc::unordered_map<oc::string, EventKey> m_eventNameKeyLookup;
	EventKey m_nextEventKey = 1;
	mutable std::shared_mutex m_eventKeyMutex; // both guarded by this; only script load ever writes

	// Parallel entity spawning: a spawn job's OnSpawn can fire an event while another job's
	// ScriptComponent::spawn registers a listener — both structures serialize here. Held only for
	// map reads/writes: fireEvent SNAPSHOTS the dispatch list and invokes the scripts after
	// releasing, so nested fires / re-registration from inside OnEvent re-lock freshly.
	std::mutex m_listenerMutex;
	oc::atomic<int> m_dispatching = 0; // fireEvent invocations in flight (see unregisterListener)
	oc::unordered_map<EventKey, oc::vector<const ScriptModule*>> m_listenersByEvent;
	LPMultiMap<const ScriptModule*, Entry> m_listenersByScript;
};

export namespace Globals
{
// The deferred EntityChange queue holds EntityPtrs (requests queued after the last drain) —
// releasing them needs the job system and networkManager still alive, see InitSeg.h.
OC_INIT_SEG(OC_SEG_SCRIPT_EVENTS)
    ScriptEventManager scriptEvents;
}

inline void ScriptEventManager::initialize()
{
    Globals::scriptHost.m_scriptLoadedCallback = [](const ScriptModule* script, const oc::vector<oc::string>& oldNames) { Globals::scriptEvents.onScriptLoadedCallback(script, oldNames); };
}