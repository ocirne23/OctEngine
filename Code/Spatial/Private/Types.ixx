export module Spatial:Types;

import Core;
import Core.glm;
import Core.Frustum;
export import :SpatialTypes; // the public value types, for every internal partition
import :Morton;

// The library's internal types: the stamps, the occlusion test interface and the frustum helpers.

// The pass generation an entry was last stamped in (1..65534; the pool is swept on wrap).
export using SpatialStamp = uint16;
// "Linked, never stamped in this pass" (unlike the spawn-guard 0, which reads as in every pass).
export constexpr SpatialStamp SpatialStamp_Linked = 0xFFFF;

// Optional per-cell visibility test, after the frustum test; camera-relative bounds.
export struct IOcclusionTester
{
    virtual bool isVisible(const glm::vec3& centerRelCamera, const glm::vec3& halfExtent) = 0;
};

export inline void inflateFrustum(Frustum& frustum, float margin)
{
    for (uint32 i = 0; i < 6; ++i)
        frustum.planes[i].w += margin;
}
