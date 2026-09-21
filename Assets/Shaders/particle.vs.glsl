#version 460

#extension GL_EXT_nonuniform_qualifier : enable // the GI volume's per-cascade texture index

// Particle billboard vertex shader: 6 vertices per instance (one quad), instance -> pool index via the
// OUT alive list the sim pass compacted this frame (the indirect draw's instanceCount is its count).
// All per-particle work lives here (color/size over life, fades, flipbook frame select, optional
// per-particle lighting: GI probe irradiance + sun), so the fragment shader stays minimal.

#include "shared.inc.glsl"
#include "particle.inc.glsl"

layout (binding = 1, std430) readonly buffer Particles { Particle pp_particles[]; };
layout (binding = 2, std430) readonly buffer AliveList { uint pa_alive[]; };
layout (binding = 3, std430) readonly buffer Emitters { ParticleEmitter pe_emitters[]; };
layout (binding = 5, std430) readonly buffer GiGridData { vec4 gi_gridData[]; };

#define GI_GRID_DATA_NAME gi_gridData
#ifdef GI_VOLUME
layout (binding = 10) uniform sampler3D u_giVolume[GI_VOLUME_MAX_IMAGES]; // the baked irradiance volume + sky SH
#define GI_VOLUME_TEXTURES_NAME u_giVolume
#endif
#include "gi_probe.inc.glsl"
// The terrain-data cascades (height / water level) for the ground fade (PARTICLE_FLAG_GROUND_FADE).
#define TERRAIN_HEIGHT_BINDING 6
#include "terrain_height.inc.glsl"

// The scene's punctual lights through the light grid (the forward pass's own buffers), for LIT
// particles: a dust mote near a lamp picks up the lamp. Irradiance only - a particle is a scattering
// speck with no normal, so every light contributes its falloff-attenuated colour isotropically.
struct LightInfo
{
    vec3 pos;
    float range;
    vec3 color;
    float width;     // 0 = point light, > 0 = rectangular area light, < 0 = spot
    vec3 direction;  // area light: up-axis, magnitude = height; spot: axis, magnitude = edge softness
    float rotation;  // area light: rotation of the quad around direction; spot: cone half-angle
};
layout (binding = 7, std430) readonly buffer InLightInfos { LightInfo in_lightInfos[]; };
layout (binding = 8, std430) readonly buffer InLightGrid { uint in_gridData[]; };
layout (binding = 9, std430) readonly buffer InGridTable
{
    uint in_numGrids;
    uint in_gridDataCounter;
    uint in_tableSize;
    uint in_gridTable[];
};
#define GRID_DATA_NAME  in_gridData
#define GRID_TABLE_NAME in_gridTable
#define TABLE_SIZE_NAME in_tableSize
#include "light_grid.inc.glsl"

// Scattering phase, Henyey-Greenstein normalized so g = 0 gives 1: forward scattering makes a light
// BEHIND the particle (light -> particle -> camera aligned) bright and one beside it dim - the halo
// and dark side that stop a lit mist reading flat. cosTheta = dot(light-to-particle, particle-to-eye);
// g = "Particles/Anisotropy" (u_rainOcclusionParams.w), separate from the fog's.
float particlePhase(float cosTheta)
{
    const float g = clamp(u_rainOcclusionParams.w, -0.95, 0.95);
    const float g2 = g * g;
    const float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (denom * sqrt(max(denom, 1e-4)));
}

// One light's irradiance at a point (the forward pass's falloff + spot cone; area and tube lights as a
// point at their centre - a particle cannot resolve their shape), phase-weighted toward the eye.
vec3 particleLightIrradiance(LightInfo light, vec3 pos, vec3 toEye)
{
    const vec3 lightVec = light.pos - pos;
    const float dist = max(length(lightVec), 1e-3);
    const float range = abs(light.range);
    float attenuation = 1.0 / (dist * dist + 1.0);
    const float dr2 = (dist / range) * (dist / range);
    float falloff = clamp(1.0 - dr2 * dr2, 0.0, 1.0);
    falloff *= falloff;
    float spot = 1.0;
    if (light.width < 0.0)
    {
        const float softness = max(length(light.direction), 1e-4);
        const float cosAngle = dot(-lightVec / dist, light.direction / softness);
        const float cosOuter = cos(light.rotation);
        spot = smoothstep(cosOuter, mix(cosOuter, 1.0, softness), cosAngle);
    }
    const float phase = particlePhase(dot(-lightVec / dist, toEye));
    return light.color * (attenuation * falloff * spot * phase);
}

