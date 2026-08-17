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
    gpu.spawnParams = glm::vec4(glm::radians(coneAngleDeg), speedMin, speedMax, spawnShell);
    gpu.lifeParams = glm::vec4(lifeMin, lifeMax, gravity, drag);
    gpu.noiseParams = glm::vec4(turbulence, turbulenceFrequency, turbulenceScroll, collisionBounce);
    gpu.sizeParams = glm::vec4(sizeStart, sizeEnd, sizeVariance, velocityStretch);
    gpu.colorStart = colorStart;
    gpu.colorEnd = colorEnd;
    gpu.fadeParams = glm::vec4(fadeIn, fadeOutStart, additivity, softFadeDistance);
    gpu.spinParams = glm::vec4(spinMax, randomRotation ? 1.0f : 0.0f, emissiveFloor, 0.0f);
    uint32 flags = 0;
    if (lit)
        flags |= PARTICLE_FLAG_LIT;
    if (collide)
        flags |= PARTICLE_FLAG_COLLIDE;
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
    if (const AssetNode* n = node.find("Size"))          { e.sizeStart = n->asFloat(0, e.sizeStart); e.sizeEnd = n->asFloat(1, e.sizeStart); }
    if (const AssetNode* n = node.find("SizeVariance"))  e.sizeVariance = n->asFloat(0, e.sizeVariance);
    if (const AssetNode* n = node.find("VelocityStretch")) e.velocityStretch = n->asFloat(0, e.velocityStretch);
    if (const AssetNode* n = node.find("Spin"))          e.spinMax = n->asFloat(0, e.spinMax);
    if (const AssetNode* n = node.find("RandomRotation")) e.randomRotation = n->asBool(0, e.randomRotation);
    if (const AssetNode* n = node.find("FadeIn"))        e.fadeIn = n->asFloat(0, e.fadeIn);
    if (const AssetNode* n = node.find("FadeOutStart"))  e.fadeOutStart = n->asFloat(0, e.fadeOutStart);
    if (const AssetNode* n = node.find("SoftFade"))      e.softFadeDistance = n->asFloat(0, e.softFadeDistance);
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
