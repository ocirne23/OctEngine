#version 450

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable

// The tessellated terrain's control stage (StaticMeshGraphicsPipeline::buildTerrainTessLayout): one patch per
// mesh triangle, subdivided near the camera for terrain_tess.tes.glsl to displace.
//
// CRACK-FREE: an edge's factor is a function of its two END POINTS only, symmetric in them (the midpoint and
// the projected length), so the two triangles sharing an edge agree on it bit for bit. The ground and the
// overlay run this same module on the same inputs - the overlay's EQUAL depth test needs that too.

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

// The edge's screen length over the target length, 1 past the fade end (no subdivision where nothing is
// displaced), up to the max factor. The screen length is estimated from the DISTANCE (length x projection
// scale / distance), not by projecting the end points: a projected length changes when the camera only
// rotates (perspective stretches edges toward the screen border), and every factor change slides the
// fractional vertices along the edge onto other heights - the terrain swam. Closer than the freeze distance
// the distance is held at it, so the subdivision stops changing near the camera (with the TES's mip
// footprint, which holds the same way).
float terrainEdgeFactor(vec3 a, vec3 b)
{
	const float maxFactor = u_terrainTessParams0.y;
	const float fadeStart = u_terrainTessParams1.x, fadeEnd = u_terrainTessParams1.y;
	const float dist = distance(0.5 * (a + b), u_viewPos);
	if (dist >= fadeEnd)
		return 1.0;
	// The projection's y scale is the length of row 1 of the mvp's 3x3 (P11 x a unit view row).
	const float projY = length(vec3(u_mvp[0][1], u_mvp[1][1], u_mvp[2][1]));
	const float pxPerMeterAt1m = 0.5 * projY * u_screenSize.y * u_viewportRect.w;
	float factor = distance(a, b) * pxPerMeterAt1m / (max(dist, u_terrainTessParams2.x) * u_terrainTessParams0.z);
	// Eased down to 1 over the band where the displacement fades out - the TES's falloff (1 - t^p), so the
	// two agree and the subdivision never pops.
	const float t = clamp((dist - fadeStart) / max(fadeEnd - fadeStart, 1e-3), 0.0, 1.0);
	factor = mix(1.0, factor, 1.0 - pow(t, u_terrainTessParams0.w));
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
		// Patch cull: all three corners outside one frustum plane, by more than the relief can move them.
		const float margin = 0.5 * max(u_terrainTessParams1.z, u_terrainTessParams1.w);
		bool outside = false;
		for (int i = 0; i < 6 && !outside; ++i)
		{
			const vec4 plane = u_frustumPlanes[i];
			outside = dot(vec4(in_pos[0], 1.0), plane) < -margin
				&& dot(vec4(in_pos[1], 1.0), plane) < -margin
				&& dot(vec4(in_pos[2], 1.0), plane) < -margin;
		}
		if (outside)
		{
			gl_TessLevelOuter[0] = 0.0;
			gl_TessLevelOuter[1] = 0.0;
			gl_TessLevelOuter[2] = 0.0;
			gl_TessLevelInner[0] = 0.0;
			return;
		}
		// Outer level i is the edge OPPOSITE corner i.
		const float e0 = terrainEdgeFactor(in_pos[1], in_pos[2]);
		const float e1 = terrainEdgeFactor(in_pos[2], in_pos[0]);
		const float e2 = terrainEdgeFactor(in_pos[0], in_pos[1]);
		gl_TessLevelOuter[0] = e0;
		gl_TessLevelOuter[1] = e1;
		gl_TessLevelOuter[2] = e2;
		gl_TessLevelInner[0] = max(e0, max(e1, e2));
	}
}
