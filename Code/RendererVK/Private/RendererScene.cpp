module RendererVK;

import RendererVK.fwd;

import Core;
import Core.fwd;
import Core.glm;
import Core.Window;
import Core.Frustum;
import Core.imgui;
import Core.Camera;
import Core.Tweaks;
import Core.Time;
import Core.Log;

import File;

import :RenderNode;
import :VK;
import :GpuProfiler;
import :Allocator;
import :StagingManager;
import :TextureManager;
import :TextureStreamer;
import :MeshStreamer;
import :MeshDataManager;
import :glslang;
import :Layout;
import :ObjectContainer;
import :LightingUtils;

// Renderer: THE SCENE THE OUTSIDE OWNS - everything it hands the renderer and takes back, in two halves.
//
// RESIDENCY (loaded and unloaded): object containers, render nodes and their transforms, the
// mesh/material/instance-offset tables, skinned bundles and skinning palettes, the capacity growths
// they trigger, and the bindless texture descriptor upkeep.
//
// SUBMISSION (pushed every frame, mostly from JOBS - see the bottom of this file): lights, fog volumes
// and decals as lock-free bump claims, plus the persistent particle / force emitter and point-query
// slot registries, which take the spawn mutex instead. Each is a thin delegate into the object that
// owns the state; the contracts live there.
//
// Nothing here records; see RendererRecord.cpp.

void Renderer::renderNode(const RenderNode& node, uint32 passMask)
{
    const uint32 numInstances = (uint32)node.m_meshInstances.size();
    if (numInstances == 0 || passMask == 0)
        return; // destroyed (freeRenderNode clears the instances), empty, or masked out by its owner
    const uint32 startIdx = m_instances.claimInstances(numInstances);
    if (startIdx == UINT32_MAX)
        return; // did not fit this frame: the node is dropped and the capacity grows at the next beginFrame

    noteTextureUse(node, passMask);
    for (const RendererVKLayout::InMeshInstance& instance : node.m_meshInstances)
    {
        m_instances.noteMeshInstances(instance.meshIdx, 1);
        Globals::meshStreamer.noteUse(instance.meshIdx);
    }

    const uint32 frameIdx = m_swapChain.getCurrentFrameIndex();
    InstanceStream::FrameSlot& instances = m_instances.slot(frameIdx);
    // Sparse transform upload: copy only when this slot has not seen the node's latest transform.
    const uint8 generation = m_instances.getBufferGeneration();
    if ((node.m_transformUploadState >> RendererVKLayout::NUM_FRAMES_IN_FLIGHT) != generation)
        node.m_transformUploadState = uint8((generation << RendererVKLayout::NUM_FRAMES_IN_FLIGHT) | RenderNode::ALL_FRAMES_DIRTY);
    const uint8 frameBit = uint8(1u << frameIdx);
    if (node.m_transformUploadState & frameBit)
    {
        memcpy(&instances.mappedTransforms[node.m_transformIdx], &m_instances.getTransform(node.m_transformIdx), sizeof(Transform));
        node.m_transformUploadState &= uint8(~frameBit);
    }
    instances.mappedPassMasks[node.m_transformIdx] = passMask;
    memcpy(instances.mappedMeshInstances.data() + startIdx, node.m_meshInstances.data(), numInstances * sizeof(node.m_meshInstances[0]));
    if (node.m_lodStateBase != UINT32_MAX) // allocated at spawn only when the node has a LOD chain
        noteLodChainUse(node, startIdx, instances); // benign races: same-value stamp + thread-safe noteUse
}

void Renderer::noteTextureUse(const RenderNode& node, uint32 passMask)
{
    if (!Globals::textureStreamer.isStreamingEnabled())
        return;
    const Sphere bounds = node.getWorldBounds();
    const float dist = oc::max(0.01f, glm::length(bounds.pos - m_cameraPos) - bounds.radius);
    const float projPixels = bounds.radius * m_mipPixelScale / dist;
    float log2P = std::log2(oc::max(1.0f, projPixels));
    if (!(passMask & RendererVKLayout::PASS_MAIN))
        log2P -= 2.0f; // off-screen (shadow/GI-only) nodes tolerate two mips coarser

    const oc::span<const RendererVKLayout::MaterialInfo> materials =
        m_materials.getBuffer().getBackingStoreAs<RendererVKLayout::MaterialInfo>();
    for (const RendererVKLayout::InMeshInstance& instance : node.m_meshInstances)
    {
        const RendererVKLayout::MaterialInfo& material = materials[instance.materialIdx];
        Globals::textureStreamer.noteUse(material.diffuseTexIdx, log2P);
        Globals::textureStreamer.noteUse(material.normalTexIdx, log2P);
        Globals::textureStreamer.noteUse(material.metalRoughnessTexIdx, log2P);
    }
}

