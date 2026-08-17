export module Particle:Effect;

import Core;
import Core.glm;
import RendererVK;

// Authoring-side particle/decal configuration. A particle EFFECT is a named set of emitters (fire =
// flames + smoke + embers) loaded from a .pfx text asset (AssetParser grammar, see loadParticleEffect)
// or built in code; the ParticleSystem instantiates effects and drives the renderer's GPU emitter
// slots from them every frame, so descs stay plain data.

export struct ParticleEmitterDesc
{
    oc::string name;

    // Appearance. An empty texturePath renders a procedural soft round sprite. A flipbook atlas plays
    // cols x rows frames at fps (0 = no flipbook).
    oc::string texturePath;
    bool textureSRGB = true;
    uint32 flipbookCols = 0;
    uint32 flipbookRows = 0;
    float flipbookFps = 0.0f;
    glm::vec4 colorStart{ 1.0f, 1.0f, 1.0f, 1.0f }; // rgb = linear color * intensity, a = alpha
    glm::vec4 colorEnd{ 1.0f, 1.0f, 1.0f, 0.0f };
    float additivity = 0.0f;     // 0 = alpha blend (smoke), 1 = additive (fire/sparks)
    float fadeIn = 0.1f;         // fraction of life to fade in over
    float fadeOutStart = 0.7f;   // life fraction where the fade to death starts
    float softFadeDistance = 0.25f; // soft-particle fade distance against scene depth (m)
    bool lit = false;            // per-particle GI probe + sun lighting
    float emissiveFloor = 0.0f;  // lit only: 0 = fully lit, 1 = ignores lighting (self-lit)

    // Spawning.
    float rate = 10.0f;          // particles / second while emitting
    uint32 burst = 0;            // particles per ParticleEffect::burst() call
    float spawnRadius = 0.0f;    // sphere around the emitter position (m)
    float spawnShell = 0.0f;     // 0 = solid sphere, 1 = surface only
    float coneAngleDeg = 15.0f;  // spread around the emitter's up axis (180 = omnidirectional)
    float speedMin = 1.0f;
    float speedMax = 2.0f;
    glm::vec3 localOffset{ 0.0f };            // spawn center offset in emitter space
    glm::vec3 localDirection{ 0.0f, 1.0f, 0.0f }; // cone axis in emitter space
    float inheritVelocity = 0.0f;             // fraction of the emitter's velocity added at spawn

    // Motion.
    float lifeMin = 1.0f;
    float lifeMax = 2.0f;
    float gravity = 0.0f;        // m/s^2 along -Y (negative = buoyant)
    float drag = 0.0f;           // 1/s
    float turbulence = 0.0f;     // wander acceleration (m/s^2)
    float turbulenceFrequency = 0.25f; // 1/m
    float turbulenceScroll = 0.0f;     // field scroll speed (m/s, upward)
    bool collide = false;        // screen-space depth collision
    float collisionBounce = 0.3f;

    // Shape over life.
    float sizeStart = 0.1f;      // m
    float sizeEnd = 0.1f;
    float sizeVariance = 0.0f;   // +- fraction per particle
    float velocityStretch = 0.0f; // s: elongates the quad along velocity (0 = round billboard)
    float spinMax = 0.0f;        // rad/s, random sign per particle
    bool randomRotation = true;  // random initial roll (ignored while velocity-stretched)

    // Fills the static part of the GPU config; the ParticleSystem overwrites the per-instance fields
    // (position/rotation/velocity) each frame. textureIdx = renderer bindless index (PARTICLE_TEX_NONE
    // for the procedural sprite).
    RendererVKLayout::ParticleEmitterGpu toGpu(uint16 textureIdx) const;
};

export struct ParticleEffectDesc
{
    oc::string name;
    oc::vector<ParticleEmitterDesc> emitters;
};

// Projected box decal spawned onto a surface (ParticleSystem::spawnDecal).
export struct DecalDesc
{
    oc::string texturePath; // empty = solid tint
    bool textureSRGB = true;
    glm::vec2 size{ 1.0f, 1.0f };  // world extent across the surface (m)
    float depth = 0.25f;           // projection half-depth along the normal (m)
    glm::vec4 tint{ 1.0f };        // rgb = color * intensity, a = base alpha
    glm::vec3 emissive{ 0.0f };
    bool lit = true;               // sun + GI modulate the color
    float lifetime = 0.0f;         // seconds; 0 = persistent until removed
    float fadeOutTime = 1.0f;      // fade at end of life (also used by removeDecal)
    float angleFadeDeg = 80.0f;    // surfaces tilted further than this from the projection fade out
    float angleFadeWidth = 0.2f;   // fade band width (cos units)
    bool randomRotation = true;    // random roll around the surface normal
};

// Loads a .pfx effect (path relative to Assets/). Grammar, all entries optional with the defaults
// above (see Assets/Effects/*.pfx for examples):
//   ParticleEffect <name>
//       Emitter <name>
//           Texture <path>            Flipbook <cols> <rows> <fps>
//           ColorStart r, g, b, a     ColorEnd r, g, b, a
//           Additivity 0.5            Lit true    EmissiveFloor 0.2
//           Rate 40                   Burst 16
//           SpawnRadius 0.2           SpawnShell 1
//           ConeAngle 25              Speed <min> <max>
//           Offset x, y, z            Direction x, y, z     InheritVelocity 0.5
//           Life <min> <max>          Gravity 9.8           Drag 1.5
//           Turbulence 2 0.5 0.3      # amplitude, frequency, scroll
//           Collide true              Bounce 0.4
//           Size <start> <end>        SizeVariance 0.3
//           VelocityStretch 0.05      Spin 3                RandomRotation false
//           FadeIn 0.1                FadeOutStart 0.6      SoftFade 0.5
// Returns false (with the error in outError) on parse failure.
export bool loadParticleEffect(const oc::string& path, ParticleEffectDesc& outDesc, oc::string& outError);
