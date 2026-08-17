module RendererVK;

import Core;
import :VK;
import :Device;

AccelerationStructure::AccelerationStructure() {}
AccelerationStructure::~AccelerationStructure()
{
    vk::Device dev = Globals::device.getDevice();
    for (Blas& blas : m_blasList)
        if (blas.handle)
            dev.destroyAccelerationStructureKHR(blas.handle);
    for (RetiredBlas& retired : m_retiredBlas)
        dev.destroyAccelerationStructureKHR(retired.handle);
    for (CompactionBatch& batch : m_compactionBatches)
        dev.destroyQueryPool(batch.queryPool);
    for (auto& slot : m_skinnedBlas)
        for (Blas& blas : slot)
            if (blas.handle)
                dev.destroyAccelerationStructureKHR(blas.handle);
    for (vk::AccelerationStructureKHR tlas : m_tlas)
        if (tlas)
            dev.destroyAccelerationStructureKHR(tlas);
}

void AccelerationStructure::initialize(uint32 maxUniqueMeshes)
{
    vk::PhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
    vk::PhysicalDeviceProperties2 props2{ .pNext = &asProps };
    Globals::device.getPhysicalDevice().getProperties2(&props2);
    m_scratchAlignment = asProps.minAccelerationStructureScratchOffsetAlignment;
    if (m_scratchAlignment == 0)
        m_scratchAlignment = 256;

    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        m_blasAddressBuffers[f].initialize(maxUniqueMeshes * sizeof(uint64),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached, false, "AS.blasAddresses");
        m_mappedBlasAddresses[f] = m_blasAddressBuffers[f].mapMemory<uint64>();
    }
    m_staticBlasAddr.assign(maxUniqueMeshes, 0);
    m_staticAddrDirtyBits.assign(maxUniqueMeshes, 0);
    m_meshAliasBuffer.initialize(maxUniqueMeshes * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached, false, "AS.meshAlias");
    m_mappedMeshAlias = m_meshAliasBuffer.mapMemory<uint32>();
}

void AccelerationStructure::resizeBlasAddressBuffer(uint32 maxUniqueMeshes)
{
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
    {
        const oc::vector<uint64> oldAddresses(m_mappedBlasAddresses[f].begin(), m_mappedBlasAddresses[f].end());
        m_blasAddressBuffers[f].initialize(maxUniqueMeshes * sizeof(uint64),
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached, false, "AS.blasAddresses");
        m_mappedBlasAddresses[f] = m_blasAddressBuffers[f].mapMemory<uint64>();
        memcpy(m_mappedBlasAddresses[f].data(), oldAddresses.data(), oldAddresses.size() * sizeof(uint64));
        m_blasAddressBuffers[f].flushMappedMemory(oldAddresses.size() * sizeof(uint64));
    }
    if (maxUniqueMeshes > m_staticBlasAddr.size())
    {
        m_staticBlasAddr.resize(maxUniqueMeshes, 0);
        m_staticAddrDirtyBits.resize(maxUniqueMeshes, 0);
    }
    const oc::vector<uint32> oldAliases(m_mappedMeshAlias.begin(), m_mappedMeshAlias.end());
    m_meshAliasBuffer.initialize(maxUniqueMeshes * sizeof(uint32),
        vk::BufferUsageFlagBits2::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCached, false, "AS.meshAlias");
    m_mappedMeshAlias = m_meshAliasBuffer.mapMemory<uint32>();
    memcpy(m_mappedMeshAlias.data(), oldAliases.data(), oldAliases.size() * sizeof(uint32));
    m_meshAliasBuffer.flushMappedMemory(oldAliases.size() * sizeof(uint32));
}