void Renderer::noteLodChainUse(const RenderNode& node, uint32 startIdx, InstanceStream::FrameSlot& instances)
{
    // The GPU cull picks each instance's level, so every level of a referenced chain must stay warm in
    // the mesh streamer. Only AUTHORED chains (no error data) need this: their levels are independent
    // mesh sets, and a cold one would draw nothing while it re-streams. Generated chains share LOD0's
    // set, which the caller's per-instance noteUse already touched. Stamped once per group per frame;
    // races on the stamp are benign: both threads write the same frame value, worst case double-noting.
    for (const RendererVKLayout::InMeshInstance& instance : node.m_meshInstances)
    {
        const uint32 groupIdx = m_meshLods.getGroupIdxForMesh(instance.meshIdx); // the instance references the chain's LOD0 mesh
        if (groupIdx == UINT32_MAX)
            continue;
        MeshLodGroup& group = m_meshLods.getGroup(groupIdx);
        if (group.errors[1] == 0.0f && oc::atomic_ref<uint32>(group.lastUseFrame).load(oc::memory_order_relaxed) != m_frameCounter)
        {
            oc::atomic_ref<uint32>(group.lastUseFrame).store(m_frameCounter, oc::memory_order_relaxed);
            for (uint8 k = 1; k < group.numLods; ++k)
                Globals::meshStreamer.noteUse(group.meshIdx[k]);
        }
    }
    // Publish the cull's hysteresis-state addressing for this node: stateSlot = instanceIdx + bias.
    instances.mappedLodStateBias[node.m_transformIdx] = (int32)node.m_lodStateBase - (int32)startIdx;
}



uint16 Renderer::getOrCreateSolidColorMaterial(const glm::vec3& color)
{
    const std::lock_guard lock(m_spawnMutex); // parallel entity spawning (cache + material registry)
    const glm::vec3 c = glm::clamp(color, 0.0f, 1.0f);
    const uint32 key = uint32(c.x * 255.0f + 0.5f) | (uint32(c.y * 255.0f + 0.5f) << 8)
        | (uint32(c.z * 255.0f + 0.5f) << 16);
    if (const auto it = m_solidColorMaterials.find(key); it != m_solidColorMaterials.end())
        return it->second;

    struct SolidColorTexture final : public ITextureData
    {
        Pixel pixel{};
        const char* getFileName() const override { return "solid_color"; }
        const Pixel* getPixels() const override { return &pixel; }
        uint32 getWidth() const override { return 1; }
        uint32 getHeight() const override { return 1; }
        const char* getFormatInfo() const override { return "rgba8888"; }
    };
    SolidColorTexture texture;
    texture.pixel = { uint8(key & 0xFF), uint8((key >> 8) & 0xFF), uint8((key >> 16) & 0xFF), 0xFF };
    const uint16 texIdx = Globals::textureManager.upload(texture, false, true);

    RendererVKLayout::MaterialInfo material{};
    // No MATERIAL_FLAG_NO_RAYTRACING: the flag also drops the instance from the sun shadow caster cull,
    // and every tinted game entity (units, structures, the player) went shadowless. Tints cast shadows
    // and sit in the TLAS like any lit material; gizmo exclusion lives in gizmos.oc.
    material.flags = 0;
    material.opacity = 1.0f;
    material.diffuseTexIdx = texIdx;
    material.normalTexIdx = RendererVKLayout::FALLBACK_NORMAL_TEX_IDX;
    material.metalRoughnessTexIdx = 0xFFFF; // lit shading's "no texture": metal 0, roughness 0.65
    material.alphaMode = 0;
    const uint16 materialIdx = (uint16)addMaterialInfos({ material });
    m_solidColorMaterials.emplace(key, materialIdx);
    // No re-record: the texture upload queued the slot's bindless write for every consuming set
    // (TextureManager::upload -> TextureStreamer::queueDescriptorWrite), and the material row is a
    // shared-buffer upload. Capacity growth (textures or materials) invalidates on its own.
    return materialIdx;
}

