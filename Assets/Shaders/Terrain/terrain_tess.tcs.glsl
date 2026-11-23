#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable

// The tessellated terrain's control stage (buildTerrainTessLayout): one patch per mesh triangle, subdivided near the
// camera for terrain_tess.tes.glsl to displace. The ground only: the water film is never tessellated.
// CRACK-FREE: an edge's factor is a function of its two END POINTS only, symmetric in them, so the two triangles
// sharing an edge agree on it bit for bit. Everything per PATCH (the culls, the inner level) touches no shared edge.
// Every LOD decision reads the CENTRE view, never the eye's: in VR both eyes then tessellate and displace the same
// geometry (per-eye decisions shimmer). Only gl_Position (the TES) projects with the eye.

#extension GL_EXT_nonuniform_qualifier : enable // the bindless array (the snow's tracks)
#extension GL_EXT_control_flow_attributes : enable // [[unroll]]

#include "shared.inc.glsl"
layout (binding = 27) uniform sampler2D u_textures[]; // the forward set's bindless array: the snow deformation maps
#define SNOW_DEFORM_READ
#include "snow.inc.glsl"

#ifdef STEREO
layout (push_constant) uniform ViewPC { uint u_viewIndex; };
#endif

layout (vertices = 3) out;

layout (location = 0) in vec3 in_pos[];
layout (location = 1) in vec3 in_normal[];
layout (location = 2) in vec4 in_terrainFields[];
layout (location = 4) in float in_river[];
layout (location = 5) in vec2 in_snowLift[]; // the VS's lift (all, the tracks' share): passed through to the TES

layout (location = 0) out vec3 out_pos[];
layout (location = 1) out vec3 out_normal[];
layout (location = 2) out vec4 out_terrainFields[];
layout (location = 4) out float out_river[];
layout (location = 5) out vec2 out_snowLift[];

// The TES displaces nothing below this normal y (its slope gate, smoothstep(0.35, 0.6, N.y)).
#define TERRAIN_TESS_MIN_NORMAL_Y 0.35
// A patch whose three corners all face away from the camera by more than this (dot(N, V)) is culled: the relief
// can tilt a displaced face back toward the camera, so a mesh-normal back face is not dropped at once.
#define TERRAIN_TESS_BACKFACE_MARGIN -0.3

