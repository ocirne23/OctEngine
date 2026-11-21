module RendererVK;

import Core;
import Core.glm;
import Core.Transform;
import Core.Sphere;

import :Renderer;
import :Device;
import :Buffer;
import :Layout;
import :InstanceStream;
import :TextureManager; // the rock materials' textures (the far volume)
import Settings;

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
    constexpr uint32 TREE_RECORDS_PER_PIECE = 3; // at most 3 representations draw at once: bark, leaves, voxels (tree_cull.inc.glsl)

    // One representation: an InMeshInstance minus its node (meshIdx | materialIdx << 16, pipelineIdx | alphaMode << 16).
    struct TreeCullRecordGpu
    {
        uint32 meshMaterial = TREE_RECORD_ABSENT;
        uint32 pipelineAlpha = 0;
    };

    // A piece TYPE (one baked variant of one species, or a rock variant): its representations, its crossfade band and its
    // voxel grid. Four 32-byte sectors (tree_type.inc.glsl): the culls read the first two, the LitTree and voxel-tier
    // shaders the last two.
    struct TreeCullTypeGpu
    {
        TreeCullRecordGpu bark;
        TreeCullRecordGpu barkFade;
        TreeCullRecordGpu leaves;
        TreeCullRecordGpu leavesFade;
        TreeCullRecordGpu voxel;           // the voxel tier: the main pass past the band (absent = the meshes at every distance)
        TreeCullRecordGpu shadowBillboard; // the shadow passes and the TLAS only (absent = the meshes cast)
        float farDistance = 0.0f; // mesh -> voxels switch distance (m); x the frame's distance scale
        float fadeWidth = 1.0f;   // crossfade band (m), centred on it
        float shadowDistance = 0.0f; // no sun shadow beyond this from the cascades' centre (m); 0 = no limit
        uint32 wind = 0;             // x the trunk bend | x the branch sway << 16, each 4.12 fixed point (tree_wind.inc.glsl)
        // The type's voxel grid (RenderTreeSet::Type's voxelGrid*): its box in the tree's own space, its textures, its header.
        glm::vec3 gridInvSize{ 0.0f };
        float gridCbrtVolume = 0.0f;   // the cube root of the box's volume (m); 0 = NO GRID
        uint32 gridTextures = 0;       // the opacity atlas | the index texture << 16
        uint32 gridHeader = 0;         // log2(res) | levels << 8 | log2(cols) << 16
        uint32 pad0 = 0, pad1 = 0;
        glm::vec3 gridMin{ 0.0f };
        float pad2 = 0.0f;
        glm::vec3 gridSize{ 0.0f };
        float pad3 = 0.0f;
    };
    static_assert(sizeof(TreeCullTypeGpu) == 128);

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

// The baked tree records' UBO words (u_present_tree*, tree_cull.inc.glsl): the claimed range, the band scale, forceFar,
// and the far-tree volume's start. Evaluated in present(): the UBO uploads in beginFrame, BEFORE RenderTreeSet::render
// claims the range - left at that upload's values, the culls read the range's never-written stream entries as
// instances (garbage mesh indices, out-of-bounds bucket writes).
// The culls' THREAD count (their dispatch, IndirectCullComputePipeline::update): one per stream instance outside the
// range, one per PIECE inside it - tree_cull.inc.glsl's treeCullThreadInstance.
uint32 RendererState::treeCullThreads() const
{
    return m_instances.getInstanceCount() - (m_treeCullCount - m_treeCullPieces);
}

// The TLAS-instance writer's live count (u_present_giTlasNumInstances): present()'s demand, clamped to the capacity.
uint32 RendererState::giTlasLiveCount() const
{
    return oc::min(m_giTlasDemand, m_rt.getMaxTlasInstances());
}

// 0 = no volume; the voxel trees draw through its fade-in band - an overlap, not a seam.
float RendererState::treeCullVolumeStart() const
{
    const bool treeVolume = m_treeCullPieces > 0 && m_treeCullSet < (uint32)m_treeSets.size() && farTreesActive()
        && m_treeSets[m_treeCullSet].hasVolume;
    return treeVolume ? oc::max(farTreesStart() + m_farTreeParams.overlap, 1.0f) : 0.0f;
}