uint16 Renderer::createMeshMaterial(RendererVKLayout::EPipelineIndex pipeline, bool rayTraced)
{
    // What ObjectContainer builds for a textureless procedural scene with a pipeline override.
    RendererVKLayout::MaterialInfo material{};
    material.flags = 0;
    material.opacity = 1.0f;
    material.diffuseTexIdx = RendererVKLayout::FALLBACK_DIFFUSE_TEX_IDX;
    material.normalTexIdx = RendererVKLayout::FALLBACK_NORMAL_TEX_IDX;
    material.metalRoughnessTexIdx = UINT16_MAX;
    material.alphaMode = (uint16)RendererVKLayout::EAlphaMode::Opaque;
    if (!rayTraced)
        material.flags |= RendererVKLayout::MATERIAL_FLAG_NO_RAYTRACING;
    if (pipeline == RendererVKLayout::EPipelineIndex::Ocean)
        material.flags |= RendererVKLayout::MATERIAL_FLAG_OCEAN;
    if (pipeline == RendererVKLayout::EPipelineIndex::TerrainLit)
        material.flags |= RendererVKLayout::MATERIAL_FLAG_TERRAIN;
    const vk::Format normalFormat = Globals::textureManager.getTexture(material.normalTexIdx).getFormat();
    if (normalFormat == vk::Format::eBc5UnormBlock || normalFormat == vk::Format::eBc5SnormBlock)
        material.flags |= RendererVKLayout::MATERIAL_FLAG_BC5_NORMAL;
    return (uint16)addMaterialInfos({ material });
}

RenderMesh Renderer::createMesh(const RenderMeshData& data)
{
    RenderMesh mesh;
    if (data.vertices.empty() || data.indices.empty())
        return mesh;
    MeshDataManager& meshDataManager = Globals::meshDataManager;
    mesh.m_numVertices = (uint32)data.vertices.size();
    mesh.m_numIndices = (uint32)data.indices.size();
    mesh.m_firstVertex = (uint32)(meshDataManager.uploadVertexData(data.vertices.data(), size_t(mesh.m_numVertices) * sizeof(RendererVKLayout::MeshVertex))
        / sizeof(RendererVKLayout::MeshVertex));
    mesh.m_firstIndex = (uint32)(meshDataManager.uploadIndexData(data.indices.data(), size_t(mesh.m_numIndices) * sizeof(RendererVKLayout::MeshIndex))
        / sizeof(RendererVKLayout::MeshIndex));
    mesh.m_bounds = data.bounds;

    RendererVKLayout::MeshInfo info{};
    info.center = data.bounds.pos;
    info.radius = data.bounds.radius;
    info.indexCount = mesh.m_numIndices;
    info.firstIndex = mesh.m_firstIndex;
    info.vertexOffset = (int32)mesh.m_firstVertex;
    info.firstInstance = 0;
    mesh.m_meshIdx = (uint16)addMeshInfos({ info }, oc::span<const uint32>(&mesh.m_numVertices, 1));
    return mesh;
}

RenderNode Renderer::spawnMeshNode(const RenderMesh& mesh, uint16 materialIdx, RendererVKLayout::EPipelineIndex pipeline, const Transform& transform)
{
    RenderNode node;
    if (!mesh.isValid())
        return node;
    if (m_identityInstanceOffsetIdx == UINT32_MAX)
        m_identityInstanceOffsetIdx = addMeshInstanceOffsets({ RendererVKLayout::MeshInstanceOffset{} });
    node.m_transformIdx = addRenderNodeTransform(transform);
    node.m_bounds = mesh.m_bounds;
    RendererVKLayout::InMeshInstance& instance = node.m_meshInstances.emplace_back();
    instance.renderNodeIdx = node.m_transformIdx;
    instance.instanceOffsetIdx = m_identityInstanceOffsetIdx;
    instance.meshIdx = mesh.m_meshIdx;
    instance.materialIdx = materialIdx;
    instance.pipelineIndex = (uint16)pipeline;
    instance.alphaMode = (uint16)RendererVKLayout::EAlphaMode::Opaque;
    return node;
}

void RenderMesh::destroy()
{
    if (m_meshIdx != UINT16_MAX)
        Globals::rendererVK.destroyMesh(*this);
}

void Renderer::destroyMesh(RenderMesh& mesh)
{
    // The same free as removeObjectContainer's for an unstreamed mesh: the data ranges, then the
    // neutralized MeshInfo slot (its BLAS and side tables go with it).
    Globals::meshDataManager.freeVertexData(size_t(mesh.m_firstVertex) * sizeof(RendererVKLayout::MeshVertex),
        size_t(mesh.m_numVertices) * sizeof(RendererVKLayout::MeshVertex));
    Globals::meshDataManager.freeIndexData(size_t(mesh.m_firstIndex) * sizeof(RendererVKLayout::MeshIndex),
        size_t(mesh.m_numIndices) * sizeof(RendererVKLayout::MeshIndex));
    freeMeshInfoRange(mesh.m_meshIdx, 1);
    mesh.m_meshIdx = UINT16_MAX;
}

uint16 Renderer::loadEffectTexture(const char* filePath, bool sRGB)
{
    const uint16 idx = Globals::textureManager.upload(filePath, true, sRGB);
    // No re-record: the upload queued the slot's bindless write for every consuming set (the
    // TextureStreamer's pending-write path, applied in recordCommandBuffers before anything records);
    // growth beyond the descriptor capacity is caught by syncTextureDescriptorCapacity.
    return idx;
}


