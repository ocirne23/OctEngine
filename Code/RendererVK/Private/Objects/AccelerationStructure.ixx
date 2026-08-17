export module RendererVK:AccelerationStructure;

import Core;
import :VK;
import :Buffer;
import :Layout;

// Hardware ray-tracing acceleration structures for GI. One bottom-level AS (BLAS) per unique mesh, built
// straight from the shared vertex/index mega-buffers; a single top-level AS (TLAS) rebuilt every frame
// from a GPU-written instance buffer. The TLAS is ray-queried by the GI probe-trace compute shader.
export class AccelerationStructure final
{
public:
    AccelerationStructure();
    ~AccelerationStructure();
    AccelerationStructure(const AccelerationStructure&) = delete;

    void initialize(uint32 maxUniqueMeshes);
    // Grows the BLAS-address buffer for a larger unique-mesh capacity, preserving its contents
    // (GPU must be idle).
    void resizeBlasAddressBuffer(uint32 maxUniqueMeshes);

    // RT mesh aliasing: a LOD chain shares ONE BLAS (rays don't need per-level fidelity), so every level
    // aliases the chain's RT level. Aliased meshes never build their own BLAS; their address entries are
    // filled from the alias target's, and the TLAS-instance writer packs the aliased meshIdx into each
    // instance's sbtOffset so hit shaders fetch the matching geometry. Grown identity-filled by
    // setNumMeshes (Renderer::addMeshInfos); addMeshLodGroup points chain levels at the RT level.
    void setNumMeshes(uint32 numMeshes);
    void setMeshAlias(uint32 meshIdx, uint32 aliasIdx);
    uint32 getMeshAlias(uint32 meshIdx) const { return meshIdx < m_numAliasMeshes ? m_mappedMeshAlias[meshIdx] : meshIdx; }
    Buffer& getMeshAliasBuffer() { return m_meshAliasBuffer; }

    // Mesh streaming eviction: zeroes the mesh's BLAS-address entries (the TLAS writer masks null-BLAS
    // instances) and, when the mesh owns the BLAS (self-aliased), destroys it and zeroes every entry
    // aliased to it. Only called for sets cold long enough that no in-flight TLAS references the BLAS.
    void onMeshEvicted(uint32 meshIdx);

    // Container teardown (Renderer::freeMeshInfoRange): zeroes the freed meshes' address entries in
    // every frame slot, resets their aliases to identity (slot reuse expects it) and RETIRES self-owned
    // static BLASes instead of destroying them — unlike streaming eviction there is no cold-frames
    // guarantee here, an in-flight TLAS may still reference them. recordCompaction destroys retirees
    // once the frames-in-flight window has passed (matured compaction batches skip them by handle compare).
    void onMeshRangeFreed(uint32 firstMeshIdx, uint32 count);

    // Skinned-bundle teardown: retires the double-buffered per-job-slot skinned BLASes so a later reuse
    // of the job slots (different geometry, different size) lazily re-creates them.
    void freeSkinnedJobSlots(uint32 firstJob, uint32 count);

    // Records BLAS builds for the given meshes into cmd, reading geometry directly from the shared
    // vertex/index buffers with each mesh's exact vertex count (maxVertex must be tight — see the
    // comment in the implementation). Aliased and streamed-out (indexCount 0) meshes are skipped; the
    // address-fill pass afterwards refreshes every aliased mesh's entry from its target.
    // allowCompaction: builds carry eAllowCompaction and record a compacted-size query for
    // recordCompaction to consume.
    void recordBuildBlas(uint32 frameIdx, vk::CommandBuffer cmd, Buffer& vertexBuffer, Buffer& indexBuffer,
        const RendererVKLayout::MeshInfo* meshInfos, const uint32* vertexCounts, oc::span<const uint32> meshIndices,
        bool allowCompaction);

    // BLAS compaction: build-size quotes are conservative worst cases, so freshly built BLASes waste
    // 30-50% of their memory. Once a build batch's compacted-size query is fence-safe to read
    // (NUM_FRAMES_IN_FLIGHT recordCompaction calls later), each BLAS is copied into a right-sized buffer
    // (vkCmdCopyAccelerationStructureKHR eCompact), its address entries are repatched (the per-frame
    // TLAS rebuild picks the new BLAS up the same frame), and the original retires after another
    // frames-in-flight window. BLASes rebuilt or evicted in the meantime are skipped by handle compare.
    // Call once per frame, after the build step and before the TLAS build.
    void recordCompaction(uint32 frameIdx, vk::CommandBuffer cmd);

    // Flushes this frame slot's pending static BLAS-address changes into its (fenced) address buffer.
    // Static addresses only ever change from CPU here, in the owning frame — never on an in-flight slot.
    // Call every frame after recordBuildBlas + recordCompaction and before the TLAS-instance write.
    void syncFrameAddresses(uint32 frameIdx);

    uint64 getBlasTotalBytes() const; // current static BLAS memory (excludes skinned + retiring originals)
    uint64 getCompactionSavedBytes() const { return m_compactionSavedBytes; }

    // A skinned mesh's deformed output region. vertexCount/firstIndex/indexCount are known exactly, so the
    // BLAS is built without relying on neighbouring mesh offsets.
    struct SkinnedBlasBuild
    {
        uint32 meshIdx;       // index into the per-mesh BLAS-address buffer
        uint32 vertexOffset;  // MeshVertex units, into the shared vertex buffer
        uint32 vertexCount;
        uint32 firstIndex;
        uint32 indexCount;
    };

