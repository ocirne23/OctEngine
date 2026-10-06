module RendererVK;

import Core;
import Core.glm;
import Core.Transform;

import :Renderer;
import :Device;
import :Buffer;
import :Layout;
import :InstanceStream;
import :TextureManager; // the rock materials' textures (the far volume)

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

uint32 Renderer::allocTreeSet()
{
    uint32 setId = 0;
    while (setId < (uint32)m_treeSets.size() && m_treeSets[setId].alive)
        ++setId;
    if (setId == (uint32)m_treeSets.size())
        m_treeSets.emplace_back();
    m_treeSets[setId].alive = true;
    return setId;
}

uint32 Renderer::createTreeInstanceSet(oc::span<const TreeInstanceType> types, oc::span<const TreeInstancePiece> pieces,
    oc::span<const TreeInstanceChunk> chunks)
{
    const uint32 setId = allocTreeSet();
    TreeInstanceSet& set = m_treeSets[setId];
    initTreeSetTypes(set, types);
    initTreeSetPieces(set, (uint32)pieces.size());
    // The chunks (no table = one chunk over every piece): each written into its own range.
    const TreeInstanceChunk whole{ 0, (uint32)pieces.size() };
    const oc::span<const TreeInstanceChunk> ranges = chunks.empty() ? oc::span<const TreeInstanceChunk>(&whole, 1) : chunks;
    set.chunks.resize(ranges.size());
    for (size_t c = 0; c < ranges.size(); ++c)
    {
        assert(ranges[c].first + ranges[c].count <= pieces.size());
        writeTreeChunk(set, set.chunks[c], pieces.subspan(ranges[c].first, ranges[c].count), ranges[c].first);
    }
    set.numPieces = (uint32)pieces.size();
    m_treeVolume.markDirty();
    return setId;
}

uint32 Renderer::createDynamicTreeInstanceSet(oc::span<const TreeInstanceType> types, uint32 pieceCapacity)
{
    const uint32 setId = allocTreeSet();
    TreeInstanceSet& set = m_treeSets[setId];
    set.dynamic = true;
    set.numBlocks = (oc::max(pieceCapacity, 1u) + TREE_SET_BLOCK - 1) / TREE_SET_BLOCK;
    initTreeSetTypes(set, types);
    initTreeSetPieces(set, set.numBlocks * TREE_SET_BLOCK);
    set.numPieces = 0;
    m_treeVolume.markDirty();
    return setId;
}

uint32 Renderer::addTreeInstanceChunk(uint32 setId, oc::span<const TreeInstancePiece> pieces)
{
    if (setId >= (uint32)m_treeSets.size() || !m_treeSets[setId].alive || !m_treeSets[setId].dynamic)
        return UINT32_MAX;
    TreeInstanceSet& set = m_treeSets[setId];
    std::lock_guard<std::mutex> lock(m_treeSetMutex);
    // The removals that are due: their slots (every frame that could list them is done) and their indices (this
    // frame's walk was stamped without them).
    for (size_t i = 0; i < set.pendingFrees.size(); )
    {
        if (set.pendingFrees[i].readyFrame <= m_frameCounter)
        {
            set.freeBlocks.release(set.pendingFrees[i].firstBlock, set.pendingFrees[i].blocks);
            set.pendingFrees[i] = set.pendingFrees.back();
            set.pendingFrees.pop_back();
        }
        else
            ++i;
    }
    for (size_t i = 0; i < set.pendingChunks.size(); )
    {
        if (set.pendingChunks[i].second < m_frameCounter)
        {
            set.freeChunks.push_back(set.pendingChunks[i].first);
            set.pendingChunks[i] = set.pendingChunks.back();
            set.pendingChunks.pop_back();
        }
        else
            ++i;
    }

    const uint32 blocks = ((uint32)pieces.size() + TREE_SET_BLOCK - 1) / TREE_SET_BLOCK;
    uint32 firstBlock = UINT32_MAX;
    if (blocks > 0)
    {
        firstBlock = set.freeBlocks.allocate(blocks);
        if (firstBlock == UINT32_MAX)
        {
            if (set.topBlock + blocks > set.numBlocks)
                return UINT32_MAX;
            firstBlock = set.topBlock;
            set.topBlock += blocks;
        }
    }
    uint32 index;
    if (!set.freeChunks.empty())
    {
        index = set.freeChunks.back();
        set.freeChunks.pop_back();
    }
    else
    {
        index = (uint32)set.chunks.size();
        set.chunks.emplace_back();
    }
    TreeInstanceSet::Chunk& chunk = set.chunks[index];
    chunk = {};
    if (blocks > 0)
        writeTreeChunk(set, chunk, pieces, firstBlock * TREE_SET_BLOCK);
    set.numPieces = oc::max(set.numPieces, set.topBlock * TREE_SET_BLOCK);
    return index; // (no re-bake: the far volume never splats a dynamic set - recordFarTrees)
}

