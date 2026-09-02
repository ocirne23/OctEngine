export module Entity:EntityNames;

import Core;

export class Entity;

// Entity display names live HERE, outside the Entity header: the entity carries no name field, and
// getName/setName/hasName forward to this registry keyed by the entity pointer. The stored string is
// the INTERNED copy (Profiler::internName): permanent and deduped, so a name pointer read from here
// doubles as the entity's ProfileScope name and stays valid in the profiler ring after the entity
// dies. Names repeat heavily (every "Enemy", every "Border"), so the intern pool stays tiny.
//
// Thread safety: set/erase run at spawn/rename/destroy time — the parallel spawn/destroy window,
// where several workers name entities at once — and get runs from the parallel entity pass (script
// thunks, the per-entity ProfileScope). Pointer-hashed SHARDS with one mutex each keep both paths
// uncontended; there is no lock-free read because a shard's map may rehash under a concurrent set.
export class EntityNameRegistry final
{
public:

    void set(const Entity* entity, oc::string_view name); // empty = unnamed (entry removed)
    const char* get(const Entity* entity) const;         // nullptr when unnamed; interned, permanent
    void erase(const Entity* entity);                    // Entity::destroy

private:

    static constexpr uint32 NUM_SHARDS = 64;

    struct Shard
    {
        mutable std::mutex mutex;
        oc::unordered_map<const Entity*, const char*> names;
    };

    Shard& shardFor(const Entity* entity) const
    {
        return m_shards[(uintptr_t(entity) >> 4) * 0x9E3779B97F4A7C15ull >> 58];
    }

    mutable oc::array<Shard, NUM_SHARDS> m_shards;
};

export namespace Globals
{
    // Plain ".CRT$XCU": constructs before, and destructs after, Globals::world (OC_SEG_WORLD), whose
    // dying root entities still erase their names. Holds no EntityPtr and calls no other global's
    // dtor, so it needs no InitSeg.h slot.
    EntityNameRegistry entityNames;
}
