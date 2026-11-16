export module RendererVK:InstanceStream;

import Core;
import Core.Transform;
import :VK;
import :Buffer;
import :Layout;
import :RenderNodeState; // Globals::renderNodeTransforms

// THE per-frame instance stream: the host-visible, mapped buffers the entity pass writes straight into
// during Renderer::renderNode, one set per frame slot, plus the CPU state that addresses them.
//
// Two things are worth knowing before touching it:
//  * the instance claim is a LOCK-FREE monotonic bump (`claimInstances`). A claim past the capacity is
//    NOT rolled back - un-bumping a non-top claim would corrupt the cursor - so the failed claim is
//    recorded instead, present() clamps the counter to it, and the successful claims stay one
//    contiguous prefix. The capacity then grows at the next beginFrame.
//  * a render-node transform slot is recycled the moment its node dies, so the buffers carry a
//    GENERATION: bumping it marks every node dirty again, which is how a re-created buffer refills.
//  * the MOTION VECTORS need every node's transform LAST frame. The other slot cannot serve (the CPU may
//    write it while this frame's GPU work reads it), so after the cull the GPU copies this slot's transforms
//    and node states into ONE device-local previous set (recordPrevCopy), which next frame's cull reads. A
//    pass mask carries its push frame above the PASS_* byte (renderNode), so the cull knows whether the
//    previous transform is really last frame's: a node not pushed last frame (off screen, just spawned into
//    a recycled slot) draws without motion instead of with a stale transform.
//  * the per-mesh instance counts (the cull's bucket sizes) are fixed arrays over every possible mesh index
//    (a meshIdx is 16 bits): a mesh spawned on a worker during the pushes never reallocates them under an
//    add. The first add of a mesh in a frame also lists it (`touchedMeshes`), so present() and beginFrame
//    walk only the meshes the frame used, not every registered one.
//
// Growth calls back out: onGpuIdle before the buffers are re-created, then onInvalidate (a re-record),
// and for the instance buffers the cull pipelines' own resize.
export class InstanceStream final
{
public:
    // One render node's per-push word pair (instance_cull.inc.glsl in_nodeStates). ONE record, so a push writes one
    // partial line of the write-combined mapping, not one in each of two buffers, and the cull reads both in one load.
    struct NodeState
    {
        uint32 passMask;     // stampedPassMask: the PASS_* byte + the push frame
        int32 lodStateBias;  // LOD hysteresis state slot = instanceIdx + bias (stateBase - startIdx); only for nodes with a LOD chain
    };
    static_assert(sizeof(NodeState) == 8);

    // One frame slot's mapped buffers. Everything here is written on the CPU and read by the GPU in
    // the same frame - nothing persists across slots.
    struct FrameSlot
    {
        Buffer transforms;
        Buffer nodeStates;   // NodeState per render node, written at push time
        Buffer meshInstances;
        Buffer firstInstances;
        Buffer meshCount;    // [0] registered mesh count: the slots the culls' draw-list compaction walks

        oc::span<RendererVKLayout::RenderNodeTransform> mappedTransforms;
        oc::span<NodeState> mappedNodeStates;
        oc::span<RendererVKLayout::InMeshInstance> mappedMeshInstances;
        oc::span<uint32> mappedFirstInstances;
        oc::span<uint32> mappedMeshCount;
    };

    void initialize(uint32 maxUniqueMeshes, oc::function<void()> onGpuIdle, oc::function<void()> onInvalidate,
        oc::function<void(uint32)> onInstanceCapacityGrown);

    FrameSlot& slot(uint32 frameIdx) { return m_slots[frameIdx]; }
    const FrameSlot& slot(uint32 frameIdx) const { return m_slots[frameIdx]; }

    // ---- Last frame's node transforms (the motion vectors; see the header) ----
    Buffer& getPrevTransforms() { return m_prevTransforms; }
    Buffer& getPrevNodeStates() { return m_prevNodeStates; }
    // In the primary, after the main cull: this slot's first numNodes transforms + node states -> the previous set.
    void recordPrevCopy(vk::CommandBuffer cb, uint32 frameIdx, uint32 numNodes);
    // The pass-mask word renderNode writes: the PASS_* byte + the push frame (24 bits; the cull compares
    // against u_frameIndex - 1).
    static uint32 stampedPassMask(uint32 passMask, uint32 frameIndex) { return passMask | (frameIndex << 8); }

