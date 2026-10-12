module Particle;

import Core;
import Core.glm;
import File;
import RendererVK;
import :Effect;

using namespace RendererVKLayout;

ParticleEmitterGpu ParticleEmitterDesc::toGpu(uint16 textureIdx) const
{
    ParticleEmitterGpu gpu;
    gpu.posSpawnRadius = glm::vec4(0.0f, 0.0f, 0.0f, spawnRadius); // position set per instance
    gpu.rotation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);              // rotation set per instance
    gpu.velocityInherit = glm::vec4(0.0f, 0.0f, 0.0f, inheritVelocity);
    // The shaders read these fields unguarded: every floor / range the GPU relies on is applied here.
    gpu.spawnParams = glm::vec4(glm::radians(glm::clamp(coneAngleDeg, 0.0f, 180.0f)), speedMin, speedMax, spawnShell);
    gpu.lifeParams = glm::vec4(glm::max(lifeMin, 0.01f), glm::max(lifeMax, 0.01f), gravity, drag);
    gpu.noiseParams = glm::vec4(turbulence, turbulenceFrequency, turbulenceScroll, collisionBounce);
    gpu.sizeParams = glm::vec4(glm::max(sizeStart, 0.0f), glm::max(sizeEnd, 0.0f), glm::clamp(sizeVariance, 0.0f, 1.0f), velocityStretch);
    gpu.colorStart = colorStart;
    gpu.colorEnd = colorEnd;
    // x > 0 (the fade-in divide); y < 1 (smoothstep(y, 1) needs edge0 < edge1, and the sim's water-floor age
    // push max(age, life * y) stays below the life); w = 1 / soft fade distance.
    gpu.fadeParams = glm::vec4(glm::max(fadeIn, 1e-3f), glm::min(fadeOutStart, 0.999f), additivity,
        1.0f / glm::max(softFadeDistance, 1e-3f));
    gpu.spinParams = glm::vec4(spinMax, randomRotation ? 1.0f : 0.0f, emissiveFloor, heightFalloff > 0.0f ? glm::max(heightFalloff, 0.01f) : 0.0f);
    uint32 flags = 0;
    if (heightFalloff > 0.0f)
        flags |= PARTICLE_FLAG_GROUND_FADE;
    if (lit)
        flags |= PARTICLE_FLAG_LIT;
    if (collide)
        flags |= PARTICLE_FLAG_COLLIDE;
    if (waterFloor)
        flags |= PARTICLE_FLAG_WATER_FLOOR;
    if (isVolume())
    {
        flags |= PARTICLE_FLAG_VOLUME;
        if (occlude)
            flags |= PARTICLE_FLAG_OCCLUDE;
        gpu.volumeParams = glm::vec4(glm::max(volume, glm::vec3(1e-3f)), glm::max(windResponse, 0.0f));
    }
    // Water-side flags apply to every emitter: the sim's relocation is volume-only by itself, the draw's
    // camera gate is not (the ocean spray is a non-volume emitter that hides under the sea).
    if (underwater)
        flags |= PARTICLE_FLAG_UNDERWATER;
    if (aboveWater)
        flags |= PARTICLE_FLAG_ABOVE_WATER;
    const uint32 flipbook = (flipbookCols > 0 && flipbookRows > 0) ? (flipbookCols | (flipbookRows << 16)) : 0;
    gpu.texFlags = glm::uvec4(textureIdx, flags, flipbook, glm::floatBitsToUint(flipbookFps));
    return gpu;
}

static glm::vec4 nodeAsVec4(const AssetNode& node, const glm::vec4& fallback)
{
    glm::vec4 v = fallback;
    for (size_t i = 0; i < 4 && i < node.numValues(); ++i)
        v[(glm::length_t)i] = node.asFloat(i, fallback[(glm::length_t)i]);
    return v;
}

