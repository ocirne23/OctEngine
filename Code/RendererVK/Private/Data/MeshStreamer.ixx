export module RendererVK:MeshStreamer;

import Core;
import :Layout;

// Mesh data streaming (counterpart of the TextureStreamer): cooked scenes register each source mesh
// (plus its generated LOD levels — they share the vertex range) as a "mesh set" with the byte ranges
// its data occupies in the .vsc cache file. Every frame the renderer notes which sets instances
// referenced; when the resident total exceeds the budget, the least-recently-seen cold sets evict
// (their MeshInfos get indexCount 0, so draws/TLAS writes become no-ops, and their mega-buffer ranges
// return to the free list after NUM_FRAMES_IN_FLIGHT). An evicted set referenced again re-streams from
// the cache on a worker thread and restores its MeshInfos at freshly allocated offsets. Static BLASes
// stay alive across the cycle — they are snapshots of identical geometry, so no rebuild is needed.
//
// Skinned meshes, procedural scenes and directly-imported (non-cooked) scenes never register — their
// data is pinned for the app's lifetime exactly as before.
export class MeshStreamer final
{
public:

    struct LevelDesc
    {
        uint16 meshInfoIdx = UINT16_MAX; // global MeshInfo index of this level
        uint64 indexByteOffset = 0;      // current allocation in the index mega-buffer
        uint32 indexCount = 0;
        uint64 srcIndicesOffset = 0;     // byte offset of the level's uint32 index array in the cache file
    };

    struct MeshSetDesc
    {
        oc::string_view filePath;         // cooked cache file
        uint32 numVertices = 0;
        uint64 vertexByteOffset = 0;       // current allocation in the vertex mega-buffer (all levels share it)
        uint64 srcAttributeOffsets[5] = {};// positions/normals/tangents/bitangents/texCoords (glm::vec3 arrays)
        uint32 numLevels = 0;              // 1 (just the source mesh) + generated LOD levels
        LevelDesc levels[RendererVKLayout::MAX_MESH_LODS];
    };

    bool initialize(); // starts the disk worker; called from Renderer::initialize
    ~MeshStreamer();
    void shutdown();  // stops the worker; from Renderer teardown (after its waitIdle)
    void onGpuIdle(); // device known idle (growth/reload/swapchain recreate): release deferred frees now

    void registerMeshSet(const MeshSetDesc& desc);

    // ObjectContainer teardown: unregisters every set whose levels' MeshInfos fall in the given global
    // range. A resident set's CURRENT mega-buffer ranges (re-streams relocate them, so the container's
    // load-time records are stale) go on the deferred free list; an in-flight re-stream's completion is
    // discarded. Set slots recycle for later registrations.
    void unregisterSets(uint32 firstMeshInfoIdx, uint32 count);

    // Called per instance from the renderNode paths (and per selected LOD level): keeps the owning set
    // warm / requests a re-stream when it was evicted. Thread-safe (relaxed atomic store);
    // unregistered/skinned mesh indices are ignored.
    void noteUse(uint16 meshInfoIdx)
    {
        if (meshInfoIdx < m_setForMeshInfo.size())
        {
            const uint32 setIdx = m_setForMeshInfo[meshInfoIdx];
            if (setIdx != UINT32_MAX)
                oc::atomic_ref<uint32>(m_sets[setIdx].lastSeenFrame).store(m_frameCounter, oc::memory_order_relaxed);
        }
    }

    // True when the set owning this MeshInfo has its data resident (or the mesh is unregistered = pinned).
    // The LOD selector uses it to render the nearest resident level while the desired one streams in.
    bool isResident(uint16 meshInfoIdx) const
    {
        if (meshInfoIdx >= m_setForMeshInfo.size())
            return true;
        const uint32 setIdx = m_setForMeshInfo[meshInfoIdx];
        return setIdx == UINT32_MAX || m_sets[setIdx].state == EState::Resident;
    }

