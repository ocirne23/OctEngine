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
// memory. The culls build each piece's four records from the camera distance inside a range of the frame's instance
// stream (tree_cull.inc.glsl: what each slot holds per type and band); per frame the CPU only lists the drawn
// chunks' pieces, claims that range and notes the chunks' per-mesh bucket sizes. Nothing writes the range's stream
// entries.
//
// The GPU layouts below are MIRRORED in tree_cull.inc.glsl - keep them in step.

namespace
{
    constexpr uint32 TREE_RECORD_ABSENT = 0xFFFFFFFFu;
    constexpr uint32 TREE_RECORDS_PER_PIECE = 4; // at most 4 representations draw at once (tree_cull.inc.glsl)

    // One representation: an InMeshInstance minus its node (meshIdx | materialIdx << 16, pipelineIdx | alphaMode << 16).
    struct TreeCullRecordGpu
    {
        uint32 meshMaterial = TREE_RECORD_ABSENT;
        uint32 pipelineAlpha = 0;
    };

    // A piece TYPE (one library piece of one species): its representations and its crossfade bands.
    struct TreeCullTypeGpu
    {
        TreeCullRecordGpu bark;
        TreeCullRecordGpu barkFade;
        TreeCullRecordGpu leaves;
        TreeCullRecordGpu leavesFade;
        TreeCullRecordGpu billboard;
        TreeCullRecordGpu trunk;      // the mid tier (tree_cull.inc.glsl): the trunk on its own ...
        TreeCullRecordGpu trunkFade;
        TreeCullRecordGpu cardsIn;    // ... and the merged branch-card mesh
        TreeCullRecordGpu cardsOut;
        float farDistance = 0.0f; // billboard switch distance (m); x the frame's distance scale
        float fadeWidth = 1.0f;   // crossfade band (m), centred on it
        float midDistance = 0.0f; // the mid tier from here (m; x the scale); 0 = none
        float midFadeWidth = 1.0f;
        float shadowDistance = 0.0f; // no sun shadow beyond this from the cascades' centre (m); 0 = no limit
        uint32 pad0 = 0;
    };
    static_assert(sizeof(TreeCullTypeGpu) == 96);

    // A placed piece: its static transform and its band test.
    struct TreeCullPieceGpu
    {
        glm::vec4 posScale{ 0.0f };
        glm::vec4 quat{ 0.0f, 0.0f, 0.0f, 1.0f };
        glm::vec3 centre{ 0.0f };
        float radius = 0.0f;
        uint32 type = 0;
        uint32 pad2 = 0;         // (was the LOD hysteresis base: tree meshes have no LOD chains)
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
// y = the range (TREE_RECORDS_PER_PIECE per piece). z = the culls' THREAD count (their dispatch,
// IndirectCullComputePipeline::update): one per stream instance outside the range, one per PIECE inside it (w = the
// piece count) - tree_cull.inc.glsl's treeCullThreadInstance.
void Renderer::uploadTreeCullUbo(PerFrameData& frameData)
{
    const bool treeVolume = m_treeCullPieces > 0 && m_treeCullSet < (uint32)m_treeSets.size() && farTreesActive()
        && m_treeSets[m_treeCullSet].hasVolume;
    m_ubo.treeCull = glm::uvec4(m_treeCullBase, m_treeCullCount, m_instances.getInstanceCount() - (m_treeCullCount - m_treeCullPieces),
        m_treeCullPieces);
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

Buffer& Renderer::treeCullList(uint32 frameIdx)
{
    return m_treeCullSet < (uint32)m_treeSets.size() && m_treeSets[m_treeCullSet].alive ? m_treeSets[m_treeCullSet].lists[frameIdx] : m_treeCullDummy;
}

uint32 Renderer::createTreeInstanceSet(oc::span<const TreeInstanceType> types, oc::span<const TreeInstancePiece> pieces,
    oc::span<const TreeInstanceChunk> chunks)
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
        TreeCullTypeGpu& gpu = gpuTypes[t];
        gpu = TreeCullTypeGpu{ record(type.bark), record(type.barkFade), record(type.leaves), record(type.leavesFade),
            record(type.billboard), record(type.trunk), record(type.trunkFade), record(type.cardsIn), record(type.cardsOut),
            type.farDistance, type.fadeWidth };
        // The mid tier only with its card mesh, its trunk and a billboard to hand over to (tree_cull.inc.glsl).
        const bool mid = type.midDistance > 0.0f && gpu.cardsIn.meshMaterial != TREE_RECORD_ABSENT
            && gpu.trunk.meshMaterial != TREE_RECORD_ABSENT && gpu.billboard.meshMaterial != TREE_RECORD_ABSENT;
        gpu.midDistance = mid ? type.midDistance : 0.0f;
        gpu.midFadeWidth = type.midFadeWidth;
        gpu.shadowDistance = type.shadowDistance;
    }

