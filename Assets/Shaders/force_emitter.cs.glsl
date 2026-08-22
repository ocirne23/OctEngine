#version 460

// Per-emitter applied force: one thread per compacted live emitter. The opposing teams' field
// pressure is integrated over a small fixed set of sample points spread through the emitter's own
// bubble (the shape's midpoint + 12 icosahedron directions at ~70% of its half-extent) — a single
// center tap would read zero when two large bubbles press rims together without their centers
// overlapping. Each sample is weighted by the emitter's own normalized field there, so deep-contact
// samples dominate and the result is continuous as contact begins/ends. The opposing gradient is
// finite-differenced (4-tap tetrahedral) — the warped-sphere shape has no cheap analytic gradient.
// Results are written SLOT-indexed (the source slot rides in teamFlags.z) straight into the
// host-visible readback buffer; the CPU reads them ~2 frames later (ocean-readback contract).

layout (local_size_x = FORCE_SIM_GROUP_SIZE) in;

#include "shared.inc.glsl" // UBO + the hash-table sentinels the grid include needs
#include "force_field.inc.glsl"

layout (binding = 5, std430) writeonly buffer OutForces { vec4 out_forces[]; }; // xyz force, w pressure

const vec3 c_icoDirs[12] = vec3[12](
    vec3( 0.0,  0.5257311,  0.8506508), vec3( 0.0, -0.5257311,  0.8506508),
    vec3( 0.0,  0.5257311, -0.8506508), vec3( 0.0, -0.5257311, -0.8506508),
    vec3( 0.5257311,  0.8506508, 0.0), vec3(-0.5257311,  0.8506508, 0.0),
    vec3( 0.5257311, -0.8506508, 0.0), vec3(-0.5257311, -0.8506508, 0.0),
    vec3( 0.8506508, 0.0,  0.5257311), vec3(-0.8506508, 0.0,  0.5257311),
    vec3( 0.8506508, 0.0, -0.5257311), vec3(-0.8506508, 0.0, -0.5257311));

void main()
{
    const uint i = gl_GlobalInvocationID.x;
    if (i >= fe_evalCount) // includes the PASSIVE tail: merged members read their own pressure
        return;
    const ForceEmitterData e = fe_emitters[i];
    const uint team = e.teamFlags.x;
    const float R = e.posReach.w;
    const vec3 center = e.posReach.xyz + e.dirFocus.xyz * (R * 0.5); // midpoint of the output line
    const float sampleRadius = 0.35 * R;
    const float h = max(0.02 * R, 0.01); // finite-difference step, reach-scaled
    const float emitterOutput = max(e.outputParams.x, 1e-4);

    vec3 force = vec3(0.0);
    float pressure = 0.0;
    const vec2 k = vec2(1.0, -1.0);
    for (uint s = 0u; s < 13u; ++s)
    {
        const vec3 x = s == 0u ? center : center + c_icoDirs[s - 1u] * sampleRadius;
        const float wSelf = forceContribution(x, e) / emitterOutput;
        if (wSelf <= 0.0)
            continue;
        float own, opp;
        forceTeamSample(x, team, own, opp);
        float o0, o1, o2, o3, unused;
        forceTeamSample(x + k.xyy * h, team, unused, o0);
        forceTeamSample(x + k.yyx * h, team, unused, o1);
        forceTeamSample(x + k.yxy * h, team, unused, o2);
        forceTeamSample(x + k.xxx * h, team, unused, o3);
        const vec3 grad = (k.xyy * o0 + k.yyx * o1 + k.yxy * o2 + k.xxx * o3) / (4.0 * h);
        force += wSelf * -grad;
        pressure += opp;
    }
    force *= e.outputParams.x * (1.0 / 13.0); // Output * mean — the raw integral; the "Force gain"
    pressure *= 1.0 / 13.0;                   // tweak is a CPU-side multiplier on the readback
    out_forces[e.teamFlags.z] = vec4(force, pressure);
}