    // Once per frame from Renderer::present() BEFORE stagingManager.update() (so restored mesh data
    // joins this frame's staging batch): drains completions, releases matured frees, issues re-streams
    // for evicted-but-wanted sets, and evicts cold sets while over budget.
    void update();

    void registerTweaks();

    struct Stats
    {
        uint64 budgetBytes;
        uint64 streamableBytes; // total registered (resident or not)
        uint64 residentBytes;   // currently in VRAM
        uint64 coldBytes;       // resident but unseen for >= coldFrames
        uint32 numSets;
        uint32 numEvictedSets;  // evicted or currently streaming back in
    };
    Stats getStats() const { return m_stats; }

private:

    enum class EState : uint8 { Resident, Evicted, StreamingIn };

    struct MeshSet
    {
        uint16 fileId = 0;
        uint16 numLevels = 0;
        EState state = EState::Resident;
        uint32 numVertices = 0;
        uint64 vertexByteOffset = 0;
        uint64 srcAttributeOffsets[5] = {};
        LevelDesc levels[RendererVKLayout::MAX_MESH_LODS];
        uint32 lastSeenFrame = 0;
        uint64 totalBytes = 0; // vertex + all levels' index bytes
        bool failed = false;   // re-stream failed (cache gone/changed): stay evicted, don't retry
        bool removed = false;  // unregistered (container destroyed): inert until the slot recycles
    };

    struct StreamInRequest
    {
        uint32 setIdx = 0;
        oc::unique_ptr<const char[]> filePath; // owned copy (null-terminated); m_files must not be referenced cross-thread
        uint32 numVertices = 0;
        uint64 srcAttributeOffsets[5] = {};
        uint32 numLevels = 0;
        struct { uint64 srcIndicesOffset; uint32 indexCount; } levels[RendererVKLayout::MAX_MESH_LODS] = {};
    };

    struct StreamInCompletion
    {
        uint32 setIdx = 0;
        bool ok = false;
        oc::unique_ptr<uint8[]> vertexData; // interleaved RendererVKLayout::MeshVertex
        oc::unique_ptr<uint8[]> indexData[RendererVKLayout::MAX_MESH_LODS];
    };

    struct DeferredFree
    {
        uint64 offset;
        uint64 size;
        uint32 frame;
        bool isVertex;
    };

    void workerRun(std::stop_token stopToken);
    void drainCompletions();
    void processDeferredFrees(bool releaseAll);
    void issueStreamIns();
    void evictSet(uint32 setIdx);
    void solveEvictions();
    uint16 getFileId(oc::string_view path);

    oc::vector<oc::string> m_files;
    oc::vector<MeshSet> m_sets;
    oc::vector<uint32> m_setForMeshInfo; // global MeshInfo idx -> set idx (UINT32_MAX = pinned)
    oc::vector<uint32> m_freeSetSlots;   // removed sets, recycled by registerMeshSet
    oc::vector<DeferredFree> m_deferredFrees;
    uint32 m_frameCounter = 0;
    uint32 m_opsInFlight = 0;
    Stats m_stats = {};

    std::jthread m_worker;
    std::mutex m_requestMutex;
    std::condition_variable_any m_requestCv;
    oc::deque<StreamInRequest> m_requests;      // guarded by m_requestMutex
    std::mutex m_completionMutex;
    oc::deque<StreamInCompletion> m_completions; // guarded by m_completionMutex

    // Tweaks ("Streaming" category, next to the texture budget)
    int m_budgetMB = 256;
    int m_coldFrames = 240;    // frames a set must go unseen before it may evict
    int m_maxOpsInFlight = 8;  // concurrent re-stream reads
    int m_maxStreamMBPerFrame = 32; // stream-in issue cap (staging pressure)
    bool m_enabled = true;
};

export namespace Globals
{
OC_INIT_SEG(OC_SEG_VK_DATA)
    MeshStreamer meshStreamer;
}