void Renderer::removeTreeInstanceChunk(uint32 setId, uint32 index)
{
    if (setId >= (uint32)m_treeSets.size() || !m_treeSets[setId].alive || !m_treeSets[setId].dynamic)
        return;
    TreeInstanceSet& set = m_treeSets[setId];
    std::lock_guard<std::mutex> lock(m_treeSetMutex);
    if (index >= (uint32)set.chunks.size())
        return;
    TreeInstanceSet::Chunk& chunk = set.chunks[index];
    if (chunk.count > 0)
    {
        const uint32 blocks = (chunk.count + TREE_SET_BLOCK - 1) / TREE_SET_BLOCK;
        set.pendingFrees.push_back({ chunk.first / TREE_SET_BLOCK, blocks, m_frameCounter + RendererVKLayout::NUM_FRAMES_IN_FLIGHT });
        // The far volume skips its slots from the next bake on (a bake in flight reads either - both harmless).
        for (uint32 i = 0; i < chunk.count; ++i)
            set.mappedVolumePieces[chunk.first + i].type = set.emptyVolumeType;
        set.volumePieces.flushMappedMemory(chunk.count * sizeof(TreeVolumePieceGpu), chunk.first * sizeof(TreeVolumePieceGpu));
    }
    chunk = {};
    set.pendingChunks.push_back({ index, m_frameCounter });
}

void Renderer::initTreeSetTypes(TreeInstanceSet& set, oc::span<const TreeInstanceType> types)
{
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

    // RT-CAPABLE types: their RT representation (tree_cull.inc.glsl treeCullRtPiece: the billboard, else the leaves,
    // else the bark - a rock) has a BLAS. The others (bushes: created without one) would only ever take inactive TLAS
    // slots.
    set.typeRtCapable.assign(types.size(), 0);
    set.typeMeshes.assign(types.size(), {});
    for (size_t t = 0; t < types.size(); ++t)
    {
        const TreeCullRecordGpu& rec = gpuTypes[t].billboard.meshMaterial != TREE_RECORD_ABSENT ? gpuTypes[t].billboard
            : gpuTypes[t].leaves.meshMaterial != TREE_RECORD_ABSENT ? gpuTypes[t].leaves : gpuTypes[t].bark;
        set.typeRtCapable[t] = rec.meshMaterial != TREE_RECORD_ABSENT && m_rt.hasStaticBlas(rec.meshMaterial & 0xFFFFu) ? 1 : 0;
        // The bucket sizes: a piece draws each of its meshes at most once (normal OR fade material - the same mesh).
        // Level-0 meshes: present() sizes a chain's buckets from its level 0.
        const TreeInstanceType& type = types[t];
        auto mesh = [&](const TreeInstanceRep& a, const TreeInstanceRep& b)
        {
            const RenderMesh* m = a.mesh && a.mesh->isValid() ? a.mesh : (b.mesh && b.mesh->isValid() ? b.mesh : nullptr);
            if (m)
                set.typeMeshes[t].push_back(m->m_meshIdx);
        };
        mesh(type.bark, type.barkFade);
        mesh(type.leaves, type.leavesFade);
        mesh(type.billboard, type.billboard);
        if (gpuTypes[t].midDistance > 0.0f)
        {
            mesh(type.trunk, type.trunkFade);
            mesh(type.cardsIn, type.cardsOut);
        }
    }

    set.types.initialize(oc::max<size_t>(gpuTypes.size() * sizeof(TreeCullTypeGpu), 16),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "TreeCullTypes");
    set.types.upload(gpuTypes.size() * sizeof(TreeCullTypeGpu), gpuTypes.data());

    // The far-tree volume's view (TreeVolumePipeline): per type its extinction mip chain (box filtered down to
    // 1^3). Types without density add nothing to the volume. One more type at the end: the SENTINEL (res 0) a free
    // piece slot of a dynamic set points at. Host-visible, read through device addresses at its (rare) rebakes.
    oc::vector<TreeVolumeTypeGpu> volumeTypes(types.size() + 1);
    set.emptyVolumeType = (uint32)types.size();
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
        gpu.albedo = glm::vec4(type.albedo, type.solid ? 0.0f : 1.0f); // w: 1 foliage, 0 a solid (tree_volume_splat.cs)
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
    upload(set.volumeTypes, volumeTypes.data(), volumeTypes.size() * sizeof(TreeVolumeTypeGpu), "TreeVolumeTypes");
    upload(set.volumeData, volumeData.data(), volumeData.size() * sizeof(float), "TreeVolumeData");
}