uint32 Renderer::addRenderNodeTransform(const Transform& transform)
{
    const std::lock_guard lock(m_spawnMutex); // parallel entity spawning
    return m_instances.allocateTransform(transform);
}

void RenderNode::destroy()
{
    if (m_transformIdx == UINT32_MAX && m_skinnedBundleHandle == UINT32_MAX)
        return;
    Globals::rendererVK.freeRenderNode(*this);
}

// No GPU sync needed anywhere in the free path: mesh instances, skinning jobs and palettes are
// re-uploaded into per-frame buffers every frame, transforms upload sparsely on change (dirty on
// slot reuse), so frames still in flight read their own copies and frames recorded from here on
// simply never reference the freed slots.
void Renderer::freeRenderNode(RenderNode& node)
{
    const std::lock_guard lock(m_spawnMutex); // parallel entity spawning
    if (node.m_transformIdx != UINT32_MAX)
    {
        m_instances.freeTransform(node.m_transformIdx);
        node.m_transformIdx = UINT32_MAX;
    }
    if (node.m_skinnedBundleHandle != UINT32_MAX)
    {
        releaseSkinnedBundle(node.m_skinnedBundleHandle);
        node.m_skinnedBundleHandle = UINT32_MAX;
    }
    if (node.m_lodStateBase != UINT32_MAX)
    {
        m_meshLods.releaseStateRange(node.m_lodStateBase, (uint32)node.m_meshInstances.size());
        node.m_lodStateBase = UINT32_MAX;
    }
    node.m_meshInstances.clear();
}

namespace
{
    uint32 growCapacity(uint32 current, uint32 needed, uint32 limit = UINT32_MAX)
    {
        uint64 capacity = current;
        while (capacity < needed)
            capacity *= 2;
        assert(needed <= limit);
        return (uint32)oc::min<uint64>(capacity, limit);
    }
}

void Renderer::waitForGpuAndFlushStaging()
{
    // Drain first: some buffers being grown here (e.g. m_instanceOffsets.getBuffer()) aren't per-frame-in-flight,
    // so an already-submitted draw/dispatch may still be reading one while a queued staging copy is about
    // to write into it (WRITE_AFTER_READ - no fence/semaphore otherwise orders a fresh copy submission
    // against earlier submissions on the same queue). Flushing pending copies only after this first wait
    // means their vkCmdCopyBuffer/Image writes always land on an idle GPU.
    auto waitResult = Globals::device.graphicsQueueWaitIdle();
    assert(waitResult == vk::Result::eSuccess && "Failed to wait for device idle during capacity growth");
    Globals::stagingManager.flushPending();
    // Drain again: flushPending() just submitted those copies (targeting the buffer the caller is about to
    // destroy/recreate) as new GPU work. Without this second wait, destroy() below would race that
    // submission - "buffer currently in use by command buffer" at vkDestroyBuffer.
    waitResult = Globals::device.graphicsQueueWaitIdle();
    assert(waitResult == vk::Result::eSuccess && "Failed to wait for device idle after staging flush");
    Globals::textureStreamer.onGpuIdle();
    Globals::meshStreamer.onGpuIdle();
    m_textures.processPendingFrees();
}

void Renderer::onUniqueMeshCapacityGrown(uint32 maxUniqueMeshes)
{
    m_instances.onUniqueMeshCapacityGrown(maxUniqueMeshes);
    m_meshLods.onUniqueMeshCapacityGrown(maxUniqueMeshes);
    m_rt.accel().resizeBlasAddressBuffer(maxUniqueMeshes);
    m_indirectCullComputePipeline.resizeCommandBuffers(maxUniqueMeshes);
    m_shadowCullComputePipeline.resizeCommandBuffers(maxUniqueMeshes);
    m_rainCullComputePipeline.resizeCommandBuffers(maxUniqueMeshes);
    m_staticMeshGraphicsPipeline.resizeMeshCapacity(maxUniqueMeshes);
    m_shadowMapGraphicsPipeline.resizeMeshCapacity(maxUniqueMeshes);
    m_rainMapGraphicsPipeline.resizeMeshCapacity(maxUniqueMeshes);
    setHaveToRecordCommandBuffers();
    printf("Renderer: grew unique mesh capacity to %u\n", maxUniqueMeshes);
}

void Renderer::setSkinningPalette(const RenderNode& node, oc::span<const glm::mat4> palette)
{
    assert(node.isSkinned());
    m_skinned.setPalette(node.m_skinnedBundleHandle, palette);
}


