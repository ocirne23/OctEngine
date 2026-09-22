#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier : enable

// The tessellated terrain's evaluation stage: interpolates the control points and DISPLACES along the
// interpolated normal by the splat's height composite (terrainReliefAt - the same layers, coverages and height
// blend the fragment shader composites), CENTRED on the mesh: height 0.5 is the mesh, so the flat mesh the
// TLAS, the collider and the shadow map still see is the relief's mean surface. Faded out with the camera
// distance (to nothing where the control stage stops subdividing) and on steep ground, where the world-XZ
// projection of the height maps stretches into streaks.
//
// INVARIANT: the ground and the overlay (EQUAL depth) run this ONE module on the same inputs, with
// `invariant gl_Position`. NO `precise`: the engine's glslang crashed in its PropagateNoContraction pass on it.

#include "shared.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (triangles, fractional_odd_spacing, ccw) in;

layout (location = 0) in vec3 in_pos[];
layout (location = 1) in vec3 in_normal[];
layout (location = 2) in vec4 in_terrainFields[];

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec4 out_terrainFields;
// The UNDISPLACED position: the terrain FS lights (sun shadow map, RT shadow rays, lights) from here. The
// shadow map and the TLAS hold the flat mesh, and the centred relief puts half the surface BELOW it - lit
// from the displaced point, that half sat inside its own caster (acne bands on the slopes away from the sun).
layout (location = 3) out vec3 out_meshPos;

invariant gl_Position;

layout (binding = 22) uniform sampler2D u_textures[]; // the splat height maps

#define TERRAIN_SPLAT_RELIEF
#define TERRAIN_SPLAT_HEIGHT_ONLY
#include "terrain_splat.inc.glsl"

bool terrainTessBefore(vec3 a, vec3 b)
{
	return a.x < b.x || (a.x == b.x && (a.z < b.z || (a.z == b.z && a.y < b.y)));
}

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex);
#endif
	const vec3 bary = gl_TessCoord;
	vec3 pos;
	vec3 Ni;
	vec4 fieldsV;
	// CRACK-FREE edges: the two patches sharing an edge list its corners in opposite order, and the 3-term sum
	// rounds differently from each side (single-pixel sparkles along the mesh edges). A vertex on an edge (one
	// weight exactly 0) interpolates its two corners in a FIXED order, from the second corner's own weight;
	// a corner (a weight exactly 1) is the control point itself. Both sides then compute the same bits, and
	// the displacement, a function of these, follows.
	const int corner = bary.x == 1.0 ? 0 : bary.y == 1.0 ? 1 : bary.z == 1.0 ? 2 : -1;
	const int zero = bary.x == 0.0 ? 0 : bary.y == 0.0 ? 1 : bary.z == 0.0 ? 2 : -1;
	if (corner >= 0)
	{
		pos = in_pos[corner];
		Ni = in_normal[corner];
		fieldsV = in_terrainFields[corner];
	}
	else if (zero >= 0)
	{
		int i = (zero + 1) % 3, j = (zero + 2) % 3;
		if (terrainTessBefore(in_pos[j], in_pos[i]))
		{
			const int t = i; i = j; j = t;
		}
		const float wj = bary[j];
		pos = in_pos[i] + (in_pos[j] - in_pos[i]) * wj;
		Ni = in_normal[i] + (in_normal[j] - in_normal[i]) * wj;
		fieldsV = in_terrainFields[i] + (in_terrainFields[j] - in_terrainFields[i]) * wj;
	}
	else
	{
		pos = in_pos[0] * bary.x + in_pos[1] * bary.y + in_pos[2] * bary.z;
		Ni = in_normal[0] * bary.x + in_normal[1] * bary.y + in_normal[2] * bary.z;
		fieldsV = in_terrainFields[0] * bary.x + in_terrainFields[1] * bary.y + in_terrainFields[2] * bary.z;
	}
	const vec3 N = normalize(Ni);
	out_meshPos = pos;

	const float fadeStart = u_terrainTessParams1.x, fadeEnd = u_terrainTessParams1.y;
	const float dist = distance(pos, u_viewPos);
	if (dist < fadeEnd && u_terrainTexParams0.x >= 0.0 && u_terrainTexParams0.y >= 1.0)
	{
		const TerrainFields f = TerrainFields(fieldsV.x, fieldsV.y, fieldsV.z, fieldsV.w);
		const TerrainLayers L = terrainLayers(pos, N, f);
		// Falloff across the fade band: 1 - t^p ("Falloff exponent"; the TCS eases its factor the same way).
		const float t = clamp((dist - fadeStart) / max(fadeEnd - fadeStart, 1e-3), 0.0, 1.0);
		const float strength = (1.0 - pow(t, u_terrainTessParams0.w)) * smoothstep(0.35, 0.6, N.y);
		const float depth = mix(mix(u_terrainTessParams1.z, u_terrainTessParams1.w, float(L.rockW)), u_terrainTessParams1.z, float(L.snowW)) * strength;
		if (depth > 1e-4)
		{
			// Height-map footprint = the target subdivided edge at this distance: a function of the POSITION
			// only, so the patches sharing a vertex pick the same mip (a per-patch footprint cracked the edges).
			// Held at the freeze distance closer in, like the control stage's factor: a mip that kept
			// sharpening as the camera approached moved every height under it.
			// The projection's y scale is the length of row 1 of the mvp's 3x3 (P11 x a unit view row).
			const float projY = length(vec3(u_mvp[0][1], u_mvp[1][1], u_mvp[2][1]));
			const float spacing = max(dist, u_terrainTessParams2.x) * 2.0 * u_terrainTessParams0.z / max(projY * u_screenSize.y * u_viewportRect.w, 1.0);
			const float16_t h = terrainReliefAt(L, pos.xz, vec2(spacing, 0.0), vec2(0.0, spacing));
			pos += N * ((float(h) - 0.5) * depth);
		}
	}

	out_pos = pos;
	out_normal = Ni; // the SMOOTH mesh normal: the coverages read it, the FS adds the relief's facet tilt to it
	out_terrainFields = fieldsV;
	gl_Position = u_mvp * vec4(pos, 1.0);
	gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
}