// The piece slots: the culls' pieces (device-local, written per chunk through the staging ring), the volume's pieces
// (host-visible, every slot on the sentinel type until a chunk writes it) and the per-frame lists (at most every slot).
void Renderer::initTreeSetPieces(TreeInstanceSet& set, uint32 capacity)
{
    set.pieces.initialize(oc::max<size_t>((size_t)capacity * sizeof(TreeCullPieceGpu), 16),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "TreeCullPieces");
    set.volumePieces.initialize(oc::max<size_t>((size_t)capacity * sizeof(TreeVolumePieceGpu), 16),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
        vk::MemoryPropertyFlagBits::eHostVisible, false, "TreeVolumePieces", BufferHostAccess::eSequentialWrite);
    set.mappedVolumePieces = oc::span<TreeVolumePieceGpu>((TreeVolumePieceGpu*)set.volumePieces.mapMemory().data(), capacity);
    TreeVolumePieceGpu empty;
    empty.type = set.emptyVolumeType;
    for (TreeVolumePieceGpu& piece : set.mappedVolumePieces)
        piece = empty;
    if (capacity > 0)
        set.volumePieces.flushMappedMemory((size_t)capacity * sizeof(TreeVolumePieceGpu));
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        set.lists[f].initialize(oc::max<size_t>((size_t)capacity * sizeof(uint32), 16), vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "TreeCullList", BufferHostAccess::eSequentialWrite);
        set.mappedLists[f] = oc::span<uint32>((uint32*)set.lists[f].mapMemory().data(), capacity);
    }
}