void AccelerationStructure::setNumMeshes(uint32 numMeshes)
{
    if (numMeshes <= m_numAliasMeshes)
        return;
    assert(numMeshes <= m_mappedMeshAlias.size() && "mesh alias buffer behind capacity growth");
    for (uint32 m = m_numAliasMeshes; m < numMeshes; ++m)
        m_mappedMeshAlias[m] = m; // identity: owns its own BLAS until a LOD group aliases it
    m_numAliasMeshes = numMeshes;
    m_meshAliasBuffer.flushMappedMemory(m_numAliasMeshes * sizeof(uint32));
}

void AccelerationStructure::setMeshAlias(uint32 meshIdx, uint32 aliasIdx)
{
    assert(meshIdx < m_numAliasMeshes && aliasIdx < m_numAliasMeshes);
    m_mappedMeshAlias[meshIdx] = aliasIdx;
    m_meshAliasBuffer.flushMappedMemory(m_numAliasMeshes * sizeof(uint32));
}

void AccelerationStructure::markStaticBlasAddr(uint32 meshIdx, uint64 addr)
{
    if (meshIdx >= m_staticBlasAddr.size())
    {
        m_staticBlasAddr.resize(meshIdx + 1, 0);
        m_staticAddrDirtyBits.resize(meshIdx + 1, 0);
    }
    m_staticBlasAddr[meshIdx] = addr;
    constexpr uint8 allBits = uint8((1u << RendererVKLayout::NUM_FRAMES_IN_FLIGHT) - 1);
    uint8& bits = m_staticAddrDirtyBits[meshIdx];
    if (bits == allBits)
        return; // already queued for every slot (value updated in place above)
    for (uint32 f = 0; f < RendererVKLayout::NUM_FRAMES_IN_FLIGHT; ++f)
        if (!(bits & (1u << f)))
            m_staticAddrDirtyLists[f].push_back(meshIdx);
    bits = allBits;
}

void AccelerationStructure::syncFrameAddresses(uint32 frameIdx)
{
    oc::vector<uint32>& list = m_staticAddrDirtyLists[frameIdx];
    if (list.empty())
        return;
    const oc::span<uint64> mapped = m_mappedBlasAddresses[frameIdx];
    for (const uint32 meshIdx : list)
    {
        mapped[meshIdx] = m_staticBlasAddr[meshIdx];
        m_staticAddrDirtyBits[meshIdx] &= uint8(~(1u << frameIdx));
    }
    list.clear();
    m_blasAddressBuffers[frameIdx].flushMappedMemory(m_blasAddressBuffers[frameIdx].getSize());
}

void AccelerationStructure::onMeshEvicted(uint32 meshIdx)
{
    if (meshIdx >= m_numAliasMeshes)
        return;
    markStaticBlasAddr(meshIdx, 0);
    if (m_mappedMeshAlias[meshIdx] == meshIdx && meshIdx < m_blasList.size() && m_blasList[meshIdx].handle)
    {
        Globals::device.getDevice().destroyAccelerationStructureKHR(m_blasList[meshIdx].handle);
        m_blasList[meshIdx].handle = nullptr;
        m_blasList[meshIdx].buffer.destroy();
        // Anything aliased to this BLAS (authored-chain levels in other, still-resident sets) must stop
        // referencing it too — those levels go RT-invisible until this mesh re-streams and rebuilds.
        for (uint32 m = 0; m < m_numAliasMeshes; ++m)
            if (m_mappedMeshAlias[m] == meshIdx)
                markStaticBlasAddr(m, 0);
    }
}

void AccelerationStructure::onMeshRangeFreed(uint32 firstMeshIdx, uint32 count)
{
    for (uint32 meshIdx = firstMeshIdx; meshIdx < firstMeshIdx + count && meshIdx < m_numAliasMeshes; ++meshIdx)
    {
        markStaticBlasAddr(meshIdx, 0);
        if (m_mappedMeshAlias[meshIdx] == meshIdx && meshIdx < m_blasList.size() && m_blasList[meshIdx].handle)
        {
            m_retiredBlas.push_back(RetiredBlas{ m_blasList[meshIdx].handle, oc::move(m_blasList[meshIdx].buffer), m_compactionFrame });
            m_blasList[meshIdx].handle = nullptr;
            m_blasList[meshIdx].compacted = false;
        }
        // Anything aliased to a freed mesh is a LOD level of the same container, freed in the same
        // range; identity aliases leave the slots ready for reuse by a later addMeshInfos.
        m_mappedMeshAlias[meshIdx] = meshIdx;
    }
    m_meshAliasBuffer.flushMappedMemory(m_numAliasMeshes * sizeof(uint32));
}

