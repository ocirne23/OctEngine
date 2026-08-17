export module RendererVK:RenderNode;

import Core;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import Threading;

import :Layout;

export namespace Globals
{
    oc::vector<Transform> renderNodeTransforms;

    // Sparse transform-upload tracking: per slot, one pending-upload bit per frame in flight, plus
    // per-WORKER per-frame index lists Renderer::present drains (only changed slots are copied into
    // that frame's GPU buffer instead of the whole array). Maintained by markRenderNodeTransformDirty.
    // The bits byte is single-writer per node (one owner pushes a node per frame); the lists are
    // per-worker so concurrent setTransform calls from parallel jobs never share a vector.
    oc::vector<uint8> renderNodeDirtyBits;
    PerWorker<oc::array<oc::vector<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT>> renderNodeDirtyLists;
}

// [Concurrency: LOCK-FREE across distinct nodes - one owner per node]
export inline void markRenderNodeTransformDirty(uint32 idx)
{
    constexpr uint8 allBits = uint8((1u << RendererVKLayout::NUM_FRAMES_IN_FLIGHT) - 1);
    uint8& bits = Globals::renderNodeDirtyBits[idx];
    if (bits == allBits)
        return;
    oc::array<oc::vector<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT>& lists = Globals::renderNodeDirtyLists.local();
    for (uint32 frame = 0; frame < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++frame)
        if (!(bits & (1u << frame)))
            lists[frame].push_back(idx);
    bits = allBits;
}

// RAII handle to a spawned renderer instance (movable, like PhysicsBody). Destroying the handle
// recycles its renderer resources: the transform slot goes on a free list, and a skinned node's whole
// allocation bundle is parked for reuse by the next spawnSkinnedNode of the same container.
export class RenderNode final
{
public:

    RenderNode() = default;
    RenderNode(const RenderNode&) = delete;
    RenderNode& operator=(const RenderNode&) = delete;
    RenderNode(RenderNode&& other) noexcept { moveFrom(oc::move(other)); }
    RenderNode& operator=(RenderNode&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            moveFrom(oc::move(other));
        }
        return *this;
    }
    ~RenderNode() { destroy(); }

    bool isValid() const { return m_transformIdx != UINT32_MAX; }
    void destroy();

    // The only write path: unchanged transforms cost one compare, changed ones are queued for
    // sparse upload (writing through a mutable getTransform would bypass the dirty tracking).
    inline void setTransform(const Transform& transform)
    {
        Transform& current = Globals::renderNodeTransforms[m_transformIdx];
        if (current.pos == transform.pos && current.scale == transform.scale && current.quat == transform.quat)
            return;
        current = transform;
        markRenderNodeTransformDirty(m_transformIdx);
    }

    inline const Transform& getTransform() const
    {
        return Globals::renderNodeTransforms[m_transformIdx];
    }

    inline size_t getNumMeshInstances() const { return m_meshInstances.size(); }

    // Per-entity TINT: rewrites every mesh instance's material index (pair with
    // Renderer::createSolidColorMaterial). Takes effect the next time the node is submitted —
    // instances are copied from this node's CPU list every renderNode() call. Main thread.
    inline void setMaterialOverride(uint16 materialIdx)
    {
        for (RendererVKLayout::InMeshInstance& instance : m_meshInstances)
            instance.materialIdx = materialIdx;
    }

    // Valid only for nodes spawned via ObjectContainer::spawnSkinnedNode. Pass to Renderer::setSkinningPalette
    // each frame with the AnimationPlayer's bone palette. UINT32_MAX for non-skinned nodes.
    inline bool isSkinned() const { return m_skinnedPaletteHandle != UINT32_MAX; }
    inline uint32 getSkinnedPaletteHandle() const { return m_skinnedPaletteHandle; }

    inline const Sphere& getLocalBounds() const { return m_bounds; }
    inline Sphere getWorldBounds() const
    {
        const Transform& transform = getTransform();
        return Sphere(transform.quat * m_bounds.pos * transform.scale + transform.pos, m_bounds.radius * transform.scale);
    };

private:

    friend class ObjectContainer;
    friend class Renderer;

    void moveFrom(RenderNode&& other) noexcept
    {
        m_transformIdx = other.m_transformIdx;
        m_skinnedPaletteHandle = other.m_skinnedPaletteHandle;
        m_skinnedBundleHandle = other.m_skinnedBundleHandle;
        m_lodStateBase = other.m_lodStateBase;
        m_bounds = other.m_bounds;
        m_meshInstances = oc::move(other.m_meshInstances);
        m_numInstancesPerMesh = oc::move(other.m_numInstancesPerMesh);
        m_lodInstances = oc::move(other.m_lodInstances);
        other.m_transformIdx = UINT32_MAX;
        other.m_skinnedPaletteHandle = UINT32_MAX;
        other.m_skinnedBundleHandle = UINT32_MAX;
        other.m_lodStateBase = UINT32_MAX;
    }

    uint32 m_transformIdx = UINT32_MAX;
    uint32 m_skinnedPaletteHandle = UINT32_MAX;
    uint32 m_skinnedBundleHandle = UINT32_MAX;
    // First slot of this node's per-instance LOD hysteresis state range on the GPU (one slot per mesh
    // instance, allocated at spawn when the node has any LOD chain; UINT32_MAX = none). The cull shader
    // addresses it as stateBase + instance ordinal via the per-frame node bias buffer.
    uint32 m_lodStateBase = UINT32_MAX;
    Sphere m_bounds;
    oc::vector<RendererVKLayout::InMeshInstance> m_meshInstances;
    oc::vector<oc::pair<uint16, uint16>> m_numInstancesPerMesh;
    // Instances with a LOD chain (the stored instance references the LOD0 mesh; the GPU cull redirects).
    // Kept CPU-side for the per-frame chain-warmth noteUse and the state-range allocation.
    struct LodInstance
    {
        uint32 instanceIdx = 0;
        uint32 lodGroupIdx = 0;
    };
    oc::vector<LodInstance> m_lodInstances;
};