static void parseEmitter(const AssetNode& node, ParticleEmitterDesc& e)
{
    e.name = node.asString();
    if (const AssetNode* n = node.find("Texture"))       { e.texturePath = n->asString(); e.textureSRGB = n->asBool(1, true); }
    if (const AssetNode* n = node.find("Flipbook"))      { e.flipbookCols = (uint32)n->asInt(0); e.flipbookRows = (uint32)n->asInt(1); e.flipbookFps = n->asFloat(2, 10.0f); }
    if (const AssetNode* n = node.find("ColorStart"))    e.colorStart = nodeAsVec4(*n, e.colorStart);
    if (const AssetNode* n = node.find("ColorEnd"))      e.colorEnd = nodeAsVec4(*n, e.colorEnd);
    if (const AssetNode* n = node.find("Additivity"))    e.additivity = n->asFloat(0, e.additivity);
    if (const AssetNode* n = node.find("Lit"))           e.lit = n->asBool(0, e.lit);
    if (const AssetNode* n = node.find("EmissiveFloor")) e.emissiveFloor = n->asFloat(0, e.emissiveFloor);
    if (const AssetNode* n = node.find("Rate"))          e.rate = n->asFloat(0, e.rate);
    if (const AssetNode* n = node.find("Burst"))         e.burst = (uint32)n->asInt(0);
    if (const AssetNode* n = node.find("SpawnRadius"))   e.spawnRadius = n->asFloat(0, e.spawnRadius);
    if (const AssetNode* n = node.find("SpawnShell"))    e.spawnShell = n->asFloat(0, e.spawnShell);
    if (const AssetNode* n = node.find("ConeAngle"))     e.coneAngleDeg = n->asFloat(0, e.coneAngleDeg);
    if (const AssetNode* n = node.find("Speed"))         { e.speedMin = n->asFloat(0, e.speedMin); e.speedMax = n->asFloat(1, e.speedMin); }
    if (const AssetNode* n = node.find("Offset"))        e.localOffset = n->asVec3(e.localOffset);
    if (const AssetNode* n = node.find("Direction"))     e.localDirection = n->asVec3(e.localDirection);
    if (const AssetNode* n = node.find("InheritVelocity")) e.inheritVelocity = n->asFloat(0, e.inheritVelocity);
    if (const AssetNode* n = node.find("Life"))          { e.lifeMin = n->asFloat(0, e.lifeMin); e.lifeMax = n->asFloat(1, e.lifeMin); }
    if (const AssetNode* n = node.find("Gravity"))       e.gravity = n->asFloat(0, e.gravity);
    if (const AssetNode* n = node.find("Drag"))          e.drag = n->asFloat(0, e.drag);
    if (const AssetNode* n = node.find("Turbulence"))    { e.turbulence = n->asFloat(0, e.turbulence); e.turbulenceFrequency = n->asFloat(1, e.turbulenceFrequency); e.turbulenceScroll = n->asFloat(2, e.turbulenceScroll); }
    if (const AssetNode* n = node.find("Collide"))       e.collide = n->asBool(0, e.collide);
    if (const AssetNode* n = node.find("Bounce"))        e.collisionBounce = n->asFloat(0, e.collisionBounce);
    if (const AssetNode* n = node.find("WaterFloor"))    e.waterFloor = n->asBool(0, e.waterFloor);
    if (const AssetNode* n = node.find("Size"))          { e.sizeStart = n->asFloat(0, e.sizeStart); e.sizeEnd = n->asFloat(1, e.sizeStart); }
    if (const AssetNode* n = node.find("SizeVariance"))  e.sizeVariance = n->asFloat(0, e.sizeVariance);
    if (const AssetNode* n = node.find("VelocityStretch")) e.velocityStretch = n->asFloat(0, e.velocityStretch);
    if (const AssetNode* n = node.find("Spin"))          e.spinMax = n->asFloat(0, e.spinMax);
    if (const AssetNode* n = node.find("RandomRotation")) e.randomRotation = n->asBool(0, e.randomRotation);
    if (const AssetNode* n = node.find("FadeIn"))        e.fadeIn = n->asFloat(0, e.fadeIn);
    if (const AssetNode* n = node.find("FadeOutStart"))  e.fadeOutStart = n->asFloat(0, e.fadeOutStart);
    if (const AssetNode* n = node.find("SoftFade"))      e.softFadeDistance = n->asFloat(0, e.softFadeDistance);
    if (const AssetNode* n = node.find("Volume"))        e.volume = n->asVec3(e.volume);
    if (const AssetNode* n = node.find("Count"))         e.count = (uint32)n->asInt(0);
    if (const AssetNode* n = node.find("FollowCamera"))  e.followCamera = n->asBool(0, e.followCamera);
    if (const AssetNode* n = node.find("Occlude"))       e.occlude = n->asBool(0, e.occlude);
    if (const AssetNode* n = node.find("WindResponse"))  e.windResponse = n->asFloat(0, e.windResponse);
    if (const AssetNode* n = node.find("Underwater"))    e.underwater = n->asBool(0, e.underwater);
    if (const AssetNode* n = node.find("AboveWater"))    e.aboveWater = n->asBool(0, e.aboveWater);
    if (const AssetNode* n = node.find("HeightFalloff")) e.heightFalloff = n->asFloat(0, e.heightFalloff);
}

bool loadParticleEffect(const oc::string& path, ParticleEffectDesc& outDesc, oc::string& outError)
{
    AssetNode root;
    if (!loadAssetFile(path, root, outError))
        return false;
    const AssetNode* effectNode = root.find("ParticleEffect");
    if (!effectNode)
    {
        outError = path + ": no ParticleEffect entry";
        return false;
    }
    outDesc = {};
    outDesc.name = effectNode->asString();
    for (const AssetNode* emitterNode : effectNode->findAll("Emitter"))
    {
        ParticleEmitterDesc& e = outDesc.emitters.emplace_back();
        parseEmitter(*emitterNode, e);
    }
    if (outDesc.emitters.empty())
    {
        outError = path + ": ParticleEffect has no Emitter entries";
        return false;
    }
    return true;
}