void AccelerationStructure::freeSkinnedJobSlots(uint32 firstJob, uint32 count)
{
    for (auto& slot : m_skinnedBlas)
    {
        for (uint32 i = firstJob; i < firstJob + count && i < (uint32)slot.size(); ++i)
        {
            if (slot[i].handle)
            {
                m_retiredBlas.push_back(RetiredBlas{ slot[i].handle, oc::move(slot[i].buffer), m_compactionFrame });
                slot[i].handle = nullptr;
            }
        }
    }
}

void AccelerationStructure::ensureScratch(Buffer& scratch, vk::DeviceAddress& outAlignedAddr, vk::DeviceSize needed)
{
    const vk::DeviceSize allocSize = needed + m_scratchAlignment;
    if (scratch.getSize() < allocSize)
    {
        scratch.initialize(allocSize,
            vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "AS.scratch");
    }
    const vk::DeviceAddress base = scratch.getDeviceAddress();
    const vk::DeviceAddress mask = (vk::DeviceAddress)m_scratchAlignment - 1;
    outAlignedAddr = (base + mask) & ~mask;
}

void AccelerationStructure::recordBuildBlas(uint32 frameIdx, vk::CommandBuffer cmd, Buffer& vertexBuffer, Buffer& indexBuffer,
    const RendererVKLayout::MeshInfo* meshInfos, const uint32* vertexCounts, oc::span<const uint32> meshIndices,
    bool allowCompaction)
{
    // Only self-aliased meshes with live geometry build a BLAS: LOD-chain levels share their RT level's,
    // and streamed-out meshes (indexCount 0) wait for their re-stream rebuild.
    oc::vector<uint32> toBuild;
    toBuild.reserve(meshIndices.size());
    uint32 maxMeshIdx = 0;
    for (const uint32 meshIdx : meshIndices)
    {
        maxMeshIdx = oc::max(maxMeshIdx, meshIdx);
        if (getMeshAlias(meshIdx) == meshIdx && meshInfos[meshIdx].indexCount > 0 && vertexCounts[meshIdx] > 0)
            toBuild.push_back(meshIdx);
    }
    if (m_blasList.size() < (size_t)maxMeshIdx + 1)
        m_blasList.resize((size_t)maxMeshIdx + 1);
    m_numBlas = oc::max(m_numBlas, maxMeshIdx + (meshIndices.empty() ? 0u : 1u));

    vk::Device dev = Globals::device.getDevice();
    const vk::DeviceAddress vbAddr = vertexBuffer.getDeviceAddress();
    const vk::DeviceAddress ibAddr = indexBuffer.getDeviceAddress();

    // Pass 1: assemble geometry + build infos, query sizes, find the total scratch requirement.
    const uint32 count = (uint32)toBuild.size();
    oc::vector<vk::AccelerationStructureGeometryKHR> geoms(count);
    oc::vector<vk::AccelerationStructureBuildGeometryInfoKHR> buildInfos(count);
    oc::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges(count);
    oc::vector<vk::AccelerationStructureBuildSizesInfoKHR> sizes(count);
    oc::vector<vk::DeviceSize> scratchOffsets(count);
    vk::DeviceSize totalScratch = 0;
    const vk::DeviceSize align = m_scratchAlignment;
    for (uint32 i = 0; i < count; i++)
    {
        const RendererVKLayout::MeshInfo& mi = meshInfos[toBuild[i]];
        const uint32 primCount = mi.indexCount / 3;

        // maxVertex MUST be tight: declaring the whole buffer makes the driver bound the BLAS over every
        // vertex, ballooning its AABB to span the scene and degenerating the BVH (catastrophically slow
        // ray traversal -> TDR / device lost). The Renderer tracks every mesh's exact vertex count (the
        // old next-mesh-offset heuristic broke once LOD levels shared vertex ranges and the streamer's
        // free-list ended offset monotonicity).
        //
        // vk::DeviceOrHostAddressConstKHR / AccelerationStructureGeometryDataKHR are unions, which can't
        // be designated-initialized; default-construct then assign the active union member.
        vk::AccelerationStructureGeometryTrianglesDataKHR tri{
            .vertexFormat = vk::Format::eR32G32B32Sfloat,
            .vertexStride = sizeof(RendererVKLayout::MeshVertex),
            .maxVertex = vertexCounts[toBuild[i]] - 1u,
            .indexType = vk::IndexType::eUint32,
        };
        tri.vertexData.deviceAddress = vbAddr;
        tri.indexData.deviceAddress = ibAddr;
        tri.transformData.deviceAddress = 0;

        geoms[i] = vk::AccelerationStructureGeometryKHR{
            .geometryType = vk::GeometryTypeKHR::eTriangles,
            // Opacity is decided per TLAS instance (ForceOpaque for everything except alpha-masked
            // materials, see gi_tlas_instances.cs.glsl), so the BLAS geometry must not be flagged opaque.
            .flags = {},
        };
        geoms[i].geometry.triangles = tri;
        buildInfos[i] = vk::AccelerationStructureBuildGeometryInfoKHR{
            .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
            .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace
                | (allowCompaction ? vk::BuildAccelerationStructureFlagBitsKHR::eAllowCompaction : vk::BuildAccelerationStructureFlagBitsKHR{}),
            .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = 1,
            .pGeometries = &geoms[i],
        };
        uint32 maxPrim = primCount;
        sizes[i] = dev.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfos[i], maxPrim);
        // Give every build its own scratch region (aligned) so concurrent builds never alias scratch
        // memory -- sharing one scratch region across builds is what was faulting the GPU.
        scratchOffsets[i] = totalScratch;
        totalScratch += (sizes[i].buildScratchSize + align - 1) & ~(align - 1);

        ranges[i] = vk::AccelerationStructureBuildRangeInfoKHR{
            .primitiveCount = primCount,
            .primitiveOffset = mi.firstIndex * (uint32)sizeof(RendererVKLayout::MeshIndex),
            .firstVertex = (uint32)mi.vertexOffset,
            .transformOffset = 0,
        };
    }

    if (count > 0)
    {
        ensureScratch(m_blasScratch[frameIdx], m_blasScratchAlignedAddr[frameIdx], totalScratch);

        // Pass 2: create and build each BLAS, serialized with a barrier between builds (each also uses a
        // disjoint scratch region). Serializing removes any concurrent-build interaction as a variable.
        for (uint32 i = 0; i < count; i++)
        {
            Blas& blas = m_blasList[toBuild[i]];
            if (blas.handle)
                dev.destroyAccelerationStructureKHR(blas.handle);
            blas.compacted = false;
            blas.buffer.initialize(sizes[i].accelerationStructureSize,
                vk::BufferUsageFlagBits2::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                vk::MemoryPropertyFlagBits::eDeviceLocal, false, "AS.blas");

            vk::AccelerationStructureCreateInfoKHR ci{
                .buffer = blas.buffer.getBuffer(),
                .size = sizes[i].accelerationStructureSize,
                .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
            };
            auto res = dev.createAccelerationStructureKHR(ci);
            assert(res.result == vk::Result::eSuccess && "Failed to create BLAS");
            blas.handle = res.value;

            buildInfos[i].dstAccelerationStructure = blas.handle;
            buildInfos[i].scratchData.deviceAddress = m_blasScratchAlignedAddr[frameIdx] + scratchOffsets[i];
            const vk::AccelerationStructureBuildRangeInfoKHR* pRange = &ranges[i];
            cmd.buildAccelerationStructuresKHR(buildInfos[i], pRange);

            vk::AccelerationStructureDeviceAddressInfoKHR ai{ .accelerationStructure = blas.handle };
            const uint64 blasAddr = dev.getAccelerationStructureAddressKHR(ai);
            markStaticBlasAddr(toBuild[i], blasAddr); // published to each frame slot in its own fenced frame

            vk::MemoryBarrier2 b{
                .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eAccelerationStructureReadKHR,
                .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            };
            cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &b });
        }

        // One barrier so all BLAS builds complete before the TLAS build (or any reads) consume them.
        vk::MemoryBarrier2 bar{
            .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
            .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });

        // Compacted-size queries for the freshly built BLASes; recordCompaction reads them once the
        // frame-slot fence guarantees this command buffer executed (a recorded CB is always submitted:
        // the acquire-failure early-out happens before recordCommandBuffers).
        if (allowCompaction)
        {
            CompactionBatch batch;
            batch.meshIndices = toBuild;
            batch.handles.reserve(count);
            for (uint32 i = 0; i < count; i++)
                batch.handles.push_back(m_blasList[toBuild[i]].handle);
            batch.frameStamp = m_compactionFrame;
            auto poolRes = dev.createQueryPool(vk::QueryPoolCreateInfo{
                .queryType = vk::QueryType::eAccelerationStructureCompactedSizeKHR, .queryCount = count });
            assert(poolRes.result == vk::Result::eSuccess && "Failed to create compaction query pool");
            batch.queryPool = poolRes.value;
            cmd.resetQueryPool(batch.queryPool, 0, count);
            cmd.writeAccelerationStructuresPropertiesKHR((uint32)batch.handles.size(), batch.handles.data(),
                vk::QueryType::eAccelerationStructureCompactedSizeKHR, batch.queryPool, 0);
            m_compactionBatches.push_back(oc::move(batch));
        }
    }

    // Address-fill: every aliased mesh's entry mirrors its target's authoritative address (skinned meshes
    // are self-aliased no-ops here). Covers both freshly built chains and re-stream rebuilds whose aliased
    // levels live in other registration batches. syncFrameAddresses publishes these per slot.
    for (uint32 m = 0; m < m_numAliasMeshes; ++m)
        if (m_mappedMeshAlias[m] != m)
        {
            const uint32 target = m_mappedMeshAlias[m];
            markStaticBlasAddr(m, target < m_staticBlasAddr.size() ? m_staticBlasAddr[target] : 0);
        }
}