// Sum of the lights covering pos: the grid cell's own list plus the grid's large-light list.
vec3 particleLocalLights(vec3 pos, vec3 toEye)
{
    vec3 sum = vec3(0.0);
    const ivec3 gridPos = getGridPos(pos);
    uint tableIdx = getTableIdx(gridPos);
    for (uint probe = 0u; probe < 8u; ++probe) // bounded probe walk (the forward pass loops until EMPTY_ENTRY)
    {
        const uint gridIdx = getGridIdx(tableIdx);
        if (gridIdx == EMPTY_ENTRY)
            break;
        if (getGridMin(gridIdx) == gridPos)
        {
            const uint numLargeLights = getLargeLightCount(gridIdx);
            for (uint i = 0u; i < min(numLargeLights, MAX_LARGE_LIGHTS_PER_GRID); ++i)
                sum += particleLightIrradiance(in_lightInfos[getLargeLightId(gridIdx, i)], pos, toEye);
            const uint cellOffset = calcCellOffset(gridIdx, gridPos, pos);
            const uint numLights = getNumLightsForCell(cellOffset);
            for (uint i = 0u; i < min(numLights, MAX_LIGHTCELL_LIGHTS); ++i)
                sum += particleLightIrradiance(in_lightInfos[getLightId(cellOffset, i)], pos, toEye);
            break;
        }
        tableIdx = getNextTableIdx(tableIdx);
    }
    return sum;
}

// 0 = centre/desktop, 1/2 = the eyes in VR (selects the view matrices + billboard basis).
layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) out vec2 v_uv0;
layout (location = 1) out vec2 v_uv1;
layout (location = 2) out vec4 v_color;
layout (location = 3) out flat float v_flipBlend;
layout (location = 4) out flat uint v_texIdx;
layout (location = 5) out flat float v_additivity;
layout (location = 6) out flat float v_softInv;
layout (location = 7) out float v_clipW;

