export module Entity:Prefab;

import Core;
import :Entity;

// `text` overrides the serialized content when non-empty (still cycle-checked against `root`) — the
// Entity Editor uses it to save its detached transform draft instead of the live world transform.
export bool savePrefab(Entity* root, const oc::string& path, const oc::string& text = {});

// True if saving `root` as prefab `id` would produce a cycle (a descendant is a prefab instance of `id`).
export bool prefabWouldCycle(Entity* root, const oc::string& id);

// Serializes `root` to prefab text (the same format savePrefab() writes to disk) without touching the
// filesystem — used to build/compare in-memory dirty-tracking baselines (e.g. the Prefab Editor UI).
export oc::string serializePrefabText(Entity* root, const oc::string& id);
