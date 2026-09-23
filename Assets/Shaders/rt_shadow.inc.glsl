// Alpha-masked-aware shadow ray (ray query). Only alpha-masked instances are non-opaque in the TLAS
// (everything else is ForceOpaque, see gi_tlas_instances.cs.glsl), so the candidate loop only ever runs
// the alpha test for masked geometry; rays through opaque scenes commit in hardware as before.
//
// Requires the includer to have declared, with these names:
//   - u_tlas (accelerationStructureEXT)
//   - in_instances[]    (InMeshInstance: meshIdxMaterialIdx in .z)
//   - in_meshInfos[]    (InMeshInfo: firstIndex / vertexOffset)
//   - in_indices[] / in_vertices[] (MeshVertex[]: mesh_vertex.inc.glsl)
//   - in_materialInfos[] (MaterialInfo: diffuseNormalTexIdx, opacity)
//   - u_textures[] + GL_EXT_nonuniform_qualifier
//   - GL_EXT_ray_query

#ifndef RT_SHADOW_INC_GLSL
#define RT_SHADOW_INC_GLSL

#define RT_SHADOW_ALPHA_LOD 3.0 // coarse mip: no derivatives on rays, and shadows don't need alpha detail

vec2 rtsVertexUV(uint vi) { return meshVertexUV(in_vertices[vi]); }

// Alpha test for a non-opaque candidate triangle. Returns true if the hit blocks the ray. Any
// out-of-range index is treated as blocking (same bounded-access policy as the GI trace).
bool rtsCandidateBlocks(rayQueryEXT rq)
{
	const int instanceIdx = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false);
	if (uint(instanceIdx) >= in_instances.length())
		return true;
	// Geometry from the RT meshIdx in the instance's sbtOffset (LOD chains trace one shared BLAS, which
	// may differ from the raster-selected level the instance references); material from the instance.
	const uint meshIdx     = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rq, false);
	const uint materialIdx = in_instances[instanceIdx].meshIdxMaterialIdx >> 16;
	if (meshIdx >= in_meshInfos.length() || materialIdx >= in_materialInfos.length())
		return true;
	const InMeshInfo mi = in_meshInfos[meshIdx];
	const uint triBase = mi.firstIndex + uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)) * 3u;
	if (triBase + 2u >= in_indices.length())
		return true;
	const uint v0 = uint(mi.vertexOffset) + in_indices[triBase + 0u];
	const uint v1 = uint(mi.vertexOffset) + in_indices[triBase + 1u];
	const uint v2 = uint(mi.vertexOffset) + in_indices[triBase + 2u];
	if (max(max(v0, v1), v2) >= in_vertices.length())
		return true;
	const vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, false);
	const vec2 uv = (1.0 - bc.x - bc.y) * rtsVertexUV(v0) + bc.x * rtsVertexUV(v1) + bc.y * rtsVertexUV(v2);
	const uint diffuseTexIdx = in_materialInfos[materialIdx].diffuseNormalTexIdx & 0x0000FFFFu;
	const float alpha = textureLod(u_textures[nonuniformEXT(diffuseTexIdx)], uv, RT_SHADOW_ALPHA_LOD).a;
	return alpha >= in_materialInfos[materialIdx].opacity;
}

// Hard shadow visibility (1 = unoccluded) that respects alpha-masked geometry.
float rtShadowVisibility(vec3 origin, vec3 dir, float tMin, float tMax)
{
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, u_tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, origin, tMin, dir, tMax);
	while (rayQueryProceedEXT(rq))
	{
		if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT
			&& rtsCandidateBlocks(rq))
			rayQueryConfirmIntersectionEXT(rq);
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionTriangleEXT ? 0.0 : 1.0;
}

#endif