    // RT-CAPABLE types: their RT representation (tree_cull.inc.glsl treeCullRtPiece: the billboard, else the leaves)
    // has a BLAS. The others (bushes: created without one) would only ever take inactive TLAS slots.
    oc::vector<uint8> rtCapable(types.size(), 0);
    for (size_t t = 0; t < types.size(); ++t)
    {
        const TreeCullRecordGpu& rec = gpuTypes[t].billboard.meshMaterial != TREE_RECORD_ABSENT ? gpuTypes[t].billboard : gpuTypes[t].leaves;
        rtCapable[t] = rec.meshMaterial != TREE_RECORD_ABSENT && m_rt.hasStaticBlas(rec.meshMaterial & 0xFFFFu) ? 1 : 0;
    }

    // The chunks (no table = one chunk over every piece). Within each, the RT-capable pieces FIRST (a stable
    // partition - the caller does not address single pieces): renderTreeInstanceSet lists them ahead of every other
    // piece, and the TLAS takes slots for those only. Per chunk the sphere around them, for the CPU's RT range test.
    if (chunks.empty())
        set.chunks.assign(1, TreeInstanceChunk{ 0, (uint32)pieces.size() });
    else
        set.chunks.assign(chunks.begin(), chunks.end());
    set.chunkRt.clear();
    set.chunkRt.resize(set.chunks.size());
    oc::vector<uint32> order(pieces.size());
    for (uint32 i = 0; i < (uint32)order.size(); ++i)
        order[i] = i;
    for (size_t c = 0; c < set.chunks.size(); ++c)
    {
        const TreeInstanceChunk& chunk = set.chunks[c];
        assert(chunk.first + chunk.count <= pieces.size());
        uint32* begin = order.data() + chunk.first;
        uint32* mid = oc::stable_partition(begin, begin + chunk.count, [&](uint32 i) { return rtCapable[pieces[i].type] != 0; });
        TreeInstanceSet::ChunkRt& rt = set.chunkRt[c];
        rt.rtCount = (uint32)(mid - begin);
        glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
        for (const uint32* it = begin; it != mid; ++it)
        {
            lo = glm::min(lo, pieces[*it].centre - pieces[*it].radius);
            hi = glm::max(hi, pieces[*it].centre + pieces[*it].radius);
        }
        if (rt.rtCount > 0)
        {
            rt.rtCentre = (lo + hi) * 0.5f;
            rt.rtRadius = glm::length(hi - lo) * 0.5f;
        }
    }

    // No LOD hysteresis slots: tree meshes have no LOD chains (Procedural TreeSystem; the culls skip the lookup).
    oc::vector<TreeCullPieceGpu> gpuPieces(pieces.size());
    for (size_t i = 0; i < pieces.size(); ++i)
    {
        const TreeInstancePiece& piece = pieces[order[i]];
        const Transform& transform = piece.transform;
        gpuPieces[i] = TreeCullPieceGpu{
            .posScale = glm::vec4(transform.pos, transform.scale),
            .quat = glm::vec4(transform.quat.x, transform.quat.y, transform.quat.z, transform.quat.w),
            .centre = piece.centre,
            .radius = piece.radius,
            .type = piece.type,
        };
    }

