export module Spatial:Occlusion;

import Core;
import Core.glm;
import Core.Transform;
import :Types;

// CPU software occlusion: occluder triangles (the largest triangles of static mesh colliders)
// are rasterized camera-relative into a small depth buffer each frame; hierarchical max-depth
// blocks then answer IOcclusionTester cell queries for the main-view visibility set. Everything
// is conservative toward "visible": pixel-center coverage under-rasterizes occluders, the block
// mip keeps the farthest depth, and any near-plane crossing reports visible.

export struct OccluderData // triangle list (3 verts per triangle), container-local space
{
    oc::vector<glm::vec3> vertices;
};

// RAII registration of one occluder set, move-only like SpatialEntry.
export class SpatialOccluder final
{
public:

    SpatialOccluder() = default;
    explicit SpatialOccluder(SpatialHandle handle) : m_handle(handle) {}
    SpatialOccluder(const SpatialOccluder&) = delete;
    SpatialOccluder& operator=(const SpatialOccluder&) = delete;
    SpatialOccluder(SpatialOccluder&& other) noexcept : m_handle(other.m_handle) { other.m_handle = {}; }
    SpatialOccluder& operator=(SpatialOccluder&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_handle = other.m_handle;
            other.m_handle = {};
        }
        return *this;
    }
    ~SpatialOccluder() { reset(); }

    void reset(); // removes from Globals::occlusionBuffer, defined in Occlusion.cpp
    bool isValid() const { return m_handle.isValid(); }

private:

    SpatialHandle m_handle;
};

export class OcclusionBuffer final : public IOcclusionTester
{
public:

    void initialize(); // registers the Spatial/Occlusion tweaks

    // The largest maxTriangles triangles by area of an indexed mesh (degenerates dropped).
    static oc::shared_ptr<const OccluderData> extractOccluders(oc::span<const glm::vec3> vertices,
                                                                oc::span<const uint32> indices, uint32 maxTriangles);

    SpatialHandle addOccluder(const oc::shared_ptr<const OccluderData>& data, const Transform& world);
    void removeOccluder(SpatialHandle handle);

    bool isEnabled() const { return m_enabled; }
    int getMaxTriangles() const { return m_maxTriangles; }

    // Rasterize all occluders for this frame's camera; isVisible() answers against the result
    // until the next render(). viewProjRelCamera maps camera-relative world positions to clip space.
    void render(const glm::mat4& viewProjRelCamera, const glm::dvec3& cameraPos);

    bool isVisible(const glm::vec3& centerRelCamera, const glm::vec3& halfExtent) override;

private:

    static constexpr uint32 Width = 256;
    static constexpr uint32 Height = 144;
    static constexpr uint32 BlockSize = 8;
    static constexpr uint32 BlocksX = Width / BlockSize;
    static constexpr uint32 BlocksY = Height / BlockSize;
    static constexpr float NearEpsilon = 1e-4f;

    void clipAndRasterize(const glm::vec4 (&clip)[3]);
    void rasterizeTriangle(const glm::vec4& c0, const glm::vec4& c1, const glm::vec4& c2);

    struct Occluder
    {
        oc::vector<glm::vec3> worldVertices; // world space, transform baked at add
        uint32 gen = 0;
        bool used = false;
    };

    oc::vector<Occluder> m_occluders;
    oc::vector<uint32> m_freeOccluders;
    oc::vector<float> m_depth;    // Width * Height, NDC z, 1e30 = no occluder
    oc::vector<float> m_blockMax; // BlocksX * BlocksY, farthest depth per block
    glm::mat4 m_viewProjRel = glm::mat4(1.0f);
    bool m_enabled = false;
    bool m_rendered = false; // valid depth data for the current frame
    float m_depthBias = 0.001f;
    int m_maxTriangles = 2048;
    int m_statTriangles = 0;
    // isVisible runs concurrently from the parallel markVisible* tasks, so it counts atomically;
    // the plain int mirrors it once per render() for the tweak panel (which binds a raw int*).
    oc::atomic<int> m_statHiddenCells = 0;
    int m_statHiddenCellsDisplay = 0;
    float m_statRasterMs = 0.0f;
};

export namespace Globals
{
    OcclusionBuffer occlusionBuffer;
}
