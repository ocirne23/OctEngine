module RendererVK;

import Core;
import Core.glm;
import Core.Transform;

import :Renderer;
import :Device;
import :Buffer;
import :Layout;
import :TreeExpandPipeline;
import :InstanceStream;

// The GPU tree expansion's CPU side (see TreeExpandPipeline): sets of placed pieces whose instance records a
// compute pass writes into the frame's instance stream. Per frame the CPU only claims the range, notes the
// per-mesh bucket sizes and queues the dispatch; the transforms are static and upload once per frame slot
// (and again after a node-buffer re-creation).

uint32 Renderer::createTreeInstanceSet(oc::span<const TreeInstanceType> types, oc::span<const TreeInstancePiece> pieces)
{
    if (m_identityInstanceOffsetIdx == UINT32_MAX)
        m_identityInstanceOffsetIdx = addMeshInstanceOffsets({ RendererVKLayout::MeshInstanceOffset{} });

    uint32 setId = 0;
    while (setId < (uint32)m_treeSets.size() && m_treeSets[setId].alive)
        ++setId;
    if (setId == (uint32)m_treeSets.size())
        m_treeSets.emplace_back();
    TreeInstanceSet& set = m_treeSets[setId];
    set.alive = true;
    set.numPieces = (uint32)pieces.size();
    for (int32& generation : set.uploadedGeneration)
        generation = -1;

    auto record = [&](const TreeInstanceRep& rep)
    {
        TreeExpandRecordGpu gpu;
        if (rep.mesh && rep.mesh->isValid())
        {
            gpu.meshMaterial = (uint32)rep.mesh->m_meshIdx | ((uint32)rep.material << 16);
            gpu.pipelineAlpha = (uint32)rep.pipeline | ((uint32)m_materials.items()[rep.material].alphaMode << 16);
        }
        return gpu;
    };
    oc::vector<TreeExpandTypeGpu> gpuTypes(types.size());
    for (size_t t = 0; t < types.size(); ++t)
    {
        const TreeInstanceType& type = types[t];
        gpuTypes[t] = TreeExpandTypeGpu{ record(type.bark), record(type.barkFade), record(type.leaves), record(type.leavesFade),
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

    oc::vector<TreeExpandPieceGpu> gpuPieces(pieces.size());
    set.transformSlots.resize(pieces.size());
    set.billboardSlots.resize(pieces.size());
    set.lodStateBases.resize(pieces.size());
    set.maxNode = 0;
    for (size_t i = 0; i < pieces.size(); ++i)
    {
        const TreeInstancePiece& piece = pieces[i];
        const TreeInstanceType& type = types[piece.type];
        set.transformSlots[i] = addRenderNodeTransform(piece.transform);
        set.billboardSlots[i] = addRenderNodeTransform(piece.transform);
        set.lodStateBases[i] = allocateLodStateRange(TREE_EXPAND_RECORDS_PER_PIECE);
        set.maxNode = oc::max(set.maxNode, oc::max(set.transformSlots[i], set.billboardSlots[i]));
        gpuPieces[i] = TreeExpandPieceGpu{ piece.centre, piece.radius, set.transformSlots[i], set.lodStateBases[i], piece.type,
            set.billboardSlots[i] };
        count(type.bark, type.barkFade);
        count(type.leaves, type.leavesFade);
        count(type.billboard, type.billboard);
    }
    set.dummyNode = addRenderNodeTransform(Transform());
    set.maxNode = oc::max(set.maxNode, set.dummyNode);
    set.meshCounts.assign(meshCounts.begin(), meshCounts.end());

    // Persistent, written once: host-visible, read by the expansion through device addresses.
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
    upload(set.pieces, gpuPieces.data(), gpuPieces.size() * sizeof(TreeExpandPieceGpu), "TreePieces");
    upload(set.types, gpuTypes.data(), gpuTypes.size() * sizeof(TreeExpandTypeGpu), "TreePieceTypes");
    return setId;
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
    const vk::DeviceAddress pieces = set.pieces.getDeviceAddress();
    oc::erase_if(m_treeDispatches, [&](const TreeExpandPipeline::Dispatch& d) { return d.pieces == pieces; });
    set.pieces.destroy();
    set.types.destroy();
    {
        const std::lock_guard lock(m_spawnMutex);
        for (uint32 node : set.transformSlots)
            m_instances.freeTransform(node);
        for (uint32 node : set.billboardSlots)
            m_instances.freeTransform(node);
        m_instances.freeTransform(set.dummyNode);
        for (uint32 base : set.lodStateBases)
            m_meshLods.releaseStateRange(base, TREE_EXPAND_RECORDS_PER_PIECE);
    }
    set.transformSlots.clear();
    set.billboardSlots.clear();
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
    if (!set.alive || set.numPieces == 0 || set.maxNode >= m_instances.getMaxRenderNodes())
        return; // nodes past the capacity: it grows at the next beginFrame

    // The static transforms, once per frame slot and node-buffer generation (a re-creation empties them).
    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();
    InstanceStream::FrameSlot& slot = m_instances.slot(frameIdx);
    const int32 generation = (int32)m_instances.getBufferGeneration();
    if (set.uploadedGeneration[frameIdx] != generation)
    {
        for (uint32 node : set.transformSlots)
            memcpy(&slot.mappedTransforms[node], &m_instances.getTransform(node), sizeof(Transform));
        for (uint32 node : set.billboardSlots)
            memcpy(&slot.mappedTransforms[node], &m_instances.getTransform(node), sizeof(Transform));
        memcpy(&slot.mappedTransforms[set.dummyNode], &m_instances.getTransform(set.dummyNode), sizeof(Transform));
        set.uploadedGeneration[frameIdx] = generation;
    }

    const uint32 base = m_instances.claimInstances(set.numPieces * TREE_EXPAND_RECORDS_PER_PIECE);
    if (base == UINT32_MAX)
        return; // full this frame: the capacity grows at the next beginFrame
    for (const auto& [meshIdx, numInstances] : set.meshCounts)
        m_instances.noteMeshInstances(meshIdx, numInstances);

    m_treeDispatches.push_back(TreeExpandPipeline::Dispatch{
        .pieces = set.pieces.getDeviceAddress(),
        .types = set.types.getDeviceAddress(),
        .numPieces = set.numPieces,
        .baseInstance = base,
        .cameraPosScale = glm::vec4(cameraPos, distanceScale),
        .forceFar = forceFar ? 1u : 0u,
        .stampedAll = InstanceStream::stampedPassMask(RendererVKLayout::PASS_ALL, m_ubo.frameIndex),
        .stampedMain = InstanceStream::stampedPassMask(RendererVKLayout::PASS_MAIN, m_ubo.frameIndex),
        .stampedProxy = InstanceStream::stampedPassMask(RendererVKLayout::PASS_SHADOW | RendererVKLayout::PASS_GI, m_ubo.frameIndex),
        .dummyNode = set.dummyNode,
        .identityOffset = m_identityInstanceOffsetIdx,
    });
}