    // ---- The push path (jobs, lock-free) ----
    // Claims `count` contiguous instance slots, or UINT32_MAX when this frame is full (the caller drops
    // the node; the capacity grows at the next beginFrame).
    uint32 claimInstances(uint32 count);
    // One add per mesh and push (RenderBatch: per mesh and flush), never one per instance: a crowd of one mesh adds to ONE
    // counter from every worker.
    void noteMeshInstances(uint16 meshIdx, uint32 count)
    {
        if (count == 0)
            return; // a zero add would list the mesh, and its first real add would list it again
        if (oc::atomic_ref<uint32>(m_numInstancesPerMesh[meshIdx]).fetch_add(count, oc::memory_order_relaxed) == 0)
            m_touchedMeshes[oc::atomic_ref<uint32>(m_numTouchedMeshes).fetch_add(1, oc::memory_order_relaxed)] = meshIdx;
    }

    // ---- Render node transform slots (spawn path, lock-free: TransformSlots) ----
    // NEVER grows the node buffers: this runs mid-frame, and re-creating them would drop the transforms, pass masks and
    // LOD biases every node already pushed this frame (a one-frame flicker of everything). A node past the capacity is
    // skipped by renderNode until growToPendingDemand grows at the next beginFrame. A reused slot uploads at its
    // first push: a fresh RenderNode starts all-dirty.
    uint32 allocateTransform(const Transform& transform) { return m_transforms.allocate(transform); }
    void freeTransform(uint32 idx) { m_transforms.free(idx); }
    Transform& getTransform(uint32 idx) { return m_transforms[idx]; }
    uint32 getNumTransforms() const { return m_transforms.highWater(); }
    uint32 getNumLiveTransforms() const { return m_transforms.numLive(); }
    uint8 getBufferGeneration() const { return m_bufferGeneration; }

    // ---- Per-frame bookkeeping ----
    void beginFrame();                   // clears the counters for a fresh push pass
    // Applies a capacity last frame's claims or transform allocations overflowed past (called before
    // this frame writes anything - growing mid-frame would drop the nodes already pushed).
    void growToPendingDemand(uint32 currentFrameIdx);
    uint32 getInstanceCount() const { return m_instanceCounter; }
    // present(): claims past the capacity never wrote, so the valid prefix ends at the smallest one.
    void clampInstanceCountToOverflow() { m_instanceCounter = oc::min(m_instanceCounter, m_instanceOverflowStart); }
    uint32 getNumInstancesForMesh(uint32 meshIdx) const { return m_numInstancesPerMesh[meshIdx]; }
    // Every mesh with instances this frame, once each, in no order (present(), after the pushes have joined).
    oc::span<const uint16> getTouchedMeshes() const { return { m_touchedMeshes.data(), m_numTouchedMeshes }; }

    uint32 getMaxRenderNodes() const { return m_maxRenderNodes; }
    uint32 getMaxInstances() const { return m_maxInstances; }

    // ---- Capacity ----
    void growRenderNodes(uint32 needed);
    void growInstances(uint32 needed, uint32 currentFrameIdx);
    // A unique-mesh capacity growth re-creates the per-slot first-instance buffers (nothing to preserve:
    // they are rewritten every present()).
    void onUniqueMeshCapacityGrown(uint32 maxUniqueMeshes);

private:
    void createNodeBuffers(FrameSlot& s);
    void createInstanceBuffer(FrameSlot& s);
    void createFirstInstanceBuffer(FrameSlot& s);
    void createPrevNodeBuffers();

    oc::array<FrameSlot, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_slots;
    Buffer m_prevTransforms; // device-local, one set for every slot: last frame's transforms (recordPrevCopy)
    Buffer m_prevNodeStates; // ... and its node states (the cull reads the stamped pass masks)
    TransformSlots& m_transforms = Globals::renderNodeTransforms;
    static constexpr uint32 MESH_INDEX_RANGE = 1u << 16; // InMeshInstance::meshIdx is 16 bits
    oc::vector<uint32> m_numInstancesPerMesh = oc::vector<uint32>(MESH_INDEX_RANGE, 0u); // never resized (see the header)
    oc::vector<uint16> m_touchedMeshes = oc::vector<uint16>(MESH_INDEX_RANGE, uint16(0));
    uint32 m_numTouchedMeshes = 0;

    oc::function<void()> m_onGpuIdle;
    oc::function<void()> m_onInvalidate;
    oc::function<void(uint32)> m_onInstanceCapacityGrown;

    uint32 m_maxRenderNodes = RendererVKLayout::INITIAL_RENDER_NODES;
    uint32 m_maxInstances = RendererVKLayout::INITIAL_INSTANCE_DATA;
    uint32 m_maxUniqueMeshes = 0;
    uint8 m_bufferGeneration = 0; // bumped when the per-frame node buffers are re-created (RenderNode dirty bits)

    uint32 m_instanceCounter = 0;
    uint32 m_pendingMaxInstances = 0;
    uint32 m_instanceOverflowStart = UINT32_MAX; // smallest failed claim this frame
};
