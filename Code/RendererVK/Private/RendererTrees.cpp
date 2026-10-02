module RendererVK;

import Core;
import Core.glm;
import Core.Transform;

import :Renderer;
import :Device;
import :Buffer;
import :Layout;
import :InstanceStream;

// The BAKED TREE RECORDS' CPU side (tree_cull.inc.glsl): sets of placed pieces uploaded ONCE to device-local
// memory. The culls build each piece's three records (bark, leaves, billboard) from the camera distance inside a
// range of the frame's instance stream; per frame the CPU only claims that range and notes the per-mesh bucket
// sizes. Nothing writes the range's stream entries.
//
// The GPU layouts below are MIRRORED in tree_cull.inc.glsl - keep them in step.

namespace
{
    constexpr uint32 TREE_RECORD_ABSENT = 0xFFFFFFFFu;
    constexpr uint32 TREE_RECORDS_PER_PIECE = 3; // bark, leaves, billboard

    // One representation: an InMeshInstance minus its node (meshIdx | materialIdx << 16, pipelineIdx | alphaMode << 16).
    struct TreeCullRecordGpu
    {
        uint32 meshMaterial = TREE_RECORD_ABSENT;
        uint32 pipelineAlpha = 0;
    };

    // A piece TYPE (one library piece of one species): its representations and its crossfade band.
    struct TreeCullTypeGpu
    {
        TreeCullRecordGpu bark;
        TreeCullRecordGpu barkFade;
        TreeCullRecordGpu leaves;
        TreeCullRecordGpu leavesFade;
        TreeCullRecordGpu billboard;
        float farDistance = 0.0f; // billboard switch distance (m); x the frame's distance scale
        float fadeWidth = 1.0f;   // crossfade band (m), centred on it
    };
    static_assert(sizeof(TreeCullTypeGpu) == 48);

    // A placed piece: its static transform and its band test.
    struct TreeCullPieceGpu
    {
        glm::vec4 posScale{ 0.0f };
        glm::vec4 quat{ 0.0f, 0.0f, 0.0f, 1.0f };
        glm::vec3 centre{ 0.0f };
        float radius = 0.0f;
        uint32 type = 0;
        uint32 lodStateBase = 0; // TREE_RECORDS_PER_PIECE LOD hysteresis slots
        uint32 pad0 = 0;
        uint32 pad1 = 0;
    };
    static_assert(sizeof(TreeCullPieceGpu) == 64);
}

// The baked tree records' UBO words (tree_cull.inc.glsl): the claimed range, the band scale, forceFar, and the
// far-tree volume's start (0 = none; the billboards draw through its fade-in band - an overlap, not a seam).
// Patched in present(): the UBO uploads in beginFrame, BEFORE renderTreeInstanceSet claims the range - left at
// that upload's 0 / 0, the culls read the range's never-written stream entries as instances (garbage mesh
// indices, out-of-bounds bucket writes).
void Renderer::uploadTreeCullUbo(PerFrameData& frameData)
{
    const bool treeVolume = m_treeCullCount > 0 && farTreesActive() && m_treeSets[m_treeCullSet].hasVolume;
    m_ubo.treeCull = glm::uvec4(m_treeCullBase, m_treeCullCount, 0u, 0u);
    m_ubo.treeCullParams = glm::vec4(m_treeCullDistanceScale, m_treeCullForceFar ? 1.0f : 0.0f,
        treeVolume ? oc::max(farTreesStart() + m_farTreeParams.overlap, 1.0f) : 0.0f,
        oc::max(m_foliageParams.shadowCascadeMargin, 0.0f)); // instanced_indirect_shadow.cs.glsl
    static_assert(offsetof(RendererVKLayout::Ubo, treeCullParams) == offsetof(RendererVKLayout::Ubo, treeCull) + sizeof(glm::uvec4));
    Globals::stagingManager.upload(frameData.ubo.getBuffer(), sizeof(glm::uvec4) + sizeof(glm::vec4), &m_ubo.treeCull,
        offsetof(RendererVKLayout::Ubo, treeCull));
}

Buffer& Renderer::treeCullPieces()
{
    return m_treeCullSet < (uint32)m_treeSets.size() && m_treeSets[m_treeCullSet].alive ? m_treeSets[m_treeCullSet].pieces : m_treeCullDummy;
}

