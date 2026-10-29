export module Spatial:Occlusion;

import Core;
import Core.glm;
import Core.Transform;
import :SpatialTypes;
import :OcclusionState; // the occluder set and the depth raster

// CPU software occlusion (Globals::occlusionBuffer): the largest static-collider triangles rasterized into a small depth buffer; the cull job's Main
// stamp drops the cells it hides. Conservative toward visible. The state is in OcclusionState.ixx; the multi-line bodies are below the classes.

export struct OccluderData // triangle list (3 verts per triangle), container-local space
{
    oc::vector<glm::vec3> vertices;
};

// RAII registration of one occluder set (addOccluder's handle), move-only like SpatialEntry.
export class SpatialOccluder final
{
public:

    SpatialOccluder() = default;
    explicit SpatialOccluder(SpatialHandle handle) : m_handle(handle) {}
    SpatialOccluder(SpatialOccluder&& other) noexcept : m_handle(other.m_handle) { other.m_handle = {}; }
    SpatialOccluder& operator=(SpatialOccluder&& other) noexcept;
    ~SpatialOccluder() { reset(); }

    void reset();
    bool isValid() const { return m_handle.isValid(); }

    SpatialOccluder(const SpatialOccluder&) = delete;
    SpatialOccluder& operator=(const SpatialOccluder&) = delete;

private:

    SpatialHandle m_handle;
};

export class OcclusionBuffer final : public OcclusionBufferState
{
public:

    void initialize();

    // The largest maxTriangles triangles by area of an indexed mesh (degenerates dropped).
    static oc::shared_ptr<const OccluderData> extractOccluders(oc::span<const glm::vec3> vertices, oc::span<const uint32> indices, uint32 maxTriangles);
    SpatialHandle addOccluder(const oc::shared_ptr<const OccluderData>& data, const Transform& world); // wrap the handle in a SpatialOccluder
    int getMaxTriangles() const { return m_settings.maxTriangles; } // "Max occluder tris": the budget extractOccluders is called with
};

export namespace Globals
{
    OcclusionBuffer occlusionBuffer;
}

// ---- Inline bodies ----

inline SpatialOccluder& SpatialOccluder::operator=(SpatialOccluder&& other) noexcept
{
    if (this != &other)
    {
        reset();
        m_handle = other.m_handle;
        other.m_handle = {};
    }
    return *this;
}