Buffer& RendererState::treeCullPieces()
{
    return m_treeCullSet < (uint32)m_treeSets.size() && m_treeSets[m_treeCullSet].alive ? m_treeSets[m_treeCullSet].pieces : m_treeCullDummy;
}

Buffer& RendererState::treeCullTypes()
{
    return m_treeCullSet < (uint32)m_treeSets.size() && m_treeSets[m_treeCullSet].alive ? m_treeSets[m_treeCullSet].types : m_treeCullDummy;
}

Buffer& RendererState::treeCullList(uint32 frameIdx)
{
    return m_treeCullSet < (uint32)m_treeSets.size() && m_treeSets[m_treeCullSet].alive ? m_treeSets[m_treeCullSet].lists[frameIdx] : m_treeCullDummy;
}

uint32 RendererState::allocTreeSet()
{
    uint32 setId = 0;
    while (setId < (uint32)m_treeSets.size() && m_treeSets[setId].alive)
        ++setId;
    if (setId == (uint32)m_treeSets.size())
        m_treeSets.emplace_back();
    m_treeSets[setId].alive = true;
    return setId;
}

RenderTreeSet RenderTreeSet::create(oc::span<const Type> types, uint32 pieceCapacity)
{
    RenderTreeSet set;
    set.m_setId = Globals::rendererVK.createTreeSet(types, pieceCapacity);
    return set;
}

void RenderTreeSet::destroy()
{
    if (m_setId == UINT32_MAX)
        return;
    Globals::rendererVK.destroyTreeSet(m_setId);
    m_setId = UINT32_MAX;
}

uint32 RenderTreeSet::addChunk(oc::span<const Piece> pieces)
{
    return Globals::rendererVK.addTreeSetChunk(m_setId, pieces);
}

void RenderTreeSet::removeChunk(uint32 chunk)
{
    Globals::rendererVK.removeTreeSetChunk(m_setId, chunk);
}

void RenderTreeSet::bind() const
{
    Globals::rendererVK.bindTreeSet(m_setId);
}

void RenderTreeSet::render(oc::span<const ChunkDraw> chunks, float distanceScale, bool forceFar) const
{
    Globals::rendererVK.renderTreeSet(m_setId, chunks, distanceScale, forceFar);
}

uint32 RendererState::createTreeSet(oc::span<const TreeInstanceType> types, uint32 pieceCapacity)
{
    const uint32 setId = allocTreeSet();
    TreeInstanceSet& set = m_treeSets[setId];
    set.numBlocks = (oc::max(pieceCapacity, 1u) + TREE_SET_BLOCK - 1) / TREE_SET_BLOCK;
    initTreeSetTypes(set, types);
    initTreeSetPieces(set, set.numBlocks * TREE_SET_BLOCK);
    m_treeVolume.markDirty(); // the records expand into this set's volume types
    return setId;
}

uint32 RendererState::addTreeSetChunk(uint32 setId, oc::span<const TreeInstancePiece> pieces)
{
    if (setId >= (uint32)m_treeSets.size() || !m_treeSets[setId].alive)
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
    return index; // (no re-bake: the far volume reads the records, never a set's pieces - recordFarTrees)
}

void RendererState::removeTreeSetChunk(uint32 setId, uint32 index)
{
    if (setId >= (uint32)m_treeSets.size() || !m_treeSets[setId].alive)
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
    }
    chunk = {};
    set.pendingChunks.push_back({ index, m_frameCounter });
}