Buffer& Renderer::treeCullTypes()
{
    return m_treeCullSet < (uint32)m_treeSets.size() && m_treeSets[m_treeCullSet].alive ? m_treeSets[m_treeCullSet].types : m_treeCullDummy;
}

uint32 Renderer::createTreeInstanceSet(oc::span<const TreeInstanceType> types, oc::span<const TreeInstancePiece> pieces)
{
    uint32 setId = 0;
    while (setId < (uint32)m_treeSets.size() && m_treeSets[setId].alive)
        ++setId;
    if (setId == (uint32)m_treeSets.size())
        m_treeSets.emplace_back();
    TreeInstanceSet& set = m_treeSets[setId];
    set.alive = true;
    set.numPieces = (uint32)pieces.size();

    auto record = [&](const TreeInstanceRep& rep)
    {
        TreeCullRecordGpu gpu;
        if (rep.mesh && rep.mesh->isValid())
        {
            gpu.meshMaterial = (uint32)rep.mesh->m_meshIdx | ((uint32)rep.material << 16);
            gpu.pipelineAlpha = (uint32)rep.pipeline | ((uint32)m_materials.items()[rep.material].alphaMode << 16);
        }
        return gpu;
    };
    oc::vector<TreeCullTypeGpu> gpuTypes(types.size());
    for (size_t t = 0; t < types.size(); ++t)
    {
        const TreeInstanceType& type = types[t];
        gpuTypes[t] = TreeCullTypeGpu{ record(type.bark), record(type.barkFade), record(type.leaves), record(type.leavesFade),
            record(type.billboard), type.farDistance, type.fadeWidth };
    }

    // Bucket sizes: a piece draws its bark mesh at most once (normal OR fade material - the same mesh), its
    // leaves once, its billboard once. Level-0 meshes: present() sizes a chain's buckets from its level 0.
    oc::unordered_map<uint16, uint32> meshCounts;
    auto count = [&](const TreeInstanceRep& a, const TreeInstanceRep& b)
    {
        const RenderMesh* mesh = a.mesh && a.mesh->isValid() ? a.mesh : (b.mesh && b.mesh->isValid() ? b.mesh : nullptr);
        if (mesh)
            ++meshCounts[mesh->m_meshIdx];
    };

    oc::vector<TreeCullPieceGpu> gpuPieces(pieces.size());
    set.lodStateBases.resize(pieces.size());
    for (size_t i = 0; i < pieces.size(); ++i)
    {
        const TreeInstancePiece& piece = pieces[i];
        const TreeInstanceType& type = types[piece.type];
        const Transform& transform = piece.transform;
        set.lodStateBases[i] = allocateLodStateRange(TREE_RECORDS_PER_PIECE);
        gpuPieces[i] = TreeCullPieceGpu{
            .posScale = glm::vec4(transform.pos, transform.scale),
            .quat = glm::vec4(transform.quat.x, transform.quat.y, transform.quat.z, transform.quat.w),
            .centre = piece.centre,
            .radius = piece.radius,
            .type = piece.type,
            .lodStateBase = set.lodStateBases[i],
        };
        count(type.bark, type.barkFade);
        count(type.leaves, type.leavesFade);
        count(type.billboard, type.billboard);
    }
    set.meshCounts.assign(meshCounts.begin(), meshCounts.end());

    // The culls' data: written once, device-local (staged in chunks).
    auto uploadDeviceLocal = [](Buffer& buffer, const void* data, size_t bytes, const char* name)
    {
        buffer.initialize(oc::max<size_t>(bytes, 16), vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, name);
        constexpr size_t CHUNK = 16 * 1024 * 1024;
        for (size_t offset = 0; offset < bytes; offset += CHUNK)
            buffer.upload(oc::min(CHUNK, bytes - offset), (const uint8*)data + offset, offset);
    };
    uploadDeviceLocal(set.pieces, gpuPieces.data(), gpuPieces.size() * sizeof(TreeCullPieceGpu), "TreeCullPieces");
    uploadDeviceLocal(set.types, gpuTypes.data(), gpuTypes.size() * sizeof(TreeCullTypeGpu), "TreeCullTypes");

    // The far-tree volume's data: host-visible, read through device addresses at its (rare) rebakes.
    auto upload = [](Buffer& buffer, const void* data, size_t bytes, const char* name)
    {
        buffer.initialize(oc::max<size_t>(bytes, 16), vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eHostVisible, false, name, BufferHostAccess::eSequentialWrite);
        if (bytes > 0)
        {
            memcpy(buffer.mapMemory().data(), data, bytes);
            buffer.flushMappedMemory(bytes);
        }
    };

    // The far-tree volume's view (TreeVolumePipeline): per type its extinction mip chain (box filtered down to
    // 1^3), per piece its transform. Types without density add nothing to the volume.
    oc::vector<TreeVolumeTypeGpu> volumeTypes(types.size());
    oc::vector<float> volumeData;
    for (size_t t = 0; t < types.size(); ++t)
    {
        const TreeInstanceType& type = types[t];
        if (!type.density || type.densityRes == 0)
            continue;
        TreeVolumeTypeGpu& gpu = volumeTypes[t];
        gpu.boxMin = type.densityMin;
        gpu.boxMax = type.densityMax;
        gpu.res = type.densityRes;
        gpu.offset = (uint32)volumeData.size();
        gpu.albedo = glm::vec4(type.albedo, 1.0f);
        uint32 res = type.densityRes;
        const size_t level0 = volumeData.size();
        volumeData.insert(volumeData.end(), type.density, type.density + (size_t)res * res * res);
        size_t prev = level0;
        while (res > 1)
        {
            const uint32 half = res / 2;
            const size_t next = volumeData.size();
            volumeData.resize(next + (size_t)half * half * half);
            for (uint32 z = 0; z < half; ++z)
            for (uint32 y = 0; y < half; ++y)
            for (uint32 x = 0; x < half; ++x)
            {
                float sum = 0.0f;
                for (uint32 k = 0; k < 8; ++k)
                    sum += volumeData[prev + (x * 2 + (k & 1)) + res * ((y * 2 + ((k >> 1) & 1)) + res * (z * 2 + (k >> 2)))];
                volumeData[next + x + half * (y + half * z)] = sum * 0.125f;
            }
            prev = next;
            res = half;
        }
        set.hasVolume = true;
    }
    oc::vector<TreeVolumePieceGpu> volumePieces(pieces.size());
    for (size_t i = 0; i < pieces.size(); ++i)
    {
        const Transform& transform = pieces[i].transform;
        volumePieces[i].posScale = glm::vec4(transform.pos, transform.scale);
        volumePieces[i].quat = glm::vec4(transform.quat.x, transform.quat.y, transform.quat.z, transform.quat.w);
        volumePieces[i].type = pieces[i].type;
    }
    upload(set.volumeTypes, volumeTypes.data(), volumeTypes.size() * sizeof(TreeVolumeTypeGpu), "TreeVolumeTypes");
    upload(set.volumeData, volumeData.data(), volumeData.size() * sizeof(float), "TreeVolumeData");
    upload(set.volumePieces, volumePieces.data(), volumePieces.size() * sizeof(TreeVolumePieceGpu), "TreeVolumePieces");
    m_treeVolume.markDirty();
    return setId;
}