// Within the chunk the RT-capable pieces FIRST (a stable partition - the caller does not address single pieces):
// renderTreeInstanceSet lists them ahead of every other piece, and the TLAS takes slots for those only. The sphere
// around them is the CPU's RT range test.
void Renderer::writeTreeChunk(TreeInstanceSet& set, TreeInstanceSet::Chunk& chunk, oc::span<const TreeInstancePiece> pieces, uint32 first)
{
    oc::vector<uint32> order(pieces.size());
    for (uint32 i = 0; i < (uint32)order.size(); ++i)
        order[i] = i;
    const auto mid = oc::stable_partition(order.begin(), order.end(), [&](uint32 i) { return set.typeRtCapable[pieces[i].type] != 0; });
    chunk.first = first;
    chunk.count = (uint32)pieces.size();
    chunk.rtCount = (uint32)(mid - order.begin());
    glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (auto it = order.begin(); it != mid; ++it)
    {
        lo = glm::min(lo, pieces[*it].centre - pieces[*it].radius);
        hi = glm::max(hi, pieces[*it].centre + pieces[*it].radius);
    }
    if (chunk.rtCount > 0)
    {
        chunk.rtCentre = (lo + hi) * 0.5f;
        chunk.rtRadius = glm::length(hi - lo) * 0.5f;
    }

    // No LOD hysteresis slots: tree meshes have no LOD chains (Procedural TreeSystem; the culls skip the lookup).
    oc::vector<TreeCullPieceGpu> gpuPieces(pieces.size());
    oc::unordered_map<uint16, uint32> meshCounts;
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
        TreeVolumePieceGpu& volume = set.mappedVolumePieces[first + i];
        volume.posScale = gpuPieces[i].posScale;
        volume.quat = gpuPieces[i].quat;
        volume.type = piece.type;
        for (uint16 mesh : set.typeMeshes[piece.type])
            ++meshCounts[mesh];
    }
    chunk.meshCounts.assign(meshCounts.begin(), meshCounts.end());
    constexpr size_t CHUNK = 16 * 1024 * 1024;
    const size_t bytes = gpuPieces.size() * sizeof(TreeCullPieceGpu);
    for (size_t offset = 0; offset < bytes; offset += CHUNK)
        set.pieces.upload(oc::min(CHUNK, bytes - offset), (const uint8*)gpuPieces.data() + offset, (size_t)first * sizeof(TreeCullPieceGpu) + offset);
    if (!pieces.empty())
        set.volumePieces.flushMappedMemory(pieces.size() * sizeof(TreeVolumePieceGpu), (size_t)first * sizeof(TreeVolumePieceGpu));
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
    if (m_treeRecords.numTypes() > 0)
        return true; // the world tree records (W4)
    for (const TreeInstanceSet& set : m_treeSets)
        if (set.alive && set.hasVolume)
            return true;
    return false;
}