void RendererState::initTreeSetTypes(TreeInstanceSet& set, oc::span<const TreeInstanceType> types)
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
            record(type.voxel), record(type.shadowBillboard), type.farDistance, type.fadeWidth };
        // The voxel tier needs its band (tree_cull.inc.glsl treeCullMainPiece).
        if (type.farDistance <= 0.0f)
            gpu.voxel = TreeCullRecordGpu{};
        gpu.shadowDistance = type.shadowDistance;
        auto fixed = [](float v) { return (uint32)std::round(glm::clamp(v, 0.0f, 15.99f) * 4096.0f); };
        gpu.wind = fixed(type.windTrunk) | (fixed(type.windBranch) << 16);
        const bool hasGrid = type.voxelGridSize.x > 0.0f && type.voxelGridSize.y > 0.0f && type.voxelGridSize.z > 0.0f;
        if (hasGrid)
        {
            gpu.gridMin = type.voxelGridMin;
            gpu.gridSize = type.voxelGridSize;
            gpu.gridInvSize = 1.0f / type.voxelGridSize;
            gpu.gridCbrtVolume = std::cbrt(type.voxelGridSize.x * type.voxelGridSize.y * type.voxelGridSize.z);
            gpu.gridTextures = (uint32)type.voxelGridOpacityTexture | ((uint32)type.voxelGridIndexTexture << 16);
            gpu.gridHeader = (uint32)type.voxelGridLog2Res | ((uint32)type.voxelGridLevels << 8) | ((uint32)type.voxelGridLog2Cols << 16);
        }
        // The voxel tier's fragment shader takes its box from the type: no voxel record without it.
        assert(hasGrid || gpu.voxel.meshMaterial == TREE_RECORD_ABSENT);
        if (!hasGrid)
            gpu.voxel = TreeCullRecordGpu{};
    }
    // A tree's record carries its type in 16 bits (the main cull, instanced_indirect.cs.glsl).
    assert(types.size() <= 0x10000);

    // RT-CAPABLE types: their RT representation (tree_cull.inc.glsl treeCullRtPiece: the shadow billboard, else the
    // leaves, else the bark - a rock) has a BLAS. The others (bushes: created without one) would only ever take inactive
    // TLAS slots.
    set.typeRtCapable.assign(types.size(), 0);
    set.typeMeshes.assign(types.size(), {});
    for (size_t t = 0; t < types.size(); ++t)
    {
        const TreeCullRecordGpu& rec = gpuTypes[t].shadowBillboard.meshMaterial != TREE_RECORD_ABSENT ? gpuTypes[t].shadowBillboard
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
        mesh(type.shadowBillboard, type.shadowBillboard);
        if (gpuTypes[t].voxel.meshMaterial != TREE_RECORD_ABSENT)
            mesh(type.voxel, type.voxel);
    }

    // THE FAR VOXEL TREES' view: per type of the far ring (`farRing`: the trees, not the bushes) its voxel bucket (one per
    // distinct voxel mesh), material, trunk-bend scale and box (centre height, half diagonal at scale 1); the buckets' draw
    // ranges from the meshes' level 0.
    // resize, not assign(n, {}): that left `bucket` 0 (not the member default, "none") on every type without a voxel tier,
    // and the bushes and rocks drew as bucket 0's box with material 0.
    set.farTypes.clear();
    set.farTypes.resize(types.size());
    set.farBuckets.clear();
    set.farShadowBuckets.clear();
    for (size_t t = 0; t < types.size(); ++t)
    {
        const TreeInstanceRep& voxel = types[t].voxel;
        if (!types[t].farRing || gpuTypes[t].voxel.meshMaterial == TREE_RECORD_ABSENT || !voxel.mesh)
            continue;
        const RenderMesh* farMesh = voxel.mesh;
        const uint16 meshIdx = farMesh->m_meshIdx;
        uint32 bucket = UINT32_MAX;
        for (uint32 b = 0; b < (uint32)set.farBuckets.size(); ++b)
            if (set.farBuckets[b].pad == meshIdx)
                bucket = b;
        if (bucket == UINT32_MAX)
        {
            if (set.farBuckets.size() >= TreeVoxelFarPipeline::MAX_BUCKETS)
                continue;
            const RendererVKLayout::MeshInfo& info = m_meshInfos.items()[meshIdx];
            bucket = (uint32)set.farBuckets.size();
            // 6 indices: the box mesh's first quad, which the vertex shader places on the screen rectangle around the box.
            set.farBuckets.push_back({ 6u, info.firstIndex, info.vertexOffset, meshIdx }); // pad: the mesh (the dedupe)
        }
        TreeVoxelFarPipeline::FarTypeGpu& far = set.farTypes[t];
        far.bucket = bucket;
        far.materialMesh = (gpuTypes[t].voxel.meshMaterial & 0xFFFF0000u) | meshIdx; // the voxel tier's material on the far mesh
        far.wind = types[t].windTrunk;
        far.centreY = farMesh->getBounds().pos.y;
        far.radius = farMesh->getBounds().radius;

        // THE FAR SHADOW CASTERS: the same trees as their SHADOW BILLBOARD (its own buckets) with its mask.
        const TreeInstanceRep& billboard = types[t].shadowBillboard;
        if (!billboard.mesh || !billboard.mesh->isValid())
            continue;
        const uint16 billboardIdx = billboard.mesh->m_meshIdx;
        uint32 shadowBucket = UINT32_MAX;
        for (uint32 b = 0; b < (uint32)set.farShadowBuckets.size(); ++b)
            if (set.farShadowBuckets[b].pad == billboardIdx)
                shadowBucket = b;
        if (shadowBucket == UINT32_MAX)
        {
            if (set.farShadowBuckets.size() >= TreeVoxelFarPipeline::MAX_BUCKETS)
                continue;
            const RendererVKLayout::MeshInfo& info = m_meshInfos.items()[billboardIdx];
            shadowBucket = (uint32)set.farShadowBuckets.size();
            set.farShadowBuckets.push_back({ info.indexCount, info.firstIndex, info.vertexOffset, billboardIdx });
        }
        far.shadowBucket = shadowBucket;
        far.shadowTex = m_materials.items()[billboard.material].diffuseTexIdx;
    }

    set.types.initialize(oc::max<size_t>(gpuTypes.size() * sizeof(TreeCullTypeGpu), 16),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "TreeCullTypes");
    set.types.upload(gpuTypes.size() * sizeof(TreeCullTypeGpu), gpuTypes.data());

    // The far-tree volume's view (TreeVolumePipeline): per type its extinction mip chain (box filtered down to
    // 1^3). Types without density add nothing to the volume. Host-visible, read through device addresses at its (rare)
    // rebakes.
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
        // w (tree_volume_splat.cs): 1 foliage, 0 a solid in the climate's bedrock colour, 0.5 a solid in its own (dead wood).
        gpu.albedo = glm::vec4(type.albedo, !type.solid ? 1.0f : type.solidOwnColour ? 0.5f : 0.0f);
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

