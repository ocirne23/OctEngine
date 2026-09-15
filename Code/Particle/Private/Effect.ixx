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

    // Weather volume (rain / snow). A non-zero volume turns the emitter into a BOX of half extents
    // volume around its position: particles spawn uniformly inside it, WRAP at its faces (leaving one
    // face re-enters through the opposite one) and never age out, so `count` particles fill the box
    // once (spawned over the first frames, spawn-cap limited) and stay. With followCamera the emitter
    // rides the camera (position = camera + localOffset, identity rotation), so the box never
    // drains. occlude enables the shelter test against the renderer's top-down rain occlusion map: a
    // particle under a roof restarts at the box top at a random XZ. Rate/Burst still work on top.
    glm::vec3 volume{ 0.0f };    // box half extents (m); zero = ordinary point/sphere emitter
    uint32 count = 0;            // particles filling the box (volume emitters only)
    bool followCamera = false;
    bool occlude = false;
    float windResponse = 0.0f;   // 1/s: how fast the horizontal velocity relaxes onto the renderer's weather
                                 // wind ("Particles/Wind *"; heavy drops ~1, flakes ~4); 0 = ignores wind
    bool underwater = false;     // volume lives BELOW the live ocean surface: a particle above it is relocated
                                 // to a random depth under it, and the draw hides the volume while the camera
                                 // is above sea level (silt, bubbles)
    bool aboveWater = false;     // the inverse: a particle under the surface is relocated above it, and the
                                 // draw hides the volume while the camera is under sea level (dust). On a
                                 // NON-volume emitter only the draw gate applies (the ocean spray)
    float heightFalloff = 0.0f;  // m: alpha falls off as exp(-height above the ground (terrain / water) / this);
                                 // 0 = off (dust hugging the ground)

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
    bool waterFloor = false;     // the live ocean surface is a floor: a particle reaching it lands, stops and fades out

    // Shape over life.
    float sizeStart = 0.1f;      // m
    float sizeEnd = 0.1f;
    float sizeVariance = 0.0f;   // +- fraction per particle
    float velocityStretch = 0.0f; // s: elongates the quad along velocity (0 = round billboard)
    float spinMax = 0.0f;        // rad/s, random sign per particle
    bool randomRotation = true;  // random initial roll (ignored while velocity-stretched)

    bool isVolume() const { return volume.x > 0.0f && volume.y > 0.0f && volume.z > 0.0f; }

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
//           Collide true              Bounce 0.4            WaterFloor true
//           Size <start> <end>        SizeVariance 0.3
//           VelocityStretch 0.05      Spin 3                RandomRotation false
//           FadeIn 0.1                FadeOutStart 0.6      SoftFade 0.5
//           Volume x, y, z            Count 60000           # weather box (half extents) + its fill count
//           FollowCamera true         Occlude true          # ride the camera / shelter under roofs
//           WindResponse 1.0                                # 1/s relaxation onto the Particles/Wind tweaks
//           Underwater true           AboveWater true       # the volume lives below / above the ocean surface
//           HeightFalloff 2.5                               # m: alpha fades exp(-height above ground / this)
// Returns false (with the error in outError) on parse failure.
export bool loadParticleEffect(const oc::string& path, ParticleEffectDesc& outDesc, oc::string& outError);