void AccelerationStructure::recordCompaction(uint32 frameIdx, vk::CommandBuffer cmd)
{
    (void)frameIdx; // address changes go through markStaticBlasAddr + syncFrameAddresses (per-slot safe)
    vk::Device dev = Globals::device.getDevice();
    m_compactionFrame++;

    // Retire originals whose replacement every in-flight TLAS has picked up by now.
    size_t kept = 0;
    for (RetiredBlas& retired : m_retiredBlas)
    {
        if (m_compactionFrame - retired.frameStamp < RendererVKLayout::NUM_FRAMES_IN_FLIGHT)
        {
            m_retiredBlas[kept++] = oc::move(retired);
            continue;
        }
        dev.destroyAccelerationStructureKHR(retired.handle);
        retired.buffer.destroy();
    }
    m_retiredBlas.resize(kept);

    // +1 over the frames-in-flight window: a batch stamped in frame F is submitted at F's end, and F's
    // fence is first waited by beginFrame(F+2) — reading at F+1 would make eWait stall on the live frame.
    bool recordedCopy = false;
    while (!m_compactionBatches.empty()
        && m_compactionFrame - m_compactionBatches.front().frameStamp > RendererVKLayout::NUM_FRAMES_IN_FLIGHT)
    {
        CompactionBatch& batch = m_compactionBatches.front();
        oc::vector<uint64> compactedSizes(batch.meshIndices.size());
        // eWait is a formality: the batch's command buffer completed behind the frame-slot fence.
        const vk::Result queryResult = dev.getQueryPoolResults(batch.queryPool, 0, (uint32)compactedSizes.size(),
            compactedSizes.size() * sizeof(uint64), compactedSizes.data(), sizeof(uint64),
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
        if (queryResult == vk::Result::eSuccess)
        {
            for (uint32 i = 0; i < (uint32)batch.meshIndices.size(); i++)
            {
                const uint32 meshIdx = batch.meshIndices[i];
                Blas& blas = m_blasList[meshIdx];
                if (blas.handle != batch.handles[i]) // rebuilt or evicted since the query: stale result
                    continue;
                const uint64 compactedSize = compactedSizes[i];
                if (compactedSize == 0 || compactedSize >= blas.buffer.getSize())
                {
                    blas.compacted = true; // no win; keep as-is
                    continue;
                }

                Buffer compactBuffer;
                compactBuffer.initialize(compactedSize,
                    vk::BufferUsageFlagBits2::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                    vk::MemoryPropertyFlagBits::eDeviceLocal, false, "AS.blasCompact");
                vk::AccelerationStructureCreateInfoKHR ci{
                    .buffer = compactBuffer.getBuffer(),
                    .size = compactedSize,
                    .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
                };
                auto res = dev.createAccelerationStructureKHR(ci);
                assert(res.result == vk::Result::eSuccess && "Failed to create compacted BLAS");

                cmd.copyAccelerationStructureKHR(vk::CopyAccelerationStructureInfoKHR{
                    .src = blas.handle, .dst = res.value, .mode = vk::CopyAccelerationStructureModeKHR::eCompact });

                m_compactionSavedBytes += blas.buffer.getSize() - compactedSize;
                m_retiredBlas.push_back(RetiredBlas{ blas.handle, oc::move(blas.buffer), m_compactionFrame });
                blas.handle = res.value;
                blas.buffer = oc::move(compactBuffer);
                blas.compacted = true;

                vk::AccelerationStructureDeviceAddressInfoKHR ai{ .accelerationStructure = blas.handle };
                const uint64 blasAddr = dev.getAccelerationStructureAddressKHR(ai);
                // Do NOT write other frame slots here: their in-flight TLAS-instance compute is reading
                // them, and this compacted BLAS's copy hasn't executed in those frames yet. Each slot
                // publishes the new address in its own fenced frame via syncFrameAddresses.
                markStaticBlasAddr(meshIdx, blasAddr);
                recordedCopy = true;
            }
        }
        dev.destroyQueryPool(batch.queryPool);
        m_compactionBatches.pop_front();
    }

    if (recordedCopy)
    {
        // Aliased LOD levels follow their target's new address (published per slot by syncFrameAddresses),
        // then this frame's TLAS build (later in the same command buffer) must see the copies completed.
        // Compact copies execute in the acceleration-structure-build stage (no ray_tracing_maintenance1 =
        // no separate copy stage).
        for (uint32 m = 0; m < m_numAliasMeshes; ++m)
            if (m_mappedMeshAlias[m] != m)
            {
                const uint32 target = m_mappedMeshAlias[m];
                markStaticBlasAddr(m, target < m_staticBlasAddr.size() ? m_staticBlasAddr[target] : 0);
            }

        vk::MemoryBarrier2 bar{
            .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
            .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR | vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bar });
    }
}