// The piece slots: the culls' pieces (device-local, written per chunk through the staging ring) and the per-frame lists
// (at most every slot).
void RendererState::initTreeSetPieces(TreeInstanceSet& set, uint32 capacity)
{
    set.pieces.initialize(oc::max<size_t>((size_t)capacity * sizeof(TreeCullPieceGpu), 16),
        vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst, vk::MemoryPropertyFlagBits::eDeviceLocal, false, "TreeCullPieces");
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        set.lists[f].initialize(oc::max<size_t>((size_t)capacity * sizeof(uint32), 16), vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "TreeCullList", BufferHostAccess::eSequentialWrite);
        set.mappedLists[f] = oc::span<uint32>((uint32*)set.lists[f].mapMemory().data(), capacity);
    }
}

// Within the chunk the RT-capable pieces FIRST (a stable partition - the caller does not address single pieces):
// renderTreeSet lists them ahead of every other piece, and the TLAS takes slots for those only. The sphere
// around them is the CPU's RT range test.
void RendererState::writeTreeChunk(TreeInstanceSet& set, TreeInstanceSet::Chunk& chunk, oc::span<const TreeInstancePiece> pieces, uint32 first)
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
        for (uint16 mesh : set.typeMeshes[piece.type])
            ++meshCounts[mesh];
    }
    chunk.meshCounts.assign(meshCounts.begin(), meshCounts.end());
    constexpr size_t CHUNK = 16 * 1024 * 1024;
    const size_t bytes = gpuPieces.size() * sizeof(TreeCullPieceGpu);
    for (size_t offset = 0; offset < bytes; offset += CHUNK)
        set.pieces.upload(oc::min(CHUNK, bytes - offset), (const uint8*)gpuPieces.data() + offset, (size_t)first * sizeof(TreeCullPieceGpu) + offset);
}

// "Far start" scaled with the camera's height h above the ground under it: sqrt(start^2 + h^2) is the 3D distance
// of a ground tree at HORIZONTAL distance start. The voxel trees hand over (tree_cull) and the march starts there,
// so from the air the hand-over stays at horizontal "Far start" - where the volume's fixed ring begins - instead of
// moving in under the camera (the trees vanished below it with nothing in their place).
float RendererState::farTreesStart() const
{
    const float ground = std::isnan(m_cameraGround) ? m_terrain.getHeightMap().getUserParam() : m_cameraGround;
    const float h = oc::max(m_cameraPos.y - ground, 0.0f);
    return std::sqrt(m_farTreeParams.startDistance * m_farTreeParams.startDistance + h * h);
}

