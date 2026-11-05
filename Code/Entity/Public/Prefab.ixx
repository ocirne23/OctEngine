export module Entity:Prefab;

import Core;
import :Entity;

export bool savePrefab(Entity* root, const oc::string& path);

// Writes finished prefab text (the Entity Editor's document) to `path` and registers it. Refuses a cycle, like savePrefab.
export bool savePrefabText(const oc::string& path, const oc::string& text);

// True if saving `root` as prefab `id` would produce a cycle (a descendant is a prefab instance of `id`).
export bool prefabWouldCycle(Entity* root, const oc::string& id);
// The same check on prefab text.
export bool prefabTextWouldCycle(const oc::string& text, const oc::string& id);

// Serializes `root` to prefab text (the same format savePrefab() writes to disk) without touching the filesystem.
export oc::string serializePrefabText(Entity* root, const oc::string& id);