// The six consumers of the bindless arrays; BindlessTextures decides WHEN, this decides WHAT.
void Renderer::initBindlessTextures()
{
    m_textures.initialize(
        [this](uint32 count)
        {
            m_giProbePipeline.resizeTextureDescriptors(count);
            m_rtaoPipeline.resizeTextureDescriptors(count);
            m_particlePipeline.resizeTextureDescriptors(count);
            m_decalPipeline.resizeTextureDescriptors(count);
            for (PerFrameData& perFrame : m_perFrameData)
            {
                for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
                    perFrame.staticMeshPipelineDescriptorSet[eye].initialize(m_staticMeshGraphicsPipeline.getDescriptorSetLayout(), "StaticMesh", count);
                perFrame.shadowDrawDescriptorSet.initialize(m_shadowMapGraphicsPipeline.getDescriptorSetLayout(), "ShadowDraw", count);
                perFrame.rainDrawDescriptorSet.initialize(m_rainMapGraphicsPipeline.getDescriptorSetLayout(), "RainOcclusionDraw", count);
            }
            setHaveToRecordCommandBuffers();
        },
        [this](uint32 frameIdx, uint16 texIdx, vk::ImageView view)
        {
            PerFrameData& frameData = m_perFrameData[frameIdx];
            for (uint32 eye = 0; eye < m_sceneViewCount; ++eye)
                m_staticMeshGraphicsPipeline.updateTextureDescriptor(frameData.staticMeshPipelineDescriptorSet[eye].getDescriptorSet(), texIdx, view);
            m_shadowMapGraphicsPipeline.updateTextureDescriptor(frameData.shadowDrawDescriptorSet.getDescriptorSet(), texIdx, view);
            m_rainMapGraphicsPipeline.updateTextureDescriptor(frameData.rainDrawDescriptorSet.getDescriptorSet(), texIdx, view);
            m_giProbePipeline.updateTextureDescriptor(frameIdx, texIdx, view);
            m_rtaoPipeline.updateTextureDescriptor(frameIdx, texIdx, view);
            m_particlePipeline.updateTextureDescriptor(frameIdx, texIdx, view);
            m_decalPipeline.updateTextureDescriptor(frameIdx, texIdx, view);
        });
}

void Renderer::addObjectContainer(ObjectContainer* pObjectContainer)
{
    m_objectContainers.push_back(pObjectContainer);
}

void Renderer::removeObjectContainer(ObjectContainer* pObjectContainer)
{
    oc::erase(m_objectContainers, pObjectContainer);
    ObjectContainer& container = *pObjectContainer;

    // Parked skinned bundles first (they free MeshInfos/output regions that reference the sources).
    // Every live RenderNode must have been destroyed before the container, so all of its bundles are
    // parked in m_freeSkinnedBundles by now.
    if (container.m_baseSkinnedMeshIdx != UINT32_MAX)
    {
        const oc::span<const uint32> parked = m_skinned.getParkedBundles(container.m_baseSkinnedMeshIdx);
        // Copied: destroySkinnedBundle writes the registry's bundle table, which the span points into.
        const oc::vector<uint32> handles(parked.begin(), parked.end());
        for (const uint32 bundleHandle : handles)
            destroySkinnedBundle(bundleHandle);
        m_skinned.clearParkedBundles(container.m_baseSkinnedMeshIdx);
        m_skinned.releaseSources(container.m_baseSkinnedMeshIdx, container.m_numSkinnedMeshes);
    }

    // Streamed mesh sets own their CURRENT mega-buffer ranges (re-streams relocate them); everything
    // the container uploaded outside a stream set is freed from its own records.
    if (container.m_numMeshInfos > 0)
        Globals::meshStreamer.unregisterSets(container.m_baseMeshInfoIdx, container.m_numMeshInfos);
    for (const ObjectContainer::OwnedDataRange& range : container.m_ownedDataRanges)
    {
        switch (range.kind)
        {
        case ObjectContainer::EOwnedRange::Vertex:   Globals::meshDataManager.freeVertexData(range.offset, range.size); break;
        case ObjectContainer::EOwnedRange::Index:    Globals::meshDataManager.freeIndexData(range.offset, range.size); break;
        case ObjectContainer::EOwnedRange::Skinning: Globals::meshDataManager.freeSkinningData(range.offset, range.size); break;
        }
    }

    freeMeshInfoRange(container.m_baseMeshInfoIdx, container.m_numMeshInfos);
    m_materials.release(container.m_baseMaterialInfoIdx, (uint32)container.m_materialNames.size());

    m_instanceOffsets.release(container.m_baseMeshInstanceOffsetsIdx, (uint32)container.m_meshInstanceOffsets.size());
    for (uint32 i = 0; i < (uint32)container.m_rebasedOffsetBaseForIdx.size(); ++i)
        if (container.m_rebasedOffsetBaseForIdx[i] != UINT32_MAX)
            m_instanceOffsets.release(container.m_rebasedOffsetBaseForIdx[i], container.m_nodeMeshRanges[i].numNodes);
    if (container.m_skinnedIdentityOffsetIdx != UINT32_MAX)
        m_instanceOffsets.release(container.m_skinnedIdentityOffsetIdx, 1);

    for (const uint32 groupIdx : container.m_ownedLodGroups)
        freeMeshLodGroup(groupIdx); // also detaches the member meshes from GPU LOD selection

    // The images may still be sampled by in-flight frames: destroyed in present() once the GPU has
    // drained, their bindless slots rewritten to the fallback at the next record.
    m_textures.queueFree(container.m_ownedTextures);
    // Freed mega-buffer/slot ranges may be recycled by an upload later this frame; the shared-table upload
    // drains the GPU itself before queuing, so no extra synchronization is needed here for that.
}

