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
        m_listenersByScript.insert(script, { entity, scriptData });
    }

    void unregisterListener(const ScriptModule* script, Entity* entity)
    {
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