void Renderer::recordFarTrees(uint32 frameIdx, vk::CommandBuffer primary)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    // The record chunks changed (they stream in and out while the camera moves): re-bake at most every 30 frames.
    if (m_treeSetsChanged && m_frameCounter - m_treeVolumeMarkFrame >= 30)
    {
        m_treeSetsChanged = false;
        m_treeVolumeMarkFrame = m_frameCounter;
        m_treeVolume.markDirty();
    }
    // The static sets (the preview grove). A DYNAMIC set (the world) never: its trees come from their records, which
    // expand to the same trees (W4) - one source, so a chunk entering / leaving the set never changes the volume.
    oc::small_vector<TreeVolumePipeline::Source, 4> sources;
    for (const TreeInstanceSet& set : m_treeSets)
        if (set.alive && set.hasVolume && !set.dynamic)
            sources.push_back(TreeVolumePipeline::Source{ set.volumePieces.getDeviceAddress(), set.volumeTypes.getDeviceAddress(),
                set.volumeData.getDeviceAddress(), set.numPieces });
    // The world tree records (W4): this frame slot's chunk table, expanded into the world set's volume types.
    TreeVolumePipeline::RecordSource records;
    if (m_treeRecords.numTypes() > 0 && m_treeRecords.tableCount(frameIdx) > 0 && m_treeRecordSet < (uint32)m_treeSets.size()
        && m_treeSets[m_treeRecordSet].alive)
    {
        const TreeInstanceSet& set = m_treeSets[m_treeRecordSet];
        records = TreeVolumePipeline::RecordSource{
            .records = m_treeRecords.records().getDeviceAddress(),
            .map = m_treeRecords.map(frameIdx).getDeviceAddress(),
            .mapSize = m_treeRecords.mapSize(),
            .chunks = m_treeRecords.table(frameIdx).getDeviceAddress(),
            .chunksCpu = m_treeRecords.tableCpu(frameIdx),
            .mapCpu = m_treeRecords.mapCpu(),
            .types = m_treeRecords.types().getDeviceAddress(),
            .volumeTypes = set.volumeTypes.getDeviceAddress(),
            .volumeData = set.volumeData.getDeviceAddress(),
            .numChunks = m_treeRecords.tableCount(frameIdx),
            .numTypes = m_treeRecords.numTypes(),
            .chunkSize = m_treeRecordChunkSize,
            .worldSeed = m_treeRecordSeed,
        };
    }
    // ROCKS (R5): the terrain's rock materials' diffuse textures (slot order, after the ground ones), the volume's climate
    // bedrock colour per column; the fallback diffuse past their count.
    oc::array<vk::ImageView, TreeVolumePipeline::ROCK_TEXTURES> rockTextures;
    rockTextures.fill(Globals::textureManager.getViewForDescriptor(RendererVKLayout::FALLBACK_DIFFUSE_TEX_IDX));
    if (m_terrain.getSplatBaseMaterial() >= 0)
    {
        const TerrainSplatCounts& counts = m_terrain.getSplatCounts();
        for (uint32 i = 0; i < oc::min(counts.numRock, TreeVolumePipeline::ROCK_TEXTURES); ++i)
            rockTextures[i] = Globals::textureManager.getViewForDescriptor((uint16)(m_terrain.getSplatTex()[counts.numGround + i].x & 0xFFFFu));
    }
    const TreeVolumePipeline::RecordParams params{
        .ubo = frameData.ubo,
        .rockTextures = rockTextures,
        .sceneDepthView = frameData.sceneColor.getDepthView(0),
        .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
        .terrainView = m_terrain.getHeightMap().getView(),
        .terrainSampler = m_terrain.getHeightMap().getSampler(),
        .skyMapView = m_giProbePipeline.getSkyMapView(),
        .skyMapSampler = m_giProbePipeline.getSkyMapSampler(),
        .cloudShadowView = m_cloudPipeline.getShadowView(),
        .cloudShadowSampler = m_cloudPipeline.getShadowSampler(),
        .cameraPos = m_cameraPos,
        .startDistance = farTreesStart(),
        .sources = oc::span<const TreeVolumePipeline::Source>(sources.data(), sources.size()),
        .records = records,
        .settings = m_farTreeParams,
        .frameNumber = m_frameCounter,
    };
    m_gpuProfiler.beginScope(primary, "Far tree bake");
    m_treeVolume.recordBake(primary, frameIdx, params);
    m_gpuProfiler.endScope(primary);
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
    m_treeVolume.invalidate(); // a running bake reads its buffers
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        set.lists[f].destroy();
        set.mappedLists[f] = {};
    }
    set.mappedVolumePieces = {};
    set.chunks.clear();
    set.typeRtCapable.clear();
    set.typeMeshes.clear();
    set.dynamic = false;
    set.numBlocks = 0;
    set.topBlock = 0;
    set.freeBlocks = {};
    set.pendingFrees.clear();
    set.freeChunks.clear();
    set.pendingChunks.clear();
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
    std::lock_guard<std::mutex> lock(m_treeSetMutex); // a dynamic set's chunks change on main meanwhile

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
        const TreeInstanceSet::Chunk& rt = set.chunks[draw.chunk];
        return rt.rtCount > 0 && (draw.passMask & (RendererVKLayout::PASS_GI | RendererVKLayout::PASS_SHADOW)) != 0
            && (rtRange <= 0.0f || glm::distance(rt.rtCentre, focus) - rt.rtRadius <= rtRange);
    };
    uint32 numListed = 0;
    for (const TreeChunkDraw& draw : chunks)
    {
        if (draw.chunk >= (uint32)set.chunks.size() || draw.passMask == 0 || !inRtSection(draw))
            continue;
        const TreeInstanceSet::Chunk& chunk = set.chunks[draw.chunk];
        const uint32 rtCount = chunk.rtCount;
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
        const TreeInstanceSet::Chunk& chunk = set.chunks[draw.chunk];
        const uint32 skip = inRtSection(draw) ? chunk.rtCount : 0u; // listed above
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
        for (const auto& [mesh, instances] : set.chunks[draw.chunk].meshCounts)
            m_instances.noteMeshInstances(mesh, instances);
    }
    set.lists[frameIdx].flushMappedMemory(numListed * sizeof(uint32));
    m_treeCullBase = base;
    m_treeCullCount = count;
    m_treeCullPieces = numListed;
    m_treeCullRtPieces = numRt;
    m_treeCullDistanceScale = distanceScale;
    m_treeCullForceFar = forceFar;
}