// "Far start" scaled with the camera's height h above the ground under it: sqrt(start^2 + h^2) is the 3D distance
// of a ground tree at HORIZONTAL distance start. The billboards hand over (tree_cull) and the march starts there,
// so from the air the hand-over stays at horizontal "Far start" - where the volume's fixed ring begins - instead of
// moving in under the camera (the billboards vanished below it with nothing in their place).
float Renderer::farTreesStart() const
{
    const float ground = std::isnan(m_farTreeCameraGround) ? m_terrain.getHeightMap().getUserParam() : m_farTreeCameraGround;
    const float h = oc::max(m_cameraPos.y - ground, 0.0f);
    return std::sqrt(m_farTreeParams.startDistance * m_farTreeParams.startDistance + h * h);
}

bool Renderer::farTreesActive() const
{
    if (!m_farTreeParams.enabled || m_sceneViewCount != 1)
        return false;
    for (const TreeInstanceSet& set : m_treeSets)
        if (set.alive && set.hasVolume)
            return true;
    return false;
}

void Renderer::recordFarTrees(uint32 frameIdx, vk::CommandBuffer primary)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    oc::small_vector<TreeVolumePipeline::Source, 4> sources;
    for (const TreeInstanceSet& set : m_treeSets)
        if (set.alive && set.hasVolume)
            sources.push_back(TreeVolumePipeline::Source{ set.volumePieces.getDeviceAddress(), set.volumeTypes.getDeviceAddress(),
                set.volumeData.getDeviceAddress(), set.numPieces });
    const TreeVolumePipeline::RecordParams params{
        .ubo = frameData.ubo,
        .sceneDepthView = frameData.sceneColor.getDepthView(0),
        .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
        .terrainView = m_terrain.getHeightMap().getView(),
        .terrainSampler = m_terrain.getHeightMap().getSampler(),
        .cameraPos = m_cameraPos,
        .startDistance = farTreesStart(),
        .sources = oc::span<const TreeVolumePipeline::Source>(sources.data(), sources.size()),
        .settings = m_farTreeParams,
        .frameNumber = m_frameCounter,
    };
    m_gpuProfiler.beginScope(primary, "Far trees");
    m_treeVolume.record(primary, frameIdx, params);
    m_gpuProfiler.endScope(primary);
}