    // Rebuilds the skinned meshes' BLASes from their (this-frame) deformed vertices. Double-buffered per
    // frame-in-flight (the previous frame's TLAS may still reference the other slot), and their addresses
    // are written into that frame's BLAS-address buffer. BLAS buffers are allocated once (geometry size is
    // constant) and rebuilt in place each frame. Call after the skinning compute, before the TLAS build.
    void recordBuildSkinnedBlas(vk::CommandBuffer cmd, uint32 frameIdx, Buffer& vertexBuffer, Buffer& indexBuffer,
        oc::span<const SkinnedBlasBuild> builds);

    // Records a per-frame TLAS (re)build from numInstances pre-written VkAccelerationStructureInstanceKHR
    // records in instanceBuffer. The TLAS is double-buffered (one per frame-in-flight): it is rebuilt
    // every frame and ray-queried in the same frame, so a single shared TLAS would be write-after-read
    // raced by the next frame's rebuild (pipelined across submits on one queue) -> device lost.
    // Returns true when the TLAS handle changed (first build / capacity growth).
    bool recordBuildTlas(vk::CommandBuffer cmd, uint32 frameIdx, Buffer& instanceBuffer, uint32 numInstances);

    // mesh idx -> BLAS device address (uint64), consumed by the TLAS-instance compute shader. Per frame
    // in flight: skinned meshes have a different BLAS per slot, so the address differs between frames.
    Buffer& getBlasAddressBuffer(uint32 frameIdx) { return m_blasAddressBuffers[frameIdx]; }
    uint32 getNumBlas() const { return m_numBlas; }

    vk::AccelerationStructureKHR getTlas(uint32 frameIdx) const { return m_tlas[frameIdx]; }

private:
    void ensureScratch(Buffer& scratch, vk::DeviceAddress& outAlignedAddr, vk::DeviceSize needed);

    // Records a static BLAS address change: updates the CPU authoritative value and queues the write into
    // every frame slot's dirty list (each slot applies it in its own fenced frame via syncFrameAddresses).
    void markStaticBlasAddr(uint32 meshIdx, uint64 addr);

    struct Blas
    {
        vk::AccelerationStructureKHR handle = nullptr;
        Buffer buffer;
        bool compacted = false; // right-sized already (or not worth it); reset on rebuild
    };
    oc::vector<Blas> m_blasList;

    // Compaction pipeline state: one query batch per build event, matured by recordCompaction-call
    // count (the frame-slot fence guarantees the queries executed by then); originals retire the same way.
    struct CompactionBatch
    {
        vk::QueryPool queryPool;
        oc::vector<uint32> meshIndices;
        oc::vector<vk::AccelerationStructureKHR> handles; // as recorded; mismatch = rebuilt/evicted, skip
        uint32 frameStamp;
    };
    oc::deque<CompactionBatch> m_compactionBatches;
    struct RetiredBlas
    {
        vk::AccelerationStructureKHR handle;
        Buffer buffer;
        uint32 frameStamp;
    };
    oc::vector<RetiredBlas> m_retiredBlas;
    uint64 m_compactionSavedBytes = 0;
    uint32 m_compactionFrame = 0;
    // Per frame in flight: static BLAS builds run every frame under mesh streaming, so a shared scratch
    // could be reallocated (ensureScratch grows it) while an earlier in-flight frame's build still reads
    // it -> device lost. Per-slot scratch (like the TLAS/skinned scratch below) is fenced by beginFrame.
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_blasScratch;
    oc::array<vk::DeviceAddress, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_blasScratchAlignedAddr{};
    // Per frame in flight (skinned BLAS addresses differ between slots; static addresses are the same, but
    // each slot is written only in its own fenced frame — see the authoritative array below).
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_blasAddressBuffers;
    oc::array<oc::span<uint64>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_mappedBlasAddresses;

    // CPU authoritative static BLAS addresses + per-slot pending sync (mirrors the render-node transform
    // dirty pattern). Writing an in-flight slot's mapped buffer races its TLAS-instance compute AND can
    // publish a compacted address whose copy hasn't executed in that frame -> device lost; so all static
    // address changes land here and each frame drains only its own slot in syncFrameAddresses.
    oc::vector<uint64> m_staticBlasAddr;
    oc::vector<uint8>  m_staticAddrDirtyBits;
    oc::array<oc::vector<uint32>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_staticAddrDirtyLists;
    uint32 m_numBlas = 0;

    // mesh idx -> mesh idx whose BLAS it uses (identity = owns one). Single-buffered: entries are static
    // per mesh and written before the mesh can first be instanced.
    Buffer m_meshAliasBuffer;
    oc::span<uint32> m_mappedMeshAlias;
    uint32 m_numAliasMeshes = 0;

    // Double-buffered skinned BLASes (rebuilt every frame) + per-frame scratch. Allocated lazily/in place.
    oc::array<oc::vector<Blas>, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_skinnedBlas;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_skinnedBlasScratch;
    oc::array<vk::DeviceAddress, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_skinnedBlasScratchAlignedAddr{};

    // Double-buffered TLAS (one per frame-in-flight) to avoid a cross-frame WAR hazard on the structure.
    oc::array<vk::AccelerationStructureKHR, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tlas{};
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tlasBuffer;
    oc::array<Buffer, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tlasScratch;
    oc::array<vk::DeviceAddress, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tlasScratchAlignedAddr{};
    oc::array<uint32, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_tlasCapacity{};

    uint32 m_scratchAlignment = 256;
};
