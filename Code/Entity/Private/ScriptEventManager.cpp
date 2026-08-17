module;

#include "ScriptAPI.h"

module Entity;

import Script;
import :ScriptComponent; // invokeScriptOnEvent (module-linkage, declared next to the component)

void ScriptEventManager::fireEvent(EventKey key)
{
	for (auto it = m_listenersByEvent.find(key); it != m_listenersByEvent.end() && it->first == key; ++it)
	{
		for (const ScriptModule* script : it->second)
		{
			auto range = m_listenersByScript.equalRange(script);
			for (auto sit = range.begin(); sit != range.end(); ++sit)
			{
				auto eventIt = script->eventKeyToIndex.find(key);
				if (eventIt == script->eventKeyToIndex.end())
					continue;
				if (sit->second.entity->isFrozen())
					continue;
				// This path calls the script's OnEvent DIRECTLY, bypassing ScriptComponent's own entry points,
				// so it has to honour //@@require and the fault gate itself (see ScriptComponent::requirementsMet).
				if (script->faulted || (script->requiredComponents & ~uint32(sit->second.entity->typeBits)))
					continue;
				invokeScriptOnEvent(script, *sit->second.entity, eventIt->second, sit->second.scriptData);
			}
		}
	}
}

void ScriptEventManager::onScriptLoadedCallback(const ScriptModule* script, const oc::vector<oc::string>& oldNames)
{
	const oc::vector<oc::string>& newNames = script->eventNames;

	// Drop this script only from buckets for names it no longer has.
	for (const oc::string& oldName : oldNames)
	{
		if (oc::find(newNames.begin(), newNames.end(), oldName) != newNames.end())
			continue; // still present — keep the existing registration
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