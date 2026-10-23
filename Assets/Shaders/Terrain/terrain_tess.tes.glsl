#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_EXT_control_flow_attributes : enable // terrain_splat.inc.glsl's [[dont_unroll]]

// The tessellated terrain's evaluation stage: interpolates the control points and DISPLACES along the interpolated
// normal by the splat's height composite (terrainReliefAt, as the FS composites it), CENTRED on the mesh: height 0.5 is
// the mesh, so the flat mesh the TLAS, the collider and the shadow map see is the relief's mean surface. Faded out
// with the camera distance (to nothing where the TCS stops subdividing) and on steep ground (the world-XZ projection
// stretches into streaks). The ground only: the water film is never tessellated (the terrain VS lifts it).
// NO `precise`: the engine's glslang crashed in its PropagateNoContraction pass on it.

#include "shared.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (triangles, fractional_odd_spacing, ccw) in;

layout (location = 0) in vec3 in_pos[];
layout (location = 1) in vec3 in_normal[];
layout (location = 2) in vec4 in_terrainFields[];
layout (location = 4) in float in_river[];

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out vec4 out_terrainFields;
layout (location = 4) out float out_river;
// The UNDISPLACED position: the terrain FS lights from here. The shadow map and the TLAS hold the flat mesh, and the
// centred relief puts half the surface BELOW it - lit from the displaced point, that half sat inside its own caster
// (acne bands). A vec3, not the film's one-float lift: the FS rebuild from that stayed live across the ground FS
// (64 -> 72 registers); the interpolant is re-interpolated at each use instead.
layout (location = 3) out vec3 out_meshPos;

invariant gl_Position;

layout (binding = 24) uniform sampler2D u_textures[]; // the splat height maps

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
	float riverV;

	// CRACK-FREE edges: the two patches sharing an edge list its corners in opposite order, and the 3-term sum rounds
	// differently from each side (sparkles along the mesh edges). A vertex on an edge (one weight exactly 0)
	// interpolates its two corners in a FIXED order; a corner (a weight exactly 1) is the control point itself.
	const int corner = bary.x == 1.0 ? 0 : bary.y == 1.0 ? 1 : bary.z == 1.0 ? 2 : -1;
	const int zero = bary.x == 0.0 ? 0 : bary.y == 0.0 ? 1 : bary.z == 0.0 ? 2 : -1;
	if (corner >= 0)
	{
		pos = in_pos[corner];
		Ni = in_normal[corner];
		fieldsV = in_terrainFields[corner];
		riverV = in_river[corner];
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
		riverV = in_river[i] + (in_river[j] - in_river[i]) * wj;
	}
	else
	{
		pos = in_pos[0] * bary.x + in_pos[1] * bary.y + in_pos[2] * bary.z;
		Ni = in_normal[0] * bary.x + in_normal[1] * bary.y + in_normal[2] * bary.z;
		fieldsV = in_terrainFields[0] * bary.x + in_terrainFields[1] * bary.y + in_terrainFields[2] * bary.z;
		riverV = in_river[0] * bary.x + in_river[1] * bary.y + in_river[2] * bary.z;
	}
	const vec3 N = normalize(Ni);
	out_meshPos = pos;

	// The CENTRE view for every displacement decision: both VR eyes displace the same surface (terrain_tess.tcs.glsl).
	const float dist = distance(pos, u_views_viewPos[VIEW_CENTER].xyz);
	if (dist < u_terrainTess_fadeEnd && u_terrain_splatBase >= 0.0 && u_terrain_numGround >= 1.0)
	{
		const TerrainFields f = TerrainFields(fieldsV.x, fieldsV.y, fieldsV.z, fieldsV.w, riverV);
		const TerrainLayers L = terrainLayers(pos, N, f);
		// Falloff across the fade band: 1 - t^p ("Height falloff exponent"; the TCS eases its factor the same way).
		const float t = clamp((dist - u_terrainTess_fadeStart) * u_terrainTess_invFadeWidth, 0.0, 1.0);
		const float strength = (1.0 - pow(t, u_terrainTess_heightFalloff)) * smoothstep(0.35, 0.6, N.y); // the HEIGHT falloff
		const float depth = mix(mix(u_terrainTess_depthGround, u_terrainTess_depthRock, float(L.rockW)), u_terrainTess_depthGround, float(L.snowW)) * strength;
		if (depth > 1e-4)
		{
			// Height-map footprint = the target subdivided edge at this distance: a function of the POSITION only, so
			// patches sharing a vertex pick the same mip. Held at the freeze distance closer in, like the TCS's factor:
			// a mip that kept sharpening as the camera approached moved every height under it.
			const float spacing = max(dist, u_terrainTess_freezeDistance) * u_terrainTess_spacingPerMetre;
			const float height = float(terrainReliefAt(L, pos.xz, vec2(spacing, 0.0), vec2(0.0, spacing)));
			pos += N * ((height - 0.5) * depth);
		}
	}

	out_pos = pos;
	out_normal = Ni; // the SMOOTH mesh normal: the coverages read it, the FS derives the displaced normal from it
	out_terrainFields = fieldsV;
	out_river = riverV;
	gl_Position = u_mvp * vec4(pos, 1.0);
	gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
}