void Renderer::freeMeshInfoRange(uint32 baseMeshInfoIdx, uint32 count)
{
    if (count == 0)
        return;
    // Neutralize the slots: zero indexCount makes the cull's DGC draws, the shadow pass and the TLAS
    // writer no-ops for any instance still referencing them this frame (a node pushed before the
    // container died), exactly like streamed-out meshes.
    const oc::span<RendererVKLayout::MeshInfo> infos = m_meshInfos.items();
    for (uint32 i = baseMeshInfoIdx; i < baseMeshInfoIdx + count; ++i)
        infos[i] = RendererVKLayout::MeshInfo{};
    m_meshInfos.upload(baseMeshInfoIdx, count);
    m_rt.onMeshInfoRangeFreed(baseMeshInfoIdx, count); // side tables, the BLASes/aliases, any queued build
    m_meshInfos.release(baseMeshInfoIdx, count);
}

// The Renderer's half of a bundle teardown: the MeshInfo range, the output vertex data, the RT job
// slots and the LOD groups are all its own. SkinnedMeshRegistry::recycleBundle then releases the
// registry's own slots and clears the entry.
void Renderer::destroySkinnedBundle(uint32 bundleHandle)
{
    const SkinnedInstanceBundle& bundle = m_skinned.getBundle(bundleHandle);
    uint32 numMeshInfos = bundle.numMeshes; // level 0s + the LOD levels appended after them
    for (uint32 k = 0; k < bundle.numMeshes; ++k)
    {
        const RendererVKLayout::SkinnedMeshSource& src = m_skinned.getSource(bundle.sourceKey + k);
        numMeshInfos += src.numLodLevels;
        // The job entry is parked (vertexCount 0) but keeps its output offset for exactly this purpose.
        Globals::meshDataManager.freeVertexData(
            (size_t)m_skinned.getJobOutVertexOffset(bundle.firstJob + k) * sizeof(RendererVKLayout::MeshVertex),
            (size_t)src.vertexCount * sizeof(RendererVKLayout::MeshVertex));
    }
    freeMeshInfoRange(bundle.baseMeshIdx, numMeshInfos);
    m_rt.accel().freeSkinnedJobSlots(bundle.firstJob, bundle.numMeshes);
    for (const uint32 groupIdx : bundle.lodGroupForMesh)
        if (groupIdx != UINT32_MAX)
            freeMeshLodGroup(groupIdx); // also detaches the member meshes from GPU LOD selection
    m_skinned.recycleBundle(bundleHandle);
}

void Renderer::setMeshStreamedOut(uint16 meshInfoIdx)
{
    m_meshInfos.items()[meshInfoIdx].indexCount = 0;
    m_meshInfos.upload(meshInfoIdx, 1);
    // The BLAS goes with the mesh data (rebuilt on re-stream); safe because eviction requires the set
    // to have been unreferenced for far longer than any in-flight TLAS.
    m_rt.onMeshEvicted(meshInfoIdx);
}

void Renderer::setMeshStreamedIn(uint16 meshInfoIdx, int32 vertexOffset, uint32 firstIndex, uint32 indexCount)
{
    RendererVKLayout::MeshInfo& info = m_meshInfos.items()[meshInfoIdx];
    info.vertexOffset = vertexOffset;
    info.firstIndex = firstIndex;
    info.indexCount = indexCount;
    m_meshInfos.upload(meshInfoIdx, 1);
    m_rt.onMeshStreamedIn(meshInfoIdx); // its BLAS rebuilds in the next recordGlobalIllum
}

