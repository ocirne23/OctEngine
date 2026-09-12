module;

#include "ScriptAPI.h"

module Entity;

import Core.Time;
import Script;
import :ScriptComponent; // invokeScriptOnEvent (module-linkage, declared next to the component)

void ScriptEventManager::fireEvent(EventKey key)
{
	if (Globals::time.isPaused())
		return; // global pause: script events don't fire, matching the Frozen rule per entity

	// SNAPSHOT under the lock, INVOKE outside it: the scripts themselves can run long, fire nested
	// events (re-entering here on the same thread) or re-register their listener - holding the lock
	// across the invokes would serialize every event dispatch engine-wide. Safe against a
	// concurrent (parallel) entity destroy because that path's unregisterListener WAITS for
	// m_dispatching to drain before any component is torn down.
	struct Dispatch
	{
		const ScriptModule* script;
		Entity* entity;
		void* scriptData;
		int eventIdx;
	};
	oc::small_vector<Dispatch, 16> dispatches;
	// Counted BEFORE the snapshot: a destroy that erases its listener after this point sees the
	// count and waits; one that erased before it is simply not in the snapshot.
	m_dispatching.fetch_add(1, oc::memory_order_acq_rel);
	{
		const std::lock_guard lock(m_listenerMutex); // parallel entity spawning (see the member comment)
		for (auto it = m_listenersByEvent.find(key); it != m_listenersByEvent.end() && it->first == key; ++it)
		{
			for (const ScriptModule* script : it->second)
			{
				auto eventIt = script->eventKeyToIndex.find(key); // rebuilt under this lock on reload
				if (eventIt == script->eventKeyToIndex.end())
					continue;
				auto range = m_listenersByScript.equalRange(script);
				for (auto sit = range.begin(); sit != range.end(); ++sit)
					dispatches.push_back({ script, sit->second.entity, sit->second.scriptData, eventIt->second });
			}
		}
	}
	for (const Dispatch& d : dispatches)
	{
		if (d.entity->isFrozen())
			continue;
		// This path calls the script's OnEvent DIRECTLY, bypassing ScriptComponent's own entry points,
		// so it has to honour //@@require and the fault gate itself (see ScriptComponent::requirementsMet).
		if (d.script->faulted || (d.script->requiredComponents & ~uint32(d.entity->typeBits)))
			continue;
		invokeScriptOnEvent(d.script, *d.entity, d.eventIdx, d.scriptData);
	}
	m_dispatching.fetch_sub(1, oc::memory_order_acq_rel);
}

void ScriptEventManager::onScriptLoadedCallback(const ScriptModule* script, const oc::vector<oc::string>& oldNames)
{
	// Parallel entity spawning: a spawn job's getOrLoad miss lands here while other jobs fire
	// events / register listeners.
	const std::lock_guard lock(m_listenerMutex);
	const oc::vector<oc::string>& newNames = script->eventNames;

	// Drop this script only from buckets for names it no longer has.
	for (const oc::string& oldName : oldNames)
	{
		if (oc::find(newNames.begin(), newNames.end(), oldName) != newNames.end())
			continue; // still present - keep the existing registration
		auto keyIt = m_eventNameKeyLookup.find(oldName);
		if (keyIt == m_eventNameKeyLookup.end())
			continue;
		auto bucketIt = m_listenersByEvent.find(keyIt->second);
		if (bucketIt != m_listenersByEvent.end())
			oc::erase(bucketIt->second, script);
	}

	// Register it only under names it didn't have before, minting a key for any never-seen name.
	for (const oc::string& newName : newNames)
	{
		if (oc::find(oldNames.begin(), oldNames.end(), newName) != oldNames.end())
			continue; // already registered from the previous load
		m_listenersByEvent[getEventKeyForName(newName)].push_back(script);
	}

	// Rebuild the script's own EventKey -> local OnEvent index map from scratch: a reload can reorder or
	// resize the entry list, so even a surviving name's index may have moved. fireEvent uses this to turn a
	// fired EventKey into the eventIdx the script's compiled OnEvent switch expects.
	script->eventKeyToIndex.clear();
	for (int i = 0; i < (int)newNames.size(); ++i)
		script->eventKeyToIndex[getEventKeyForName(newNames[i])] = i;
}