uint64 AccelerationStructure::getBlasTotalBytes() const
{
    uint64 total = 0;
    for (const Blas& blas : m_blasList)
        if (blas.handle)
            total += blas.buffer.getSize();
    return total;
}

void AccelerationStructure::recordBuildSkinnedBlas(vk::CommandBuffer cmd, uint32 frameIdx, Buffer& vertexBuffer, Buffer& indexBuffer,
    oc::span<const SkinnedBlasBuild> builds)
{
    if (builds.empty())
        return;

    vk::Device dev = Globals::device.getDevice();
    const vk::DeviceAddress vbAddr = vertexBuffer.getDeviceAddress();
    const vk::DeviceAddress ibAddr = indexBuffer.getDeviceAddress();
    oc::vector<Blas>& slot = m_skinnedBlas[frameIdx];
    bool wroteAddresses = false;

    // Pass 1: build geometry + sizes, find total scratch, (re)allocate BLAS buffers for new entries.
    const uint32 count = (uint32)builds.size();
    oc::vector<vk::AccelerationStructureGeometryKHR> geoms(count);
    oc::vector<vk::AccelerationStructureBuildGeometryInfoKHR> buildInfos(count);
    oc::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges(count);
    oc::vector<vk::DeviceSize> scratchOffsets(count);
    vk::DeviceSize totalScratch = 0;
    const vk::DeviceSize align = m_scratchAlignment;
    slot.resize(count);
    for (uint32 i = 0; i < count; ++i)
    {
        const SkinnedBlasBuild& b = builds[i];
        const uint32 primCount = b.indexCount / 3;
        if (primCount == 0)
            continue; // parked bundle (freed skinned node): keep the slot in place, skip the rebuild

        vk::AccelerationStructureGeometryTrianglesDataKHR tri{
            .vertexFormat = vk::Format::eR32G32B32Sfloat,
            .vertexStride = sizeof(RendererVKLayout::MeshVertex),
            .maxVertex = b.vertexCount - 1u, // exact: a skinned mesh owns a private contiguous region
            .indexType = vk::IndexType::eUint32,
        };
        tri.vertexData.deviceAddress = vbAddr;
        tri.indexData.deviceAddress = ibAddr;
        tri.transformData.deviceAddress = 0;

        geoms[i] = vk::AccelerationStructureGeometryKHR{ .geometryType = vk::GeometryTypeKHR::eTriangles, .flags = {} };
        geoms[i].geometry.triangles = tri;
        buildInfos[i] = vk::AccelerationStructureBuildGeometryInfoKHR{
            .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
            .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild, // rebuilt every frame
            .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = 1,
            .pGeometries = &geoms[i],
        };
        uint32 maxPrim = primCount;
        const vk::AccelerationStructureBuildSizesInfoKHR sizes = dev.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfos[i], maxPrim);
        scratchOffsets[i] = totalScratch;
        totalScratch += (sizes.buildScratchSize + align - 1) & ~(align - 1);

        // Allocate the BLAS buffer + handle once (geometry size is constant); reused/rebuilt in place after.
        Blas& blas = slot[i];
        if (!blas.handle)
        {
            blas.buffer.initialize(sizes.accelerationStructureSize,
                vk::BufferUsageFlagBits2::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                vk::MemoryPropertyFlagBits::eDeviceLocal, false, "AS.skinnedBlas");
            vk::AccelerationStructureCreateInfoKHR ci{ .buffer = blas.buffer.getBuffer(), .size = sizes.accelerationStructureSize, .type = vk::AccelerationStructureTypeKHR::eBottomLevel };
            auto res = dev.createAccelerationStructureKHR(ci);
            assert(res.result == vk::Result::eSuccess && "Failed to create skinned BLAS");
            blas.handle = res.value;
            vk::AccelerationStructureDeviceAddressInfoKHR ai{ .accelerationStructure = blas.handle };
            m_mappedBlasAddresses[frameIdx][b.meshIdx] = dev.getAccelerationStructureAddressKHR(ai);
            wroteAddresses = true; // fresh slot OR a freed job slot re-created for new geometry
        }

        ranges[i] = vk::AccelerationStructureBuildRangeInfoKHR{
            .primitiveCount = primCount,
            .primitiveOffset = b.firstIndex * (uint32)sizeof(RendererVKLayout::MeshIndex),
            .firstVertex = b.vertexOffset,
            .transformOffset = 0,
        };
    }
    if (wroteAddresses)
        m_blasAddressBuffers[frameIdx].flushMappedMemory(m_blasAddressBuffers[frameIdx].getSize());

    ensureScratch(m_skinnedBlasScratch[frameIdx], m_skinnedBlasScratchAlignedAddr[frameIdx], totalScratch);

    // Pass 2: rebuild each BLAS in place, serialized with a barrier (each uses a disjoint scratch region).
    for (uint32 i = 0; i < count; ++i)
    {
        if (ranges[i].primitiveCount == 0)
            continue; // parked bundle, skipped in pass 1
        buildInfos[i].dstAccelerationStructure = slot[i].handle;
        buildInfos[i].scratchData.deviceAddress = m_skinnedBlasScratchAlignedAddr[frameIdx] + scratchOffsets[i];
        const vk::AccelerationStructureBuildRangeInfoKHR* pRange = &ranges[i];
        cmd.buildAccelerationStructuresKHR(buildInfos[i], pRange);

        vk::MemoryBarrier2 bb{
            .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &bb });
    }
}