// SharedTable does the slot claim, the mirror write, the upload and any capacity growth (holes are
// never compacted, so a range a destroyed container freed is reused first). The callback is the
// Renderer's own per-slot bookkeeping: the parallel CPU side tables and the BLAS build queue.
uint32 Renderer::addMeshInfos(const oc::vector<RendererVKLayout::MeshInfo>& meshInfos, oc::span<const uint32> vertexCounts, bool skinnedOutputs)
{
    assert(vertexCounts.size() == meshInfos.size() && "one exact vertex count per MeshInfo");
    if (meshInfos.empty())
        return m_meshInfos.count();
    const std::lock_guard lock(m_spawnMutex); // parallel entity spawning

    const uint32 count = (uint32)meshInfos.size();
    const uint32 baseMeshInfoIdx = m_meshInfos.add(meshInfos, [&](uint32 base, bool reused)
    {
        if (!reused)
        {
            m_instances.resizePerMeshCounts(base + count);
            m_meshLods.resizeMeshMapping(base + count);
            assert(base + count < USHRT_MAX);
        }
        // The BLAS side tables and the build queue: a recycled slot below the one-time build
        // watermark needs its build queued explicitly, since the watermark scan will not reach it.
        m_rt.onMeshInfosAdded(base, count, vertexCounts, skinnedOutputs, reused);
        // Fresh mesh slots must read as "no LOD chain" on the GPU (device memory starts undefined;
        // addMeshLodGroup overwrites the chained ones right after). A growth re-uploads the whole
        // mapping itself, so this only has to cover the within-capacity append.
        if (!reused && base + count <= m_meshInfos.capacity())
            m_meshLods.uploadMeshMapping(base, count);
    });

    // After the capacity check: the alias buffer is grown by resizeBlasAddressBuffer inside the growth
    // callback, and setNumMeshes writes identity entries for the new range.
    m_rt.accel().setNumMeshes(m_meshInfos.count()); // identity RT aliases until a LOD group overrides

    return baseMeshInfoIdx;
}

uint32 Renderer::addMaterialInfos(const oc::vector<RendererVKLayout::MaterialInfo>& materialInfos)
{
    if (materialInfos.empty())
        return m_materials.count();
    const std::lock_guard lock(m_spawnMutex); // parallel entity spawning
    // Within capacity this is a contents-only upload: every pass binds the whole buffer and nothing
    // recorded depends on the material count, so only a growth re-records.
    return m_materials.add(materialInfos);
}

uint32 Renderer::addMeshInstanceOffsets(const oc::vector<RendererVKLayout::MeshInstanceOffset>& meshInstanceOffsets)
{
    if (meshInstanceOffsets.empty())
        return m_instanceOffsets.count();
    const std::lock_guard lock(m_spawnMutex); // parallel entity spawning
    // Within capacity this is a contents-only upload: every pass binds the whole buffer and nothing
    // recorded depends on the offset count, so only a growth re-records (rebased static spawns hit this).
    return m_instanceOffsets.add(meshInstanceOffsets);
}


// ---- The per-frame submission surface ----

void Renderer::addLightInfo(const RendererVKLayout::LightInfo& light)
{
    const uint32 idx = m_submission.addLight(m_swapChain.getCurrentFrameIndex(), light);
    if (idx != UINT32_MAX)
        m_lightGridComputePipeline.addLight(idx, light); // bounds + grid claims, right here on the adding thread
}

void Renderer::addFogVolume(const RendererVKLayout::FogVolumeInfo& fogVolume)
{
    m_submission.addFogVolume(m_swapChain.getCurrentFrameIndex(), fogVolume);
}

void Renderer::addPointLight(const PointLight& light)   { addLightInfo(light); }
void Renderer::addAreaLight(const AreaLight& areaLight) { addLightInfo(areaLight); }
void Renderer::addSpotLight(const SpotLight& spotLight) { addLightInfo(spotLight); }

void Renderer::addDecal(const RendererVKLayout::DecalInfo& decal)
{
    if (const uint32 idx = m_submission.claimDecal(); idx != UINT32_MAX)
        m_decalPipeline.getMapped(m_swapChain.getCurrentFrameIndex())[idx] = decal;
}

// The emitter slot contract (KILL flag drain, retirement) lives in ParticleState; the mutex here is
// the spawn one, taken for parallel entity spawning.
uint32 Renderer::createParticleEmitter(const RendererVKLayout::ParticleEmitterGpu& desc)
{
    const std::lock_guard lock(m_spawnMutex);
    return m_particles.createEmitter(m_frameCounter, desc);
}

void Renderer::updateParticleEmitter(uint32 slot, const RendererVKLayout::ParticleEmitterGpu& desc)
{
    m_particles.updateEmitter(slot, desc);
}

void Renderer::emitParticles(uint32 slot, uint32 count)
{
    m_particles.emit(slot, count);
}

void Renderer::destroyParticleEmitter(uint32 slot)
{
    const std::lock_guard lock(m_spawnMutex);
    m_particles.destroyEmitter(slot, m_frameCounter);
}

void Renderer::setRainOcclusionVolume(const glm::vec3& center, const glm::vec3& halfExtents)
{
    m_particles.requestRainVolume(center, halfExtents);
}

