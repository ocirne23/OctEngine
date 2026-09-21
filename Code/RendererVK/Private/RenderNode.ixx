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

    // The only write path: unchanged transforms cost one compare, changed ones set one pending-upload
    // bit per frame in flight, which Renderer::renderNode consumes for its slot (writing through a
    // mutable getTransform would bypass the dirty tracking). One owner per node: setTransform and
    // renderNode of the same node never run concurrently.
    inline void setTransform(const Transform& transform)
    {
        Transform& current = Globals::renderNodeTransforms[m_transformIdx];
        if (current.pos == transform.pos && current.scale == transform.scale && current.quat == transform.quat)
            return;
        current = transform;
        m_transformUploadState |= ALL_FRAMES_DIRTY;
    }

    inline const Transform& getTransform() const
    {
        return Globals::renderNodeTransforms[m_transformIdx];
    }

    inline size_t getNumMeshInstances() const { return m_meshInstances.size(); }

    // Per-entity TINT: rewrites every mesh instance's material index (pair with
    // Renderer::getOrCreateSolidColorMaterial). Takes effect the next time the node is submitted -
    // instances are copied from this node's CPU list every renderNode() call. Main thread.
    inline void setMaterialOverride(uint16 materialIdx)
    {
        for (RendererVKLayout::InMeshInstance& instance : m_meshInstances)
            instance.materialIdx = materialIdx;
    }

    // True only for nodes spawned via ObjectContainer::spawnSkinnedNode. Pass the node to
    // Renderer::setSkinningPalette each frame with the AnimationPlayer's bone palette (the palette
    // handle lives in the node's skinned bundle).
    inline bool isSkinned() const { return m_skinnedBundleHandle != UINT32_MAX; }

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
        m_skinnedBundleHandle = other.m_skinnedBundleHandle;
        m_lodStateBase = other.m_lodStateBase;
        m_transformUploadState = other.m_transformUploadState;
        m_bounds = other.m_bounds;
        m_meshInstances = oc::move(other.m_meshInstances);
        other.m_transformIdx = UINT32_MAX;
        other.m_skinnedBundleHandle = UINT32_MAX;
        other.m_lodStateBase = UINT32_MAX;
    }

    // One cache line. Everything the push needs beyond these is derived from renderer tables: the per-mesh
    // instance counts from m_meshInstances, an instance's LOD chain from MeshLodRegistry
    // (the stored instance references the LOD0 mesh; the GPU cull redirects), the skinning palette
    // from the skinned bundle.
    uint32 m_transformIdx = UINT32_MAX;
    uint32 m_skinnedBundleHandle = UINT32_MAX;
    // First slot of this node's per-instance LOD hysteresis state range on the GPU (one slot per mesh
    // instance, allocated at spawn when the node has any LOD chain; UINT32_MAX = none). The cull shader
    // addresses it as stateBase + instance ordinal via the per-frame node bias buffer.
    uint32 m_lodStateBase = UINT32_MAX;
    // Sparse transform upload state. Low bits: one pending bit per frame in flight, cleared by
    // Renderer::renderNode when it copies the transform into that slot's mapped buffer (mutable: the
    // push takes a const node). High bits: the renderer's node-buffer generation at the last push; a
    // mismatch = the GPU buffers were recreated, so all bits count as set. The capacity doubles per
    // grow, so there are at most 32 generations.
    static constexpr uint8 ALL_FRAMES_DIRTY = uint8((1u << RendererVKLayout::NUM_FRAMES_IN_FLIGHT) - 1);
    static_assert(8 - RendererVKLayout::NUM_FRAMES_IN_FLIGHT >= 6, "generation needs 6 bits");
    mutable uint8 m_transformUploadState = ALL_FRAMES_DIRTY;
    Sphere m_bounds;
    oc::vector<RendererVKLayout::InMeshInstance> m_meshInstances;
};
static_assert(sizeof(RenderNode) <= 56);
