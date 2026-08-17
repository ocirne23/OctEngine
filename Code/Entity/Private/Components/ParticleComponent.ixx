export module Entity:ParticleComponent;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;
import Particle;

// A particle effect attached to the entity ("Component Particle" in .pre files): instantiates a .pfx
// effect (Particle library) and follows the entity's world transform every update, feeding its motion
// to the emitters (velocity inheritance/stretch). Gameplay toggles emission or fires bursts through
// the effect handle; the authored state is just the effect path + initial Emitting.
export struct ParticleComponent
{
    static constexpr EComponentID getId() { return EComponentID_Particle; }

    ~ParticleComponent() {}

    ParticleEffect effect;
    glm::vec3 lastPos = glm::vec3(0.0f); // world position last update (finite-difference emitter velocity)
    bool hasLastPos = false;

    struct SpawnInfo
    {
        oc::string effectPath; // .pfx, relative to Assets/
        bool emitting = true;   // initial continuous-emission state
    };

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, const Transform& world, float deltaSeconds);
};

export const ParticleComponent::SpawnInfo* getParticleSpawnInfo(const Entity* entity);

// Serializes a particle spawn recipe into a "Component Particle" node.
export void writeParticleSpawnInfo(const ParticleComponent::SpawnInfo& info, AssetNode& out);