bool RendererState::treeVoxelFarActive() const
{
    return Globals::settings.foliage.voxelFarTrees && m_sceneViewCount == 1 && m_treeRecords.numTypes() > 0
        && m_treeRecordSet < (uint32)m_treeSets.size() && m_treeSets[m_treeRecordSet].alive && !m_treeSets[m_treeRecordSet].farBuckets.empty();
}

// The far volume's hand-over (where the near set's trees leave the main pass too: treeCullVolumeStart), or without the
// volume "Trees/Voxel far end".
float RendererState::treeVoxelFarEnd() const
{
    return farTreesActive() ? farTreesStart() + m_farTreeParams.overlap : Globals::settings.foliage.voxelFarEnd;
}

// present: the slot's chunk list - every resident record chunk that is not the near set's and reaches within the end
// distance (horizontally, from the camera) - and the world set's far tables.
void RendererState::updateTreeVoxelFar(uint32 frameIdx)
{
    m_treeVoxelFarListed = false;
    m_treeVoxelFarShadowListed = false;
    if (!treeVoxelFarActive())
        return;
    const TreeInstanceSet& set = m_treeSets[m_treeRecordSet];
    const float end = treeVoxelFarEnd();
    const float chunkSize = m_treeRecordChunkSize;
    const glm::vec2 eye(m_cameraPos.x, m_cameraPos.z);
    // THE FAR SHADOW CASTERS: the same chunks within the cascades' reach (the shadow distance + "Foliage shadow cascade
    // margin", from the scene focus; the GPU keeps a tree per cascade split), with the sun cascades on.
    const bool shadows = !m_rtParams.effectiveSunShadow() && !set.farShadowBuckets.empty();
    const glm::vec3 focus3 = self().sceneFocusOrCamera();
    const glm::vec2 focus(focus3.x, focus3.z);
    const float shadowEnd = m_shadowParams.maxDistance + oc::max(Globals::settings.foliage.shadowCascadeMargin, 0.0f)
        + Globals::settings.farTree.height;
    oc::vector<TreeVoxelFarPipeline::ChunkGpu> shadowChunks;
    // NEAREST FIRST: the workgroups append in about their order, so when the trees outnumber MAX_INSTANCES it is the
    // farthest that are dropped (in the table's order the overflow fell anywhere past ~9 km, different trees every frame).
    oc::vector<oc::pair<float, TreeVoxelFarPipeline::ChunkGpu>> listed;
    for (const TreeRecordChunkGpu& chunk : m_treeRecords.tableCpu(frameIdx))
    {
        if (chunk.count == 0)
            continue;
        if (m_treeNearChunks.find((uint64)(uint32)chunk.coord.x | ((uint64)(uint32)chunk.coord.y << 32)) != m_treeNearChunks.end())
            continue;
        const glm::vec2 lo = glm::vec2(chunk.coord) * chunkSize;
        if (shadows && shadowChunks.size() < TreeVoxelFarPipeline::MAX_CHUNKS
            && glm::length(glm::clamp(focus, lo, lo + chunkSize) - focus) <= shadowEnd)
            shadowChunks.push_back({ chunk.coord, chunk.first, chunk.count });
        const float d = glm::length(glm::clamp(eye, lo, lo + chunkSize) - eye);
        if (d > end)
            continue;
        listed.push_back({ d, TreeVoxelFarPipeline::ChunkGpu{ chunk.coord, chunk.first, chunk.count } });
    }
    oc::sort(listed.begin(), listed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    oc::vector<TreeVoxelFarPipeline::ChunkGpu> chunks;
    chunks.reserve(oc::min<size_t>(listed.size(), TreeVoxelFarPipeline::MAX_CHUNKS));
    for (size_t i = 0; i < listed.size() && i < TreeVoxelFarPipeline::MAX_CHUNKS; ++i)
        chunks.push_back(listed[i].second);
    m_treeVoxelFar.setFrame(frameIdx, chunks, set.farTypes, set.farBuckets, shadowChunks, set.farShadowBuckets);
    m_treeVoxelFarListed = true;
    m_treeVoxelFarShadowListed = !shadowChunks.empty();
}

bool RendererState::farTreesActive() const
{
    // The volume's only source: the world tree records (W4).
    return m_farTreeParams.enabled && m_sceneViewCount == 1 && m_treeRecords.numTypes() > 0;
}

void RendererState::recordFarTrees(uint32 frameIdx, vk::CommandBuffer primary, const oc::function<void(const TreeVolumePipeline::RecordParams*)>& march)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    // The record chunks changed (they stream in and out while the camera moves): re-bake at most every 30 frames.
    if (m_treeSetsChanged && m_frameCounter - m_treeVolumeMarkFrame >= 30)
    {
        m_treeSetsChanged = false;
        m_treeVolumeMarkFrame = m_frameCounter;
        m_treeVolume.markDirty();
    }
    // The world tree records (W4): this frame slot's chunk table, expanded into the world set's volume types. The set's
    // own pieces are never a source: the records expand to the same trees - one source, so a chunk entering / leaving the
    // set never changes the volume.
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
    uint32 numRock = 0;
    if (m_terrain.getSplatBaseMaterial() >= 0)
    {
        const TerrainSplatCounts& counts = m_terrain.getSplatCounts();
        numRock = oc::min(counts.numRock, TreeVolumePipeline::ROCK_TEXTURES);
        for (uint32 i = 0; i < numRock; ++i)
            rockTextures[i] = Globals::textureManager.getViewForDescriptor((uint16)(m_terrain.getSplatTex()[counts.numGround + i].x & 0xFFFFu));
    }
    const TreeVolumePipeline::RecordParams params{
        .ubo = frameData.ubo,
        .rockTextures = rockTextures,
        .numRock = numRock,
        .sceneDepthView = frameData.sceneColor.getDepthView(0),
        .sceneDepthSampler = frameData.sceneColor.getDepthSampler(),
        .terrainView = m_terrain.getHeightMap().getView(),
        .terrainSampler = m_terrain.getHeightMap().getSampler(),
        .skyMapView = m_giProbePipeline.getSkyMapView(),
        .skyMapSampler = m_giProbePipeline.getSkyMapSampler(),
        .cloudShadowView = m_cloudPipeline.getShadowView(),
        .cloudShadowSampler = m_cloudPipeline.getShadowSampler(),
        .factionView = m_terrain.getFactionMap().getView(),
        .factionSampler = m_terrain.getFactionMap().getSampler(),
        .cameraPos = m_cameraPos,
        .startDistance = farTreesStart(),
        .records = records,
        .settings = m_farTreeParams,
        .frameNumber = m_frameCounter,
    };
    // Only while a bake runs: never an empty scope (TreeVolumePipeline::updateBake).
    if (m_treeVolume.updateBake(params))
    {
        m_gpuProfiler.beginScope(primary, "Far tree bake");
        m_treeVolume.recordBake(primary, frameIdx, params);
        m_gpuProfiler.endScope(primary);
    }
    // The march's phases run in the post-scene compute group (recordPostSceneCompute); params live until it returns.
    march(m_treeVolume.ready(m_farTreeParams) ? &params : nullptr);
}

