module Entity;

import Core;
import Core.Profiler;
import :EntityNames;

void EntityNameRegistry::set(const Entity* entity, oc::string_view name)
{
    assert(!name.empty());

    oc::unique_ptr<char[]> copy = oc::make_unique<char[]>(name.size() + 1);
    oc::char_traits<char>::copy(copy.get(), name.data(), name.size());
    copy[name.size()] = '\0';

    Shard& shard = shardFor(entity);
    {
        std::lock_guard lock(shard.mutex);
		shard.names[entity] = oc::move(copy);
    }
}

const char* EntityNameRegistry::get(const Entity* entity) const
{
    Shard& shard = shardFor(entity);
    std::lock_guard lock(shard.mutex);
    const auto it = shard.names.find(entity);
    return it != shard.names.end() ? it->second.get() : nullptr;
}

void EntityNameRegistry::erase(const Entity* entity)
{
    Shard& shard = shardFor(entity);
    oc::unique_ptr<char[]> retired;
    {
        std::lock_guard lock(shard.mutex);
        const auto it = shard.names.find(entity);
        if (it != shard.names.end())
        {
            retired = oc::move(it->second);
            shard.names.erase(it);
        }
    }
}