void main()
{
    g_viewIndex = int(u_viewIndex);

    const uint particleIdx = pa_alive[gl_InstanceIndex];
    const Particle particle = pp_particles[particleIdx];
    const ParticleEmitter e = pe_emitters[particle.misc.x];

    const float age = particle.posAge.w;
    // A weather volume particle never ages out (it wraps), so its "life" sits at the midpoint: colour and
    // size are the mid blend and the fade envelope is replaced by the box's XZ edge fade below.
    const bool volume = (e.texFlags.y & PARTICLE_FLAG_VOLUME) != 0u;
    const float lifeFrac = volume ? 0.5 : clamp(age / particle.velLife.w, 0.0, 1.0);
    const vec3 pos = particle.posAge.xyz;

    // Per-particle constants re-derived from the spawn seed (cheaper than storing them).
    uint seed = particle.misc.y ^ 0x27D4EB2Fu;
    const float sizeRand = 1.0 + e.sizeParams.z * (particleRand(seed) * 2.0 - 1.0);
    const float size = max(1e-4, mix(e.sizeParams.x, e.sizeParams.y, lifeFrac) * sizeRand);

    // Alpha envelope: fade in over the first fadeParams.x of life, out from fadeParams.y to death.
    const float fadeIn = clamp(lifeFrac / max(e.fadeParams.x, 1e-3), 0.0, 1.0);
    const float fadeOut = 1.0 - smoothstep(e.fadeParams.y, 1.0, lifeFrac);
    float envelope = volume
        ? particleVolumeEdgeFade(pos - e.posSpawnRadius.xyz, e.volumeParams.xyz) * weatherSheet(pos, u_timeSeconds)
        : fadeIn * fadeOut;
    // The camera's side of the water decides whole-emitter visibility: an underwater volume shows only
    // while the camera is under the water surface at its XZ, an above-water one only while it is over
    // it. The surface is the LIVE wave height under the camera when the ocean supplies it (the CPU
    // mirror, u_weatherWind2.z), else the local calm water level from the terrain data, else sea level.
    // The sim keeps the particles themselves on their side of the live surface; this is the gate.
    if ((e.texFlags.y & (PARTICLE_FLAG_UNDERWATER | PARTICLE_FLAG_ABOVE_WATER)) != 0u)
    {
        const float waterAtCamera = u_weatherWind2.w > 0.5 ? u_weatherWind2.z
            : (terrainHeightMapPresent() ? terrainDataAt(u_viewPos.xz).y : u_oceanParams2.w);
        const bool cameraUnder = u_viewPos.y < waterAtCamera;
        if (((e.texFlags.y & PARTICLE_FLAG_UNDERWATER) != 0u) != cameraUnder)
            envelope = 0.0;
    }
    // Ground fade: exp(-height above the TERRAIN / fade height) - dust hugs the ground and thins out with
    // height - and none over water: it fades out with the water depth across the first half metre, so
    // the sea is dust-free and the waterline is a soft edge rather than a cut.
    if ((e.texFlags.y & PARTICLE_FLAG_GROUND_FADE) != 0u && terrainHeightMapPresent())
    {
        const vec4 td = terrainDataAt(pos.xz);
        envelope *= exp(-max(pos.y - td.x, 0.0) / max(e.spinParams.w, 0.01));
        envelope *= 1.0 - smoothstep(0.0, 0.5, td.y - td.x);
    }
    float alpha = mix(e.colorStart.a, e.colorEnd.a, lifeFrac) * envelope;
    vec3 color = mix(e.colorStart.rgb, e.colorEnd.rgb, lifeFrac);

    // (Lighting is evaluated further down, at the CORNER's world position, so a big sprite gets a
    // gradient across it from the interpolation instead of one flat tint.)

    // Billboard basis for the selected view.
    const vec3 fwd = normalize(pos - u_viewPos);
    vec3 right = cross(vec3(0.0, 1.0, 0.0), fwd);
    right = length(right) > 1e-4 ? normalize(right) : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(fwd, right);

    const uint vi = uint(gl_VertexIndex);
    // (0,0) (1,0) (1,1) / (0,0) (1,1) (0,1)
    const vec2 corner01 = vec2((vi == 1u || vi == 2u || vi == 4u) ? 1.0 : 0.0,
                               (vi == 2u || vi == 4u || vi == 5u) ? 1.0 : 0.0);
    vec2 c = corner01 * 2.0 - 1.0;

    float halfW = size * 0.5;
    float halfH = size * 0.5;
    // Weather volume streaks: a streak is the drop's motion relative to what the EYE TRACKS. A player
    // tracks the ground, not the camera, so only a fraction (u_cameraVelocity.w) of the camera's own
    // motion smears the drops - the full amount lays fast-panned rain nearly flat.
    const vec3 vel = volume ? particle.velLife.xyz - u_cameraVelocity.xyz * u_cameraVelocity.w : particle.velLife.xyz;
    if (e.sizeParams.w > 0.0 && dot(vel, vel) > 1e-4)
    {
        // Velocity stretch: align the quad's up axis with the screen-projected velocity, and stretch by
        // the PROJECTED speed - a drop flying along the view ray is seen end-on as a dot, so its axis
        // (numerically arbitrary at that point) never shows.
        const vec3 velPlane = vel - fwd * dot(vel, fwd);
        const float l = length(velPlane);
        if (l > 1e-3)
        {
            up = velPlane / l;
            right = normalize(cross(fwd, up));
            halfH += l * e.sizeParams.w * 0.5;
        }
    }
    else if (e.spinParams.x != 0.0 || e.spinParams.y > 0.5)
    {
        const float rot = uintBitsToFloat(particle.misc.z) + uintBitsToFloat(particle.misc.w) * age;
        const float cr = cos(rot), sr = sin(rot);
        c = vec2(c.x * cr - c.y * sr, c.x * sr + c.y * cr);
    }

    const vec3 world = pos + right * (c.x * halfW) + up * (c.y * halfH);
    gl_Position = u_mvp * vec4(world, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w;

    if ((e.texFlags.y & PARTICLE_FLAG_LIT) != 0u)
    {
        // Lit at THIS corner's world position (the four corners differ, and the rasterizer interpolates
        // the colour across the quad), so a lamp beside a 2 m mist sprite lights its near edge more than
        // its far edge and the sprite reads as a gradient rather than a flat card.
        const vec3 n = normalize(u_viewPos - world + vec3(0.0, 1e-4, 0.0));
        float coverage;
#ifdef GI_VOLUME
        vec3 E = evalProbeVolumeCoverage(world, n, coverage);
#else
        vec3 E = evalProbeSHCoverage(world, n, coverage);
#endif
        // x "GI/Strength" (u_aoParams.y, 0 with GI or RT off, where the probes and the sky SH are stale),
        // like every other GI consumer.
        const vec3 irr = mix(giEvalSkySH(n), E, coverage) * u_aoParams.y;
        // The sun and the scene's lights are phase-weighted (particlePhase): a back-lit mist glows, a
        // side-lit one dims. GI and ambient stay isotropic - they come from everywhere.
        const vec3 toEye = n;
        const vec3 sunDir = normalize(u_sunDirection);
        const vec3 sun = atmosTransmittanceToLight(0.0, sunDir, u_skyUp)
            * u_sunColor.rgb * u_eclipseParams.x * particlePhase(dot(-sunDir, toEye));
        // GI + sun + ambient, plus the scene's punctual lights through the light grid (a lamp lights the
        // dust around it).
        const vec3 light = irr * (1.0 / PI) + sun * 0.2 + u_ambientColor + particleLocalLights(world, toEye);
        color *= mix(light, vec3(1.0), e.spinParams.z);
    }

    // Flipbook frame selection (uv y flipped: texture v grows downward).
    const vec2 uvBase = vec2(corner01.x, 1.0 - corner01.y);
    const uint cols = e.texFlags.z & 0xFFFFu;
    const uint rows = e.texFlags.z >> 16u;
    if (cols * rows > 1u)
    {
        const float frame = age * uintBitsToFloat(e.texFlags.w);
        const uint total = cols * rows;
        const uint f0 = uint(frame) % total;
        const uint f1 = (f0 + 1u) % total;
        v_uv0 = (uvBase + vec2(float(f0 % cols), float(f0 / cols))) / vec2(float(cols), float(rows));
        v_uv1 = (uvBase + vec2(float(f1 % cols), float(f1 / cols))) / vec2(float(cols), float(rows));
        v_flipBlend = fract(frame);
    }
    else
    {
        v_uv0 = uvBase;
        v_uv1 = uvBase;
        v_flipBlend = 0.0;
    }

    v_color = vec4(color, alpha);
    v_texIdx = e.texFlags.x;
    v_additivity = e.fadeParams.z;
    v_softInv = 1.0 / max(e.fadeParams.w, 1e-3);
    v_clipW = gl_Position.w;
}