void RendererState::recordFarTreesApply(uint32 frameIdx)
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

void RendererState::destroyTreeSet(uint32 setId)
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
        m_treeCullAoPieces = 0;
        setHaveToRecordCommandBuffers();
    }
    set.pieces.destroy();
    set.types.destroy();
    set.volumeTypes.destroy();
    set.volumeData.destroy();
    set.hasVolume = false;
    m_treeVolume.invalidate(); // a running bake reads its buffers
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        set.lists[f].destroy();
        set.mappedLists[f] = {};
    }
    set.chunks.clear();
    set.typeRtCapable.clear();
    set.typeMeshes.clear();
    set.numBlocks = 0;
    set.topBlock = 0;
    set.freeBlocks = {};
    set.pendingFrees.clear();
    set.freeChunks.clear();
    set.pendingChunks.clear();
    set.alive = false;
}

void RendererState::bindTreeSet(uint32 setId)
{
    if (setId >= (uint32)m_treeSets.size() || !m_treeSets[setId].alive || m_treeCullSet == setId)
        return;
    m_treeCullSet = setId; // the recorded culls bind its buffers
    setHaveToRecordCommandBuffers();
}

void RendererState::renderTreeSet(uint32 setId, oc::span<const TreeChunkDraw> chunks, float distanceScale, bool forceFar)
{
    // The culls carry the BOUND set's records (bindTreeSet, main), one set per frame. The distance test runs
    // from the main view: the culls' camera.
    if (setId != m_treeCullSet || setId >= (uint32)m_treeSets.size())
        return;
    TreeInstanceSet& set = m_treeSets[setId];
    if (!set.alive || set.topBlock == 0)
        return;
    if (m_treeCullTaken.exchange(true, oc::memory_order_relaxed))
    {
        assert(false && "RenderTreeSet::render: one call per frame");
        return;
    }
    std::lock_guard<std::mutex> lock(m_treeSetMutex); // the set's chunks change on main meanwhile

    // The LIST in two sections. First the RT section: the RT-capable pieces (first in their chunk) of the chunks drawn
    // for GI / shadows whose RT sphere reaches into "Trees/RT range" of the scene focus - the TLAS takes ONE slot per
    // entry of this section only (m_treeCullRtPieces; tree_cull.inc.glsl treeCullTlasInstance). Then every other
    // listed piece. The culls read the whole list, in any order.
    // The RT section's own FIRST part: the chunks that reach into RTAO's range (its max distance + a ray's length) - each
    // of their trees takes a SECOND TLAS slot, its bark mesh for RTAO's rays (m_treeCullAoPieces; treeCullAoPiece tests
    // the tree itself).
    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();
    oc::span<uint32> list = set.mappedLists[frameIdx];
    const float rtRange = m_foliageParams.rtRange;
    const float aoRange = m_rtaoParams.enabled ? m_rtaoParams.maxDistance + m_rtaoParams.radius : -1.0f;
    const glm::vec3 focus = self().sceneFocusOrCamera();
    auto inRtSection = [&](const TreeChunkDraw& draw)
    {
        const TreeInstanceSet::Chunk& rt = set.chunks[draw.chunk];
        return rt.rtCount > 0 && (draw.passMask & (RendererVKLayout::PASS_GI | RendererVKLayout::PASS_SHADOW)) != 0
            && (rtRange <= 0.0f || glm::distance(rt.rtCentre, focus) - rt.rtRadius <= rtRange);
    };
    auto inAoPart = [&](const TreeChunkDraw& draw)
    {
        const TreeInstanceSet::Chunk& rt = set.chunks[draw.chunk];
        return aoRange >= 0.0f && glm::distance(rt.rtCentre, focus) - rt.rtRadius <= aoRange;
    };
    uint32 numListed = 0, numAo = 0;
    for (int part = 0; part < 2; ++part) // the RTAO part, then the rest of the RT section
    {
        for (const TreeChunkDraw& draw : chunks)
        {
            if (draw.chunk >= (uint32)set.chunks.size() || draw.passMask == 0 || !inRtSection(draw) || inAoPart(draw) != (part == 0))
                continue;
            const TreeInstanceSet::Chunk& chunk = set.chunks[draw.chunk];
            const uint32 rtCount = chunk.rtCount;
            assert(numListed + rtCount <= list.size() && "RenderTreeSet::render: a chunk listed twice");
            const uint32 bits = draw.passMask << 28;
            for (uint32 i = 0; i < rtCount; ++i)
                list[numListed + i] = (chunk.first + i) | bits;
            numListed += rtCount;
        }
        if (part == 0)
            numAo = numListed;
    }
    const uint32 numRt = numListed;
    for (const TreeChunkDraw& draw : chunks)
    {
        if (draw.chunk >= (uint32)set.chunks.size() || draw.passMask == 0)
            continue;
        const TreeInstanceSet::Chunk& chunk = set.chunks[draw.chunk];
        const uint32 skip = inRtSection(draw) ? chunk.rtCount : 0u; // listed above
        assert(numListed + chunk.count - skip <= list.size() && "RenderTreeSet::render: a chunk listed twice");
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
    m_treeCullAoPieces = numAo;
    m_treeCullDistanceScale = distanceScale;
    m_treeCullForceFar = forceFar;
}
