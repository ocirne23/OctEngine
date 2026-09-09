#version 460

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
#include "gi_probe.inc.glsl"

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
    const float lifeFrac = clamp(age / particle.velLife.w, 0.0, 1.0);
    const vec3 pos = particle.posAge.xyz;

    // Per-particle constants re-derived from the spawn seed (cheaper than storing them).
    uint seed = particle.misc.y ^ 0x27D4EB2Fu;
    const float sizeRand = 1.0 + e.sizeParams.z * (particleRand(seed) * 2.0 - 1.0);
    const float size = max(1e-4, mix(e.sizeParams.x, e.sizeParams.y, lifeFrac) * sizeRand);

    // Alpha envelope: fade in over the first fadeParams.x of life, out from fadeParams.y to death.
    const float fadeIn = clamp(lifeFrac / max(e.fadeParams.x, 1e-3), 0.0, 1.0);
    const float fadeOut = 1.0 - smoothstep(e.fadeParams.y, 1.0, lifeFrac);
    float alpha = mix(e.colorStart.a, e.colorEnd.a, lifeFrac) * fadeIn * fadeOut;
    vec3 color = mix(e.colorStart.rgb, e.colorEnd.rgb, lifeFrac);

    if ((e.texFlags.y & PARTICLE_FLAG_LIT) != 0u)
    {
        const vec3 n = normalize(u_viewPos - pos + vec3(0.0, 1e-4, 0.0));
        float coverage;
        vec3 E = evalProbeSHCoverage(pos, n, coverage);
        const vec3 irr = mix(giEvalSkySH(n), E, coverage);
        const vec3 sun = atmosTransmittanceToLight(0.0, normalize(u_sunDirection), u_skyUp)
            * u_sunColor.rgb * u_eclipseParams.x;
        const vec3 light = irr * (1.0 / PI) + sun * 0.2 + u_ambientColor;
        color *= mix(light, vec3(1.0), e.spinParams.z);
    }

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
    const vec3 vel = particle.velLife.xyz;
    if (e.sizeParams.w > 0.0 && dot(vel, vel) > 1e-4)
    {
        // Velocity stretch: align the quad's up axis with the screen-projected velocity.
        const vec3 velPlane = vel - fwd * dot(vel, fwd);
        const float l = length(velPlane);
        if (l > 1e-3)
        {
            up = velPlane / l;
            right = normalize(cross(fwd, up));
            halfH += length(vel) * e.sizeParams.w * 0.5;
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
