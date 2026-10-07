#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable

// The tessellated terrain's control stage (StaticMeshGraphicsPipeline::buildTerrainTessLayout): one patch per
// mesh triangle, subdivided near the camera for terrain_tess.tes.glsl to displace. The ground only: the
// surface-water film is never tessellated.
//
// CRACK-FREE: an edge's factor is a function of its two END POINTS only (positions and normals), symmetric in
// them, so the two triangles sharing an edge agree on it bit for bit. Everything per PATCH (the culls, the inner
// level) touches no shared edge.
//
// Every level-of-detail decision reads the CENTRE view (VIEW_CENTER), never the eye's: in VR both eyes then
// tessellate and displace the same geometry (per-eye decisions made the eyes disagree - shimmer). Only
// gl_Position (the TES) projects with the eye.

#include "shared.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (vertices = 3) out;

layout (location = 0) in vec3 in_pos[];
layout (location = 1) in vec3 in_normal[];
layout (location = 2) in vec4 in_terrainFields[];

layout (location = 0) out vec3 out_pos[];
layout (location = 1) out vec3 out_normal[];
layout (location = 2) out vec4 out_terrainFields[];

// The TES displaces nothing below this normal y (its slope gate, smoothstep(0.35, 0.6, N.y)).
#define TERRAIN_TESS_MIN_NORMAL_Y 0.35
// A patch whose three corners all face away from the camera by more than this (dot(N, V)) is culled: the relief
// can tilt a displaced face back toward the camera, so a mesh-normal back face is not dropped at once.
#define TERRAIN_TESS_BACKFACE_MARGIN -0.3

// The edge's screen length over the target length, 1 past the fade end (no subdivision where nothing is
// displaced), up to the max factor. The screen length is estimated from the DISTANCE (length x projection
// scale / distance), not by projecting the end points: a projected length changes when the camera only
// rotates (perspective stretches edges toward the screen border), and every factor change slides the
// fractional vertices along the edge onto other heights - the terrain swam. Closer than the freeze distance
// the distance is held at it, so the subdivision stops changing near the camera (with the TES's mip
// footprint, which holds the same way).
// na / nb = the end points' normal y. 1 (no subdivision) where the TES displaces nothing along the edge: both
// end points past the slope gate, or a VERTICAL edge (the chunk's skirt walls: the same xz).
float terrainEdgeFactor(vec3 a, vec3 b, float na, float nb)
{
	if (max(na, nb) < TERRAIN_TESS_MIN_NORMAL_Y || distance(a.xz, b.xz) < 1e-3)
		return 1.0;
	const float maxFactor = u_terrainTess_maxFactor;
	const float fadeStart = u_terrainTess_fadeStart, fadeEnd = u_terrainTess_fadeEnd;
	const vec3 viewPos = u_views_viewPos[VIEW_CENTER].xyz;
	const float dist = distance(0.5 * (a + b), viewPos);
	if (dist >= fadeEnd)
		return 1.0;
	// The projection's y scale is the length of row 1 of the centre mvp's 3x3 (P11 x a unit view row).
	const mat4 mvp = u_views_mvp[VIEW_CENTER];
	const float projY = length(vec3(mvp[0][1], mvp[1][1], mvp[2][1]));
	const float pxPerMeterAt1m = 0.5 * projY * u_screenSize.y * u_viewportRect.w;
	float factor = distance(a, b) * pxPerMeterAt1m / (max(dist, u_terrainTess_freezeDistance) * u_terrainTess_targetEdgePx);
	// Eased down to 1 over the fade band with its OWN falloff ("Factor falloff exponent"); the TES's height
	// fades by "Height falloff exponent". Equal = they agree; a factor dropping before the height leaves the
	// still-displaced relief on coarser triangles (facets).
	const float t = clamp((dist - fadeStart) / max(fadeEnd - fadeStart, 1e-3), 0.0, 1.0);
	factor = mix(1.0, factor, 1.0 - pow(t, u_terrainTess_factorFalloff));
	return clamp(factor, 1.0, maxFactor);
}

void main()
{
#ifdef STEREO
	g_viewIndex = int(u_viewIndex);
#endif
	out_pos[gl_InvocationID] = in_pos[gl_InvocationID];
	out_normal[gl_InvocationID] = in_normal[gl_InvocationID];
	out_terrainFields[gl_InvocationID] = in_terrainFields[gl_InvocationID];

	if (gl_InvocationID == 0)
	{
		// Patch cull: all three corners outside one frustum plane (the centre view's), by more than the relief
		// can move them.
		const float margin = 0.5 * max(u_terrainTess_depthGround, u_terrainTess_depthRock);
		bool outside = false;
		for (int i = 0; i < 6 && !outside; ++i)
		{
			const vec4 plane = u_frustumPlanes[i];
			outside = dot(vec4(in_pos[0], 1.0), plane) < -margin
				&& dot(vec4(in_pos[1], 1.0), plane) < -margin
				&& dot(vec4(in_pos[2], 1.0), plane) < -margin;
		}
		// Back-face cull: all three corners' smooth normals turned away from the camera by the margin. Such a
		// patch is behind the terrain facing the camera (a heightfield's back slopes are occluded), and its
		// silhouette corners sit near dot 0, inside the margin. The rasterizer would cull its flat triangle
		// anyway; this saves its tessellation and displacement.
		const vec3 viewPos = u_views_viewPos[VIEW_CENTER].xyz;
		const vec3 n0 = normalize(in_normal[0]), n1 = normalize(in_normal[1]), n2 = normalize(in_normal[2]);
		if (!outside)
			outside = dot(n0, normalize(viewPos - in_pos[0])) < TERRAIN_TESS_BACKFACE_MARGIN
				&& dot(n1, normalize(viewPos - in_pos[1])) < TERRAIN_TESS_BACKFACE_MARGIN
				&& dot(n2, normalize(viewPos - in_pos[2])) < TERRAIN_TESS_BACKFACE_MARGIN;
		if (outside)
		{
			gl_TessLevelOuter[0] = 0.0;
			gl_TessLevelOuter[1] = 0.0;
			gl_TessLevelOuter[2] = 0.0;
			gl_TessLevelInner[0] = 0.0;
			return;
		}
		// Outer level i is the edge OPPOSITE corner i.
		const float e0 = terrainEdgeFactor(in_pos[1], in_pos[2], n1.y, n2.y);
		const float e1 = terrainEdgeFactor(in_pos[2], in_pos[0], n2.y, n0.y);
		const float e2 = terrainEdgeFactor(in_pos[0], in_pos[1], n0.y, n1.y);
		gl_TessLevelOuter[0] = e0;
		gl_TessLevelOuter[1] = e1;
		gl_TessLevelOuter[2] = e2;
		// The inner level is the patch's own (no shared edge). A SKIRT patch (a vertical face: the chunk's border
		// walls, TerrainGenerator) gets no interior subdivision: nothing there needs relief. Its edges keep their
		// factors - the top edge is shared with the surface.
		const vec3 faceN = cross(in_pos[1] - in_pos[0], in_pos[2] - in_pos[0]);
		const bool skirt = abs(faceN.y) < 0.05 * length(faceN);
		gl_TessLevelInner[0] = skirt ? 1.0 : max(e0, max(e1, e2));
	}
}
