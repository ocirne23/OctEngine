export module Entity:GameProjectileComponent;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;

// The game-layer PROJECTILE component. See GameUnitComponent.ixx for the design shared by the three
// game components and the authority/thread contract.

export struct GameProjectileParams
{
    float pushGain = 20000.0f; // field deflection (the force-ball pattern)
    float voidY = -20.0f;
};

// A shot: ages out, dies below the world, deflects in enemy fields, saps the nearest active enemy
// emitter while pressing near it (via addLoad), and on ANY contact (main-thread, routed from
// World::handleContactEvent) damages an enemy-team unit/structure it hit and despawns — no
// bouncing shells. Team-tagged: own-team actors pass free but still stop it.
export struct GameProjectileComponent
{
    static constexpr EComponentID getId() { return EComponentID_GameProjectile; }
    ~GameProjectileComponent() {}

    static GameProjectileParams params;

    struct SpawnInfo
    {
        uint32 team = 0;
        float unitDamage = 25.0f;      // on hitting an enemy unit
        float structureDamage = 20.0f; // on hitting an enemy structure
        float lifetime = 6.0f;
        float emitterDrain = 0.0f;     // energy/s sapped from the nearest active enemy emitter
        float emitterDrainRadius = 5.0f;
        float splashRadius = 0.0f;     // > 0: the contact damages EVERY enemy unit/structure within
                                       // it (the lobber shell), not just the one it touched
    };

    uint32 team = 0;
    float unitDamage = 25.0f, structureDamage = 20.0f;
    float lifetime = 6.0f;
    float emitterDrain = 0.0f, emitterDrainRadius = 5.0f;
    float splashRadius = 0.0f;
    float age = 0.0f;
    bool spent = false; // hit something — despawn queued, never damage twice

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info) {}
    void update(Entity& entity, float deltaSec);
    void onContact(Entity& self, Entity& other, bool begin); // main thread (physics contact dispatch)
};

export const GameProjectileComponent::SpawnInfo* getGameProjectileSpawnInfo(const Entity* entity);
export void writeGameProjectileSpawnInfo(const GameProjectileComponent::SpawnInfo& info, AssetNode& out);
