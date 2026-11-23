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
    uint32 impactPuff = UINT32_MAX;  // the puff streams of an ImpactPuff shell's burst: its shards and its flash (the game
    uint32 impactFlash = UINT32_MAX; // makes them; UINT32_MAX = none)
    uint32 muzzlePuff = UINT32_MAX;  // ... of a MuzzleFlash shell's launch: its burst and its smoke
    uint32 muzzleSmoke = UINT32_MAX;
    uint32 fireImpactPuff = UINT32_MAX;  // ... of a FireImpact shot's burst (the mage's fireball): its embers and its flash
    uint32 fireImpactFlash = UINT32_MAX;
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
        float splashEdge = 1.0f;       // the splash damage's share at the edge of splashRadius: linear from the full damage at the centre (1 = flat)
        bool hitsShadows = false;      // its damage hurts a SHADOW too (a unit that is not solid: GameUnitComponent::damage)
        bool impactPuff = false;       // its impact bursts (params.impactPuff), on every role
        bool fireImpact = false;       // ... a FIRE burst instead (params.fireImpactPuff, an orange light), on every role
        bool muzzleFlash = false;      // its launch flashes where it starts: a burst, smoke and a short light, on every role
        float fieldDeflect = 0.0f;     // m/s^2 at full depth: an ENEMY bubble bends its flight out of it (the baked field;
                                       // for a shot with no Force emitter of its own - the cannon shell)
        float deflectDrain = 0.0f;     // battery energy/s it takes, while it is bent, from the enemy unit whose bubble bends it
        uint8 pierce = 0;              // > 0: it flies THROUGH units (its body must not collide with them) and hurts each enemy
                                       // it passes, up to this many (the ballista's bolt); without HitsShadows only SOLID ones
    };
    static constexpr int MaxPierce = 8;

    uint32 team = 0;
    float unitDamage = 25.0f, structureDamage = 20.0f;
    float lifetime = 6.0f;
    float emitterDrain = 0.0f, emitterDrainRadius = 5.0f;
    float splashRadius = 0.0f, splashEdge = 1.0f;
    bool hitsShadows = false, impactPuff = false, fireImpact = false, muzzleFlash = false;
    float fieldDeflect = 0.0f, deflectDrain = 0.0f;
    uint8 pierce = 0, pierced = 0;
    const void* pierceHits[MaxPierce] = {}; // the units it hurt already (compared only, never read)
    glm::vec3 lastPos{ 0.0f };              // the pierce's swept segment starts here (the last tick's position)
    glm::vec3 origin{ 0.0f }; // where it started (the muzzle flash's light)
    float age = 0.0f;
    bool spent = false; // hit something - despawn queued, never damage twice

    // THE IMPACT LIGHT of an ImpactPuff / FireImpact shot: it is gone at its impact, so `destroy` records the place (a small
    // ring under a mutex: destroy may run off the main thread) and the game draws them every frame with this - a bright
    // point light that fades out. Main thread, where a light record survives the frame (GameMatch::update). Every impact's
    // CRATER in the snow goes to the renderer here too (the same window rule).
    static void drawImpactLights();

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info); // the impact's sound cue
    void update(Entity& entity, float deltaSec);
    void onContact(Entity& self, Entity* other, bool begin); // other = null: a body with no entity (the terrain, a tree, a rock)
};

export const GameProjectileComponent::SpawnInfo* getGameProjectileSpawnInfo(const Entity* entity);
export void writeGameProjectileSpawnInfo(const GameProjectileComponent::SpawnInfo& info, AssetNode& out);
