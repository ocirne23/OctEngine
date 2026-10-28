export module RendererVK:RenderNodeState;

import Core;
import Core.glm;
import Core.Sphere;
import Core.Transform;
import Threading;

import :Layout;
import :SlotAlloc;

// The render-node transform slots, LOCK-FREE (SlotAlloc): a slot never moves, so a spawn on one worker never reallocates the array under a setTransform /
// push on another (the old oc::vector did).
export class TransformSlots final
{
public:
    Transform& operator[](uint32 idx) { return m_transforms[idx]; }
    const Transform& operator[](uint32 idx) const { return m_transforms[idx]; }

    uint32 allocate(const Transform& transform)
    {
        const uint32 idx = m_slots.allocate(1);
        m_transforms.ensure(idx) = transform;
        return idx;
    }
    void free(uint32 idx) { m_slots.release(idx, 1); }

    // Every slot ever handed out (live or free): the node buffers' required size.
    uint32 highWater() const { return m_slots.highWater(); }
    uint32 numLive() const { return m_slots.highWater() - m_slots.numFree(); } // stats: may lag

private:
    SlotRangeAllocator m_slots;
    PagedArray<Transform> m_transforms;
};

export namespace Globals
{
    TransformSlots renderNodeTransforms;
}

// Declared before the friend lines below, which would otherwise introduce the names (RendererVK.fwd has extern "C++" ones).
export class ObjectContainer;
export class Renderer;

// Everything of RenderNode but its public API (RenderNode.ixx).
export class RenderNodeState
{
protected:

    friend class ObjectContainer;
    friend class Renderer;
    friend class RendererState;

    void moveFrom(RenderNodeState&& other) noexcept
    {
        m_transformIdx = other.m_transformIdx;
        m_skinnedBundleHandle = other.m_skinnedBundleHandle;
        m_lodStateBase = other.m_lodStateBase;
        m_transformUploadState = other.m_transformUploadState;
        m_passMask = other.m_passMask;
        m_bounds = other.m_bounds;
        m_meshInstances = oc::move(other.m_meshInstances);
        other.m_transformIdx = UINT32_MAX;
        other.m_skinnedBundleHandle = UINT32_MAX;
        other.m_lodStateBase = UINT32_MAX;
    }

    // One cache line. Everything the push needs beyond these is derived from renderer tables: the per-mesh instance counts from m_meshInstances, an
    // instance's LOD chain from MeshLodRegistry (the stored instance references the LOD0 mesh; the GPU cull redirects), the skinning palette from the skinned
    // bundle.
    uint32 m_transformIdx = UINT32_MAX;
    uint32 m_skinnedBundleHandle = UINT32_MAX;
    // First slot of this node's per-instance LOD hysteresis state range on the GPU (one slot per mesh instance, allocated at spawn when the node has any LOD
    // chain; UINT32_MAX = none). The cull shader addresses it as stateBase + instance ordinal via the per-frame node bias buffer.
    uint32 m_lodStateBase = UINT32_MAX;
    // Sparse transform upload state. Low bits: one pending bit per frame in flight, cleared by Renderer::renderNode when it copies the transform into that
    // slot's mapped buffer (mutable: the push takes a const node). High bits: the renderer's node-buffer generation at the last push; a mismatch = the GPU
    // buffers were recreated, so all bits count as set. The capacity doubles per grow, so there are at most 32 generations.
    static constexpr uint8 ALL_FRAMES_DIRTY = uint8((1u << RendererVKLayout::NUM_FRAMES_IN_FLIGHT) - 1);
    static_assert(8 - RendererVKLayout::NUM_FRAMES_IN_FLIGHT >= 6, "generation needs 6 bits");
    mutable uint8 m_transformUploadState = ALL_FRAMES_DIRTY;
    uint8 m_passMask = uint8(RendererVKLayout::PASS_ALL); // in the padding before m_bounds
    static_assert(RendererVKLayout::PASS_ALL <= 0xFF);
    Sphere m_bounds;
    oc::vector<RendererVKLayout::InMeshInstance> m_meshInstances;
};
