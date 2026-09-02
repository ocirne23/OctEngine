module Entity;

import Core;
import Core.Profiler;
import :EntityNames;

void EntityNameRegistry::set(const Entity* entity, oc::string_view name)
{
    // Intern OUTSIDE the shard lock: the profiler has its own mutex, and a name that repeats is a
    // hash-set hit there. Runs only at spawn/rename time - never in the update path.
    const char* interned = name.empty() ? nullptr : Globals::profiler.internName(name);
    Shard& shard = shardFor(entity);
    std::lock_guard lock(shard.mutex);
    if (interned)
        shard.names[entity] = interned;
    else
        shard.names.erase(entity);
}

const char* EntityNameRegistry::get(const Entity* entity) const
{
    Shard& shard = shardFor(entity);
    std::lock_guard lock(shard.mutex);
    const auto it = shard.names.find(entity);
    return it != shard.names.end() ? it->second : nullptr;
}

void EntityNameRegistry::erase(const Entity* entity)
{
    Shard& shard = shardFor(entity);
    std::lock_guard lock(shard.mutex);
    shard.names.erase(entity);
}
