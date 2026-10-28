export module RendererVK:RenderNode;

import Core;
import Core.Sphere;
import Core.Transform;

import :Layout;
import :RenderNodeState; // the state and the transform slots

// RAII handle to a spawned renderer instance (movable, like PhysicsBody). Destroying the handle recycles its renderer resources: the transform slot goes on a
// free list, and a skinned node's whole allocation bundle is parked for reuse by the next spawnSkinnedNode of the same container. The state is in
// RenderNodeState.ixx; the multi-line bodies are below the class.
export class RenderNode final : public RenderNodeState
{
public:

    // ---- Lifetime ----

    RenderNode() = default;
    RenderNode(RenderNode&& other) noexcept { moveFrom(oc::move(other)); }
    RenderNode& operator=(RenderNode&& other) noexcept;
    ~RenderNode() { destroy(); }

    bool isValid() const { return m_transformIdx != UINT32_MAX; }
    void destroy();

    // ---- Transform and bounds ----

    // The only write path: unchanged transforms cost one compare, changed ones set one pending-upload bit per frame in flight, which Renderer::renderNode
    // consumes for its slot (writing through a mutable getTransform would bypass the dirty tracking). One owner per node: setTransform and renderNode of the
    // same node never run concurrently.
    void setTransform(const Transform& transform);
    const Transform& getTransform() const { return Globals::renderNodeTransforms[m_transformIdx]; }

    const Sphere& getLocalBounds() const { return m_bounds; }
    Sphere getWorldBounds() const;

    // ---- Drawing ----

    size_t getNumMeshInstances() const { return m_meshInstances.size(); }

    // Per-entity TINT: rewrites every mesh instance's material index (pair with RenderMaterial::getOrCreateSolidColor). Takes effect the next time the node
    // is submitted - instances are copied from this node's CPU list every renderNode() call. Main thread.
    void setMaterialOverride(uint16 materialIdx);

    // The PASS_* bits Renderer::renderNode(node) pushes this node with (PASS_ALL by default; 0 = the push is skipped). Lets an owner decide per node once and
    // push a whole list blindly - the ocean marks its sectors PASS_MAIN (or 0 when dry). Set on main; read by the pushing job.
    void setPassMask(uint32 passMask) { m_passMask = uint8(passMask); }
    uint32 getPassMask() const { return m_passMask; }

    // True only for nodes spawned via ObjectContainer::spawnSkinnedNode. Pass the node to Renderer::setSkinningPalette each frame with the AnimationPlayer's
    // bone palette (the palette handle lives in the node's skinned bundle).
    bool isSkinned() const { return m_skinnedBundleHandle != UINT32_MAX; }

    // ---- Not copyable ----

    RenderNode(const RenderNode&) = delete;
    RenderNode& operator=(const RenderNode&) = delete;
};
static_assert(sizeof(RenderNode) <= 56);

// ---- Inline bodies ----

inline RenderNode& RenderNode::operator=(RenderNode&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        moveFrom(oc::move(other));
    }
    return *this;
}

inline void RenderNode::setTransform(const Transform& transform)
{
    Transform& current = Globals::renderNodeTransforms[m_transformIdx];
    if (current.pos == transform.pos && current.scale == transform.scale && current.quat == transform.quat)
        return;
    current = transform;
    m_transformUploadState |= ALL_FRAMES_DIRTY;
}

inline Sphere RenderNode::getWorldBounds() const
{
    const Transform& transform = getTransform();
    return Sphere(transform.quat * m_bounds.pos * transform.scale + transform.pos, m_bounds.radius * transform.scale);
}

inline void RenderNode::setMaterialOverride(uint16 materialIdx)
{
    for (RendererVKLayout::InMeshInstance& instance : m_meshInstances)
        instance.materialIdx = materialIdx;
}