bool AccelerationStructure::recordBuildTlas(vk::CommandBuffer cmd, uint32 frameIdx, Buffer& instanceBuffer, uint32 numInstances)
{
    if (numInstances == 0)
        return false;

    vk::Device dev = Globals::device.getDevice();

    vk::AccelerationStructureGeometryInstancesDataKHR inst{ .arrayOfPointers = vk::False };
    inst.data.deviceAddress = instanceBuffer.getDeviceAddress();
    vk::AccelerationStructureGeometryKHR geom{
        .geometryType = vk::GeometryTypeKHR::eInstances,
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };
    geom.geometry.instances = inst;
    vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &geom,
    };

    // Grow (and recreate) the TLAS only when the instance count exceeds the current capacity; otherwise
    // rebuild in place. Capacity is rounded up generously so this is rare. Returns true when the handle
    // changed, so callers that bake it (e.g. the recorded-once AO pass) know to re-record.
    const bool handleChanged = (!m_tlas[frameIdx] || numInstances > m_tlasCapacity[frameIdx]);
    if (handleChanged)
    {
        const uint32 capacity = ((numInstances + 4095u) / 4096u) * 4096u;
        vk::AccelerationStructureBuildSizesInfoKHR sizes = dev.getAccelerationStructureBuildSizesKHR(vk::AccelerationStructureBuildTypeKHR::eDevice, buildInfo, capacity);
        if (m_tlas[frameIdx])
            dev.destroyAccelerationStructureKHR(m_tlas[frameIdx]);
        m_tlasBuffer[frameIdx].initialize(sizes.accelerationStructureSize,
            vk::BufferUsageFlagBits2::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
            vk::MemoryPropertyFlagBits::eDeviceLocal, false, "AS.tlas");
        vk::AccelerationStructureCreateInfoKHR ci{
            .buffer = m_tlasBuffer[frameIdx].getBuffer(),
            .size = sizes.accelerationStructureSize,
            .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        };
        auto res = dev.createAccelerationStructureKHR(ci);
        assert(res.result == vk::Result::eSuccess && "Failed to create TLAS");
        m_tlas[frameIdx] = res.value;
        ensureScratch(m_tlasScratch[frameIdx], m_tlasScratchAlignedAddr[frameIdx], sizes.buildScratchSize);
        m_tlasCapacity[frameIdx] = capacity;
    }

    buildInfo.dstAccelerationStructure = m_tlas[frameIdx];
    buildInfo.scratchData.deviceAddress = m_tlasScratchAlignedAddr[frameIdx];
    vk::AccelerationStructureBuildRangeInfoKHR range{
        .primitiveCount = numInstances,
        .primitiveOffset = 0,
        .firstVertex = 0,
        .transformOffset = 0,
    };
    const vk::AccelerationStructureBuildRangeInfoKHR* pRange = &range;
    cmd.buildAccelerationStructuresKHR(buildInfo, pRange);
    return handleChanged;
}
