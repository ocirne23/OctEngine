export module Entity:GameProjectileComponent;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;

export struct GameProjectileParams
{
    float pushGain = 20000.0f; // field deflection (the force-ball pattern)
    float voidY = -20.0f;
};

// Authority only (clients early-out of update); onContact is main-thread (physics contact dispatch).
export struct GameProjectileComponent
{
    static constexpr EComponentID getId() { return EComponentID_GameProjectile; }
    ~GameProjectileComponent() {}

    static GameProjectileParams params;

    struct SpawnInfo
    {
        uint32 team = 0;
        float unitDamage = 25.0f;
        float structureDamage = 20.0f;
        float lifetime = 6.0f;
        float emitterDrain = 0.0f;     // energy/s sapped from the nearest active enemy emitter
        float emitterDrainRadius = 5.0f;
        float splashRadius = 0.0f;     // > 0: the contact damages EVERY enemy within it (the lobber shell)
    };

    uint32 team = 0;
    float unitDamage = 25.0f, structureDamage = 20.0f;
    float lifetime = 6.0f;
    float emitterDrain = 0.0f, emitterDrainRadius = 5.0f;
    float splashRadius = 0.0f;
    float age = 0.0f;
    bool spent = false; // hit something - despawn queued, never damage twice

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info) {}
    void update(Entity& entity, float deltaSec);
    void onContact(Entity& self, Entity& other, bool begin);
};

export const GameProjectileComponent::SpawnInfo* getGameProjectileSpawnInfo(const Entity* entity);
export void writeGameProjectileSpawnInfo(const GameProjectileComponent::SpawnInfo& info, AssetNode& out);