    // Per chunk its bucket sizes: a piece draws each of its meshes at most once (normal OR fade material - the same
    // mesh). Level-0 meshes: present() sizes a chain's buckets from its level 0. (The order within a chunk does not
    // matter here.)
    set.meshCounts.clear();
    set.meshCountsBegin.clear();
    oc::unordered_map<uint16, uint32> meshCounts;
    auto count = [&](const TreeInstanceRep& a, const TreeInstanceRep& b)
    {
        const RenderMesh* mesh = a.mesh && a.mesh->isValid() ? a.mesh : (b.mesh && b.mesh->isValid() ? b.mesh : nullptr);
        if (mesh)
            ++meshCounts[mesh->m_meshIdx];
    };
    for (const TreeInstanceChunk& chunk : set.chunks)
    {
        meshCounts.clear();
        for (uint32 i = chunk.first; i < chunk.first + chunk.count; ++i)
        {
            const TreeInstanceType& type = types[pieces[i].type];
            count(type.bark, type.barkFade);
            count(type.leaves, type.leavesFade);
            count(type.billboard, type.billboard);
            if (gpuTypes[pieces[i].type].midDistance > 0.0f)
            {
                count(type.trunk, type.trunkFade);
                count(type.cardsIn, type.cardsOut);
            }
        }
        set.meshCountsBegin.push_back((uint32)set.meshCounts.size());
        set.meshCounts.insert(set.meshCounts.end(), meshCounts.begin(), meshCounts.end());
    }
    set.meshCountsBegin.push_back((uint32)set.meshCounts.size());

    // This frame's list (renderTreeInstanceSet): host-visible per frame slot, at most every piece.
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        set.lists[f].initialize(oc::max<size_t>(pieces.size() * sizeof(uint32), 16), vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "TreeCullList", BufferHostAccess::eSequentialWrite);
        set.mappedLists[f] = oc::span<uint32>((uint32*)set.lists[f].mapMemory().data(), pieces.size());
    }

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
        // The recorded culls bind its buffers: back to the dummies, and no tree this frame. A range claimed already
        // stays claimed (a claim cannot be returned) with NO trees in it: the culls' threads then skip it whole
        // (tree_cull.inc.glsl treeCullThreadInstance), instead of reading its never-written entries as instances.
        m_treeCullSet = UINT32_MAX;
        m_treeCullPieces = 0;
        m_treeCullRtPieces = 0;
        setHaveToRecordCommandBuffers();
    }
    set.pieces.destroy();
    set.types.destroy();
    set.volumePieces.destroy();
    set.volumeTypes.destroy();
    set.volumeData.destroy();
    set.hasVolume = false;
    m_treeVolume.markDirty();
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        set.lists[f].destroy();
        set.mappedLists[f] = {};
    }
    set.chunks.clear();
    set.chunkRt.clear();
    set.meshCounts.clear();
    set.meshCountsBegin.clear();
    set.numPieces = 0;
    set.alive = false;
}

void Renderer::bindTreeInstanceSet(uint32 setId)
{
    if (setId >= (uint32)m_treeSets.size() || !m_treeSets[setId].alive || m_treeCullSet == setId)
        return;
    m_treeCullSet = setId; // the recorded culls bind its buffers
    setHaveToRecordCommandBuffers();
}

