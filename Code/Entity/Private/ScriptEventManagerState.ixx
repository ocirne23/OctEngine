export module Entity:ScriptEventManagerState;

import Core;
import Core.LPMultiMap;

import :Entity;
import Script;

export struct ScriptComponent; // declared before its friend line
export class ScriptEventManager;

// Everything of ScriptEventManager but its public API (ScriptEventManager.ixx): the event-name keys, the listener registry and the deferred change queue.
export class ScriptEventManagerState
{
protected:

    friend struct ScriptComponent; // registers / unregisters its listeners

    using EventKey = uint32;

    template<typename M = ScriptEventManager> M& self() { return static_cast<M&>(*this); }

    void onScriptLoadedCallback(const ScriptModule* script, const oc::vector<oc::string>& oldNames);

    void registerListener(const ScriptModule* script, Entity* entity, void* scriptData)
    {
        const std::lock_guard lock(m_listenerMutex); // parallel entity spawning
        m_listenersByScript.insert(script, { entity, scriptData });
    }

    // waitForDispatches: the ENTITY DESTROY path (Entity::destroy calls this before any component is torn down). fireEvent invokes its snapshot outside the
    // lock, so a dispatch that already captured this entity may still be running on another worker (parallel destruction): wait until every in-flight
    // dispatch has drained before the teardown proceeds - the entity is then either invoked while still fully alive, or never. Never set from inside a
    // dispatch (a script's re-registration through syncScriptDataLive passes false); destroys are deferred requests, so no dispatch ever reaches a destroy
    // on its own thread.
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

    // Parallel entity spawning: a spawn job's OnSpawn can fire an event while another job's ScriptComponent::spawn registers a listener - both structures
    // serialize here. Held only for map reads/writes: fireEvent SNAPSHOTS the dispatch list and invokes the scripts after releasing, so nested fires /
    // re-registration from inside OnEvent re-lock freshly.
    std::mutex m_listenerMutex;
    oc::atomic<int> m_dispatching = 0; // fireEvent invocations in flight (see unregisterListener)
    oc::unordered_map<EventKey, oc::vector<const ScriptModule*>> m_listenersByEvent;
    LPMultiMap<const ScriptModule*, Entry> m_listenersByScript;
};
