module Entity;

import Core;
import Core.glm;
import Core.Transform;
import :Entity;
import Particle;

void ParticleComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    if (info.effectPath.empty())
        return;
    effect = Globals::particleSystem.createEffect(info.effectPath, base.pos, base.quat);
    if (effect.isValid() && !info.emitting)
        effect.setEmitting(false);
}

void ParticleComponent::destroy(Entity& entity, const SpawnInfo&)
{
    effect.destroy(); // live particles retire over the next frames
}

void ParticleComponent::update(Entity& entity, const Transform& world, float deltaSeconds)
{
    if (!effect.isValid())
        return;
    effect.setTransform(world.pos, world.quat);
    if (hasLastPos && deltaSeconds > 1e-5f)
        effect.setVelocity((world.pos - lastPos) / deltaSeconds);
    lastPos = world.pos;
    hasLastPos = true;
}

const ParticleComponent::SpawnInfo* getParticleSpawnInfo(const Entity* entity)
{
    return getSpawnInfo<ParticleComponent>(entity);
}

void writeParticleSpawnInfo(const ParticleComponent::SpawnInfo& info, AssetNode& out)
{
    if (info.effectPath.empty())
        return;
    out.set("Effect", info.effectPath);
    if (!info.emitting)
        out.set("Emitting", info.emitting);
}