void Renderer::recordFarTreesApply(uint32 frameIdx)
{
    CommandBuffer& cb = m_perFrameData[frameIdx].farTreesApplyCommandBuffer;
    beginScenePassSecondary(frameIdx, cb);
    vk::CommandBuffer vkCb = cb.getCommandBuffer();
    const vk::Extent2D extent = renderExtent();
    const glm::ivec2 vpMin = m_renderRect.min;
    const glm::ivec2 vpSize = m_renderRect.getSize();
    vkCb.setViewport(0, vk::Viewport{ .x = 0.0f, .y = 0.0f, .width = (float)extent.width, .height = (float)extent.height, .minDepth = 0.0f, .maxDepth = 1.0f });
    vkCb.setScissor(0, vk::Rect2D{ .offset = vk::Offset2D{ vpMin.x, vpMin.y }, .extent = vk::Extent2D{ (uint32)vpSize.x, (uint32)vpSize.y } });
    m_treeVolume.recordApply(cb, frameIdx);
    cb.end();
}

void Renderer::destroyTreeInstanceSet(uint32 setId)
{
    if (setId >= (uint32)m_treeSets.size() || !m_treeSets[setId].alive)
        return;
    TreeInstanceSet& set = m_treeSets[setId];
    // In-flight frames still read its buffers and nodes: drain (a set dies on a respawn / reload, never per frame).
    const vk::Result waitResult = Globals::device.graphicsQueueWaitIdle();
    assert(waitResult == vk::Result::eSuccess);
    (void)waitResult;
    if (m_treeCullSet == setId)
    {
        // The recorded culls bind its buffers: back to the dummies, and no range this frame.
        m_treeCullSet = UINT32_MAX;
        m_treeCullBase = 0;
        m_treeCullCount = 0;
        setHaveToRecordCommandBuffers();
    }
    set.pieces.destroy();
    set.types.destroy();
    set.volumePieces.destroy();
    set.volumeTypes.destroy();
    set.volumeData.destroy();
    set.hasVolume = false;
    m_treeVolume.markDirty();
    {
        const std::lock_guard lock(m_spawnMutex);
        for (uint32 base : set.lodStateBases)
            m_meshLods.releaseStateRange(base, TREE_RECORDS_PER_PIECE);
    }
    set.lodStateBases.clear();
    set.meshCounts.clear();
    set.numPieces = 0;
    set.alive = false;
}

void Renderer::renderTreeInstanceSet(uint32 setId, const glm::vec3& cameraPos, float distanceScale, bool forceFar)
{
    if (setId >= (uint32)m_treeSets.size())
        return;
    TreeInstanceSet& set = m_treeSets[setId];
    if (!set.alive || set.numPieces == 0)
        return;
    // The culls carry one set's records per frame (the distance test runs from the main view: the culls' camera).
    (void)cameraPos;
    assert(m_treeCullCount == 0 && "renderTreeInstanceSet: one tree set per frame");
    if (m_treeCullCount != 0)
        return;
    if (m_treeCullSet != setId)
    {
        m_treeCullSet = setId; // the recorded culls bind its buffers
        setHaveToRecordCommandBuffers();
    }

    const uint32 base = m_instances.claimInstances(set.numPieces * TREE_RECORDS_PER_PIECE);
    if (base == UINT32_MAX)
        return; // full this frame: the capacity grows at the next beginFrame
    for (const auto& [meshIdx, numInstances] : set.meshCounts)
        m_instances.noteMeshInstances(meshIdx, numInstances);
    m_treeCullBase = base;
    m_treeCullCount = set.numPieces * TREE_RECORDS_PER_PIECE;
    m_treeCullDistanceScale = distanceScale;
    m_treeCullForceFar = forceFar;
}