void Renderer::renderTreeInstanceSet(uint32 setId, oc::span<const TreeChunkDraw> chunks, float distanceScale, bool forceFar)
{
    // The culls carry the BOUND set's records (bindTreeInstanceSet, main), one set per frame. The distance test runs
    // from the main view: the culls' camera.
    if (setId != m_treeCullSet || setId >= (uint32)m_treeSets.size())
        return;
    TreeInstanceSet& set = m_treeSets[setId];
    if (!set.alive || set.numPieces == 0)
        return;
    if (m_treeCullTaken.exchange(true, oc::memory_order_relaxed))
    {
        assert(false && "renderTreeInstanceSet: one call per frame");
        return;
    }

    // The LIST in two sections. First the RT section: the RT-capable pieces (first in their chunk) of the chunks drawn
    // for GI / shadows whose RT sphere reaches into "Trees/RT range" of the scene focus - the TLAS takes ONE slot per
    // entry of this section only (m_treeCullRtPieces; tree_cull.inc.glsl treeCullTlasInstance). Then every other
    // listed piece. The culls read the whole list, in any order.
    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();
    oc::span<uint32> list = set.mappedLists[frameIdx];
    const float rtRange = m_foliageParams.rtRange;
    const glm::vec3 focus = sceneFocusOrCamera();
    auto inRtSection = [&](const TreeChunkDraw& draw)
    {
        const TreeInstanceSet::ChunkRt& rt = set.chunkRt[draw.chunk];
        return rt.rtCount > 0 && (draw.passMask & (RendererVKLayout::PASS_GI | RendererVKLayout::PASS_SHADOW)) != 0
            && (rtRange <= 0.0f || glm::distance(rt.rtCentre, focus) - rt.rtRadius <= rtRange);
    };
    uint32 numListed = 0;
    for (const TreeChunkDraw& draw : chunks)
    {
        if (draw.chunk >= (uint32)set.chunks.size() || draw.passMask == 0 || !inRtSection(draw))
            continue;
        const TreeInstanceChunk& chunk = set.chunks[draw.chunk];
        const uint32 rtCount = set.chunkRt[draw.chunk].rtCount;
        assert(numListed + rtCount <= list.size() && "renderTreeInstanceSet: a chunk listed twice");
        const uint32 bits = draw.passMask << 28;
        for (uint32 i = 0; i < rtCount; ++i)
            list[numListed + i] = (chunk.first + i) | bits;
        numListed += rtCount;
    }
    const uint32 numRt = numListed;
    for (const TreeChunkDraw& draw : chunks)
    {
        if (draw.chunk >= (uint32)set.chunks.size() || draw.passMask == 0)
            continue;
        const TreeInstanceChunk& chunk = set.chunks[draw.chunk];
        const uint32 skip = inRtSection(draw) ? set.chunkRt[draw.chunk].rtCount : 0u; // listed above
        assert(numListed + chunk.count - skip <= list.size() && "renderTreeInstanceSet: a chunk listed twice");
        const uint32 bits = draw.passMask << 28;
        for (uint32 i = skip; i < chunk.count; ++i)
            list[numListed + i - skip] = (chunk.first + i) | bits;
        numListed += chunk.count - skip;
    }
    if (numListed == 0)
        return;

    const uint32 count = numListed * TREE_RECORDS_PER_PIECE;
    const uint32 base = m_instances.claimInstances(count);
    if (base == UINT32_MAX)
        return; // full this frame: the capacity grows at the next beginFrame
    for (const TreeChunkDraw& draw : chunks)
    {
        if (draw.chunk >= (uint32)set.chunks.size() || draw.passMask == 0)
            continue;
        for (uint32 m = set.meshCountsBegin[draw.chunk]; m < set.meshCountsBegin[draw.chunk + 1]; ++m)
            m_instances.noteMeshInstances(set.meshCounts[m].first, set.meshCounts[m].second);
    }
    set.lists[frameIdx].flushMappedMemory(numListed * sizeof(uint32));
    m_treeCullBase = base;
    m_treeCullCount = count;
    m_treeCullPieces = numListed;
    m_treeCullRtPieces = numRt;
    m_treeCullDistanceScale = distanceScale;
    m_treeCullForceFar = forceFar;
}