// Every force slot mutator takes the spawn mutex (parallel entity spawning); the slot bookkeeping and
// the retirement contract live in ForceFieldState.
uint32 Renderer::createForceEmitter(const RendererVKLayout::ForceEmitterGpu& desc)
{
    const std::lock_guard lock(m_spawnMutex);
    return m_force.createEmitter(m_frameCounter, desc);
}

void Renderer::updateForceEmitter(uint32 slot, const RendererVKLayout::ForceEmitterGpu& desc)
{
    m_force.updateEmitter(slot, desc);
}

void Renderer::destroyForceEmitter(uint32 slot)
{
    const std::lock_guard lock(m_spawnMutex);
    m_force.destroyEmitter(slot, m_frameCounter);
}

uint32 Renderer::createForceQuerySlot()
{
    const std::lock_guard lock(m_spawnMutex);
    return m_force.createQuery(m_frameCounter);
}

void Renderer::setForceQuery(uint32 slot, const glm::vec3& pos)
{
    m_force.setQuery(slot, pos);
}

void Renderer::destroyForceQuerySlot(uint32 slot)
{
    const std::lock_guard lock(m_spawnMutex);
    m_force.destroyQuery(slot, m_frameCounter);
}

glm::vec4 Renderer::getForceEmitterReadback(uint32 slot) const
{
    const oc::span<const glm::vec4> forces = m_forceFieldPipeline.getForceReadback(m_swapChain.getCurrentFrameIndex());
    return slot < forces.size() ? forces[slot] : glm::vec4(0.0f);
}

RendererVKLayout::ForceBakeReadback Renderer::getForceBakeReadback() const
{
    return m_forceFieldPipeline.getBakeReadback(m_swapChain.getCurrentFrameIndex());
}

RendererVKLayout::ForceQueryResult Renderer::getForceQueryReadback(uint32 slot) const
{
    const oc::span<const RendererVKLayout::ForceQueryResult> results = m_forceFieldPipeline.getQueryReadback(m_swapChain.getCurrentFrameIndex());
    return slot < results.size() ? results[slot] : RendererVKLayout::ForceQueryResult{ RendererVKLayout::MAX_FORCE_TEAMS, 0.0f, 0.0f, 0u };
}

void Renderer::setForceFieldParams(const ForceFieldParams& params)
{
    // The grid toggle and the LIVE team count are compile-time shader defines (FORCE_GRID /
    // NUM_FORCE_TEAMS): rebuild the force pipelines, same GPU-idle + reload pattern as the ocean
    // hit-lighting tweak. A team-count change additionally remakes the team-sized bake
    // volume/buffers (setNumTeams) - a game-mode event, never per-frame.
    const uint32 numTeams = glm::clamp(params.numTeams, 2u, RendererVKLayout::MAX_FORCE_TEAMS);
    if (params.useGrid != m_forceFieldPipeline.getUseGrid()
        || params.densityView != m_forceFieldPipeline.getDensityView() // FORCE_DENSITY_VIEW: the debug overlay is a define too
        || numTeams != m_forceFieldPipeline.getNumTeams()
        || params.unionHalfRes != m_forceFieldPipeline.getUnionHalfRes()
        || params.unionJitter != m_forceFieldPipeline.getUnionJitter())
    {
        if (Globals::device.graphicsQueueWaitIdle() == vk::Result::eSuccess)
        {
            printf("ForceFieldPipeline: rebuilding force pipelines (grid %d, %u teams, union %s%s)\n",
                params.useGrid ? 1 : 0, numTeams, // loud: a silent skip here strands stale binaries
                params.unionHalfRes ? "half-res" : "full-res", params.unionJitter ? "" : ", no jitter");
            // Shader source reads from the frame loop: intentional, rare main-thread IO (a game-
            // mode switch or the grid tweak), declared so FileSystem's assert stays meaningful.
            const FileSystem::AllowMainThreadIO allowIo;
            m_forceFieldPipeline.setUseGrid(params.useGrid);
            m_forceFieldPipeline.setDensityView(params.densityView);
            m_forceFieldPipeline.setNumTeams(numTeams);
            m_forceFieldPipeline.setUnionJitter(params.unionJitter);
            if (params.unionHalfRes != m_forceFieldPipeline.getUnionHalfRes())
            {
                // The targets change size (and the march target existence) with the mode.
                m_forceFieldPipeline.setUnionHalfRes(params.unionHalfRes);
                const vk::Extent2D ext = m_swapChain.getLayout().extent;
                m_forceFieldPipeline.resizeIntervalTarget(ext.width, ext.height);
            }
            m_forceFieldPipeline.reloadShaders(m_perFrameData[0].sceneColor.getRenderPass());
            setHaveToRecordCommandBuffers();
        }
    }
    m_force.setParams(params);
}
