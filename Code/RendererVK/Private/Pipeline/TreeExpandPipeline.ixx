export module RendererVK:TreeExpandPipeline;

import Core;
import Core.glm;

import :VK;
import :ComputePipeline;

// GPU expansion of procedural tree pieces into THE FRAME'S INSTANCE STREAM (tree_expand.cs.glsl). One thread
// per placed piece picks its representation from the camera distance - the mesh LOD chains, the
// mesh <-> billboard crossfade (both), or the billboard - and writes up to three InMeshInstance records into a
// range the CPU claimed for the frame, plus the piece node's stamped pass mask and LOD state bias. Unused
// records point at a dummy node with pass mask 0, which every cull skips. After it the regular cull, LOD
// selection, shadow cull and TLAS writer see ordinary instances. No descriptor set: everything goes in as
// device addresses, re-read every frame (the instance buffers are re-created when they grow).
//
// The GPU layouts below are MIRRORED in tree_expand.cs.glsl - keep them in step.

// One representation: an InMeshInstance minus its node (meshIdx | materialIdx << 16, pipelineIdx | alphaMode
// << 16). meshMaterial = TREE_EXPAND_ABSENT: the piece has none.
export constexpr uint32 TREE_EXPAND_ABSENT = 0xFFFFFFFFu;
export constexpr uint32 TREE_EXPAND_RECORDS_PER_PIECE = 3; // bark, leaves, billboard

export struct TreeExpandRecordGpu
{
    uint32 meshMaterial = TREE_EXPAND_ABSENT;
    uint32 pipelineAlpha = 0;
};

// A piece TYPE (one library piece of one species): its representations and its crossfade band.
export struct TreeExpandTypeGpu
{
    TreeExpandRecordGpu bark;
    TreeExpandRecordGpu barkFade;
    TreeExpandRecordGpu leaves;
    TreeExpandRecordGpu leavesFade;
    TreeExpandRecordGpu billboard;
    float farDistance = 0.0f; // billboard switch distance (m); x the frame's distance scale
    float fadeWidth = 1.0f;   // crossfade band (m), centred on it
};
static_assert(sizeof(TreeExpandTypeGpu) == 48);

// A placed piece.
export struct TreeExpandPieceGpu
{
    glm::vec3 centre{ 0.0f }; // world centre of its far representation (the band test)
    float radius = 0.0f;
    uint32 transformIdx = 0;  // its render node slot: the MESH records (bark, leaves)
    uint32 lodStateBase = 0;  // TREE_EXPAND_RECORDS_PER_PIECE LOD hysteresis slots
    uint32 typeIdx = 0;
    uint32 billboardNode = 0; // a second node, same transform: the BILLBOARD record - its own pass mask, so the
                              // billboard stands in for the piece in the shadow / GI / RT passes while the mesh
                              // draws (see tree_expand.cs.glsl)
};
static_assert(sizeof(TreeExpandPieceGpu) == 32);

export class TreeExpandPipeline final
{
public:
    // One set's expansion for this frame.
    struct Dispatch
    {
        vk::DeviceAddress pieces = 0;
        vk::DeviceAddress types = 0;
        uint32 numPieces = 0;
        uint32 baseInstance = 0;    // the claimed range: TREE_EXPAND_RECORDS_PER_PIECE per piece
        glm::vec4 cameraPosScale{ 0.0f }; // xyz camera, w = the far distance scale
        uint32 forceFar = 0;
        // InstanceStream::stampedPassMask(.., this frame) of PASS_ALL, of MAIN (a mesh beside a billboard) and of
        // SHADOW | GI (a billboard only standing in for the mesh).
        uint32 stampedAll = 0;
        uint32 stampedMain = 0;
        uint32 stampedProxy = 0;
        uint32 dummyNode = 0;
        uint32 identityOffset = 0;  // the shared identity instance offset (spawnMeshNode's)
    };

    void initialize();
    void reloadShaders();
    // At the top of the primary, before every reader of the instance stream; ends with the barrier to them.
    void record(vk::CommandBuffer cb, vk::DeviceAddress instances, vk::DeviceAddress passMasks, vk::DeviceAddress lodStateBias,
        oc::span<const Dispatch> dispatches);

private:
    void buildLayout(ComputePipelineLayout& layout);

    ComputePipeline m_pipeline;
};