// The edge's screen length over the target length, 1 past the fade end, up to the max factor. The screen length is
// estimated from the DISTANCE, not by projecting the end points: a projected length changes when the camera only
// rotates, and every factor change slides the fractional vertices onto other heights - the terrain swam. Closer than
// the freeze distance the distance is held at it, so the subdivision stops changing near the camera (as the TES's
// mip footprint). na / nb = the end points' normal y: 1 where both are past the TES's slope gate. ta / tb = the end
// points' drawn tracks (the VS's, |in_snowLift.y|).
float terrainEdgeFactor(vec3 a, vec3 b, float na, float nb, float ta, float tb)
{
	if (max(na, nb) < TERRAIN_TESS_MIN_NORMAL_Y)
		return 1.0;

	float maxFactor = u_terrainTess_maxFactor;
	const vec3 viewPos = u_views_viewPos[VIEW_CENTER].xyz;
	const float dist = distance(0.5 * (a + b), viewPos);
	if (dist >= u_terrainTess_fadeEnd)
		return 1.0;
	// factorPerMetre = the centre view's pixels per metre at 1 m over the target edge length (the CPU's).
	float factor = distance(a, b) * u_terrainTess_factorPerMetre / max(dist, u_terrainTess_freezeDistance);
	// THE SNOW'S TRACKS within "Weather/Snow/Track detail range": an edge over a deformation (its ends, or its quarter
	// points: a 0.6 m track fits between two vertices) takes twice the factor, up to at least 32 - footprints need ~6 cm
	// vertices. The quarter points are taken in a FIXED end order, so both patches sample the same points and agree.
	if (dist < u_snow_trackDetailRange && u_snow_deformPresent > 0.5)
	{
		const bool swap = a.x > b.x || (a.x == b.x && a.z > b.z);
		const vec2 p = swap ? b.xz : a.xz, q = swap ? a.xz : b.xz;
		float deform = max(ta, tb);
		[[unroll]] for (int i = 1; i <= 3; ++i)
			deform = max(deform, abs(snowDeformAt(mix(p, q, 0.25 * float(i)))));
		const float w = smoothstep(0.01, 0.05, deform);
		factor *= 1.0 + w;
		maxFactor = max(maxFactor, mix(maxFactor, 32.0, w));
	}
	// Eased down to 1 over the fade band with its OWN falloff ("Factor falloff exponent", the TES's height: "Height
	// falloff exponent"); a factor dropping before the height leaves still-displaced relief on coarse triangles.
	const float t = clamp((dist - u_terrainTess_fadeStart) * u_terrainTess_invFadeWidth, 0.0, 1.0);
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
	out_river[gl_InvocationID] = in_river[gl_InvocationID];
	out_snowLift[gl_InvocationID] = in_snowLift[gl_InvocationID];

	if (gl_InvocationID == 0)
	{
		// Patch cull: all three corners outside one frustum plane (the centre view's), by more than the relief and the
		// snow's tracks between them can move them (the TES's residual: at most 1.5 x the undisturbed depth).
		float trackReach = 0.0;
		if (u_snow_deformPresent > 0.5)
		{
			const float undeformed = max(in_snowLift[0].x - in_snowLift[0].y, max(in_snowLift[1].x - in_snowLift[1].y, in_snowLift[2].x - in_snowLift[2].y));
			trackReach = min(1.5 * undeformed, SNOW_DEFORM_RANGE);
		}
		const float margin = 0.5 * max(u_terrainTess_depthGround, u_terrainTess_depthRock) + trackReach;
		bool outside = false;
		for (int i = 0; i < 6 && !outside; ++i)
		{
			const vec4 plane = u_frustumPlanes[i];
			outside = dot(vec4(in_pos[0], 1.0), plane) < -margin
				&& dot(vec4(in_pos[1], 1.0), plane) < -margin
				&& dot(vec4(in_pos[2], 1.0), plane) < -margin;
		}

		// Back-face cull: all three corners' smooth normals turned away from the camera by the margin (a heightfield's
		// back slopes are occluded; silhouette corners sit near dot 0, inside the margin). Saves the tessellation and
		// displacement of a triangle the rasterizer would cull anyway.
		const vec3 viewPos = u_views_viewPos[VIEW_CENTER].xyz;
		const vec3 n0 = in_normal[0], n1 = in_normal[1], n2 = in_normal[2]; // per vertex (not interpolated): unit
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
		const float t0 = abs(in_snowLift[0].y), t1 = abs(in_snowLift[1].y), t2 = abs(in_snowLift[2].y);
		const float e0 = terrainEdgeFactor(in_pos[1], in_pos[2], n1.y, n2.y, t1, t2);
		const float e1 = terrainEdgeFactor(in_pos[2], in_pos[0], n2.y, n0.y, t2, t0);
		const float e2 = terrainEdgeFactor(in_pos[0], in_pos[1], n0.y, n1.y, t0, t1);
		gl_TessLevelOuter[0] = e0;
		gl_TessLevelOuter[1] = e1;
		gl_TessLevelOuter[2] = e2;
		// The inner level is the patch's own (no shared edge). A track or a crater wholly INSIDE the patch (its centroid)
		// doubles it, up to 32.
		float inner = max(e0, max(e1, e2));
		if (u_snow_deformPresent > 0.5 && inner > 1.0)
		{
			const vec3 middle = (in_pos[0] + in_pos[1] + in_pos[2]) * (1.0 / 3.0);
			if (distance(middle, viewPos) < u_snow_trackDetailRange && abs(snowDeformAt(middle.xz)) > 0.01)
				inner = max(inner, min(2.0 * inner, 32.0));
		}
		gl_TessLevelInner[0] = inner;
	}
}
