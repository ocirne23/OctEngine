module;

#include <cstdio> // fopen_s/_fseeki64 on the disk worker (macros don't cross header units)

module RendererVK;

import Core;
import Core.glm;
import Core.Tweaks;
import Core.Log;
import :MeshStreamer;
import :MeshDataManager;
import :Renderer;
import :Layout;

// StreamInRequest carries an owned, null-terminated copy of the cache path (unique_ptr<const char[]>)
// instead of referencing m_files' oc::string across the worker thread.
static oc::unique_ptr<const char[]> copyPathString(const oc::string& path)
{
    oc::unique_ptr<char[]> buffer = oc::make_unique<char[]>(path.size() + 1);
    memcpy(buffer.get(), path.c_str(), path.size() + 1);
    return buffer;
}

bool MeshStreamer::initialize()
{
    registerTweaks();
    m_worker = std::jthread([this](std::stop_token stopToken) { workerRun(stopToken); });
    return true;
}

MeshStreamer::~MeshStreamer()
{
    shutdown();
}

void MeshStreamer::shutdown()
{
    if (m_worker.joinable())
    {
        m_worker.request_stop();
        m_requestCv.notify_all();
        m_worker.join();
    }
    m_requests.clear();
    m_completions.clear();
    m_deferredFrees.clear(); // Renderer teardown: buffers are about to go away wholesale
}

void MeshStreamer::onGpuIdle()
{
    processDeferredFrees(true);
}

void MeshStreamer::registerTweaks()
{
    Tweak::boolean("Mesh Streaming", "Enabled", &m_enabled);
    Tweak::intVar("Mesh Streaming", "Mesh budget (MB)", &m_budgetMB, 64, 8192);
    Tweak::intVar("Mesh Streaming", "Mesh cold frames", &m_coldFrames, 30, 3000);
    Tweak::intVar("Mesh Streaming", "Mesh max ops", &m_maxOpsInFlight, 1, 32);
    Tweak::intVar("Mesh Streaming", "Mesh max MB/frame", &m_maxStreamMBPerFrame, 4, 256);
}

uint16 MeshStreamer::getFileId(oc::string_view path)
{
    for (uint32 i = 0; i < (uint32)m_files.size(); ++i)
        if (m_files[i] == path)
            return (uint16)i;
    m_files.emplace_back(path);
    return (uint16)(m_files.size() - 1);
}

void MeshStreamer::registerMeshSet(const MeshSetDesc& desc)
{
    assert(desc.numLevels >= 1 && desc.numLevels <= RendererVKLayout::MAX_MESH_LODS);

    uint32 setIdx;
    if (!m_freeSetSlots.empty()) // slot of a set unregistered by a destroyed container
    {
        setIdx = m_freeSetSlots.back();
        m_freeSetSlots.pop_back();
        m_sets[setIdx] = MeshSet{};
    }
    else
    {
        m_sets.emplace_back();
        setIdx = (uint32)(m_sets.size() - 1);
    }
    MeshSet& set = m_sets[setIdx];
    set.fileId = getFileId(desc.filePath);
    set.numLevels = (uint16)desc.numLevels;
    set.numVertices = desc.numVertices;
    set.vertexByteOffset = desc.vertexByteOffset;
    memcpy(set.srcAttributeOffsets, desc.srcAttributeOffsets, sizeof(set.srcAttributeOffsets));
    set.lastSeenFrame = m_frameCounter;
    set.totalBytes = (uint64)desc.numVertices * sizeof(RendererVKLayout::MeshVertex);

    for (uint32 k = 0; k < desc.numLevels; ++k)
    {
        set.levels[k] = desc.levels[k];
        set.totalBytes += (uint64)desc.levels[k].indexCount * sizeof(RendererVKLayout::MeshIndex);
        const uint16 meshInfoIdx = desc.levels[k].meshInfoIdx;
        if (meshInfoIdx >= m_setForMeshInfo.size())
            m_setForMeshInfo.resize(meshInfoIdx + 1, UINT32_MAX);
        m_setForMeshInfo[meshInfoIdx] = setIdx;
    }
}

void MeshStreamer::unregisterSets(uint32 firstMeshInfoIdx, uint32 count)
{
    for (uint32 meshInfoIdx = firstMeshInfoIdx; meshInfoIdx < firstMeshInfoIdx + count && meshInfoIdx < (uint32)m_setForMeshInfo.size(); ++meshInfoIdx)
    {
        const uint32 setIdx = m_setForMeshInfo[meshInfoIdx];
        m_setForMeshInfo[meshInfoIdx] = UINT32_MAX;
        if (setIdx == UINT32_MAX)
            continue;
        MeshSet& set = m_sets[setIdx];
        if (set.removed)
            continue; // another level of the same set already processed it
        set.removed = true;
        if (set.state == EState::Resident)
        {
            // Free the CURRENT allocation like an eviction, minus the MeshInfo rewrites — the renderer
            // neutralizes the whole freed range itself (freeMeshInfoRange).
            for (uint32 k = 0; k < set.numLevels; ++k)
                m_deferredFrees.push_back(DeferredFree{
                    .offset = set.levels[k].indexByteOffset,
                    .size = (uint64)set.levels[k].indexCount * sizeof(RendererVKLayout::MeshIndex),
                    .frame = m_frameCounter, .isVertex = false });
            m_deferredFrees.push_back(DeferredFree{
                .offset = set.vertexByteOffset,
                .size = (uint64)set.numVertices * sizeof(RendererVKLayout::MeshVertex),
                .frame = m_frameCounter, .isVertex = true });
        }
        // A slot with a read in flight recycles when its completion drains; the rest can recycle now.
        if (set.state != EState::StreamingIn)
            m_freeSetSlots.push_back(setIdx);
        set.state = EState::Evicted; // keeps stats/eviction passes away from it
        set.totalBytes = 0;
    }
}

// Disk worker: reads a set's attribute + index arrays from the cooked cache and interleaves the vertex
// attributes into the renderer's MeshVertex layout (same math as ObjectContainer's load path), so the
// main thread only has to allocate ranges and hand the buffers to the staging manager.
void MeshStreamer::workerRun(std::stop_token stopToken)
{
    Globals::profiler.registerThread("MeshStreamer", Profiler::SORT_KEY_BACKGROUND + 1);

    ProfileScope dumb("MeshStreamer::workerRun", EProfileCategory::Renderer); // just for attributes[5] debug iterator allocator..
    oc::vector<glm::vec3> attributes[5];
    for (;;)
    {
        StreamInRequest request;
        {
            std::unique_lock lock(m_requestMutex);
            if (!m_requestCv.wait(lock, stopToken, [&] { return !m_requests.empty(); }))
                return; // stop requested
            request = oc::move(m_requests.front());
            m_requests.pop_front();
        }
        ProfileScope profileScope("Mesh re-stream", EProfileCategory::Renderer); // after the cv wait: measure the work, not the idle

        StreamInCompletion completion;
        completion.setIdx = request.setIdx;

        FILE* pFile = nullptr;
        fopen_s(&pFile, request.filePath.get(), "rb");
        if (pFile)
        {
            bool ok = true;
            const uint32 numVertices = request.numVertices;
            for (uint32 a = 0; a < 5 && ok; ++a)
            {
                attributes[a].resize(numVertices);
                ok = _fseeki64(pFile, (int64)request.srcAttributeOffsets[a], SEEK_SET) == 0
                    && fread(attributes[a].data(), sizeof(glm::vec3), numVertices, pFile) == numVertices;
            }
            for (uint32 k = 0; k < request.numLevels && ok; ++k)
            {
                const size_t byteSize = (size_t)request.levels[k].indexCount * sizeof(uint32);
                completion.indexData[k] = oc::unique_ptr<uint8[]>(new uint8[byteSize]);
                ok = _fseeki64(pFile, (int64)request.levels[k].srcIndicesOffset, SEEK_SET) == 0
                    && fread(completion.indexData[k].get(), 1, byteSize, pFile) == byteSize;
            }
            fclose(pFile);

            if (ok)
            {
                completion.vertexData = oc::unique_ptr<uint8[]>(new uint8[(size_t)numVertices * sizeof(RendererVKLayout::MeshVertex)]);
                RendererVKLayout::MeshVertex* pVertices = (RendererVKLayout::MeshVertex*)completion.vertexData.get();
                const glm::vec3* pPositions = attributes[0].data();
                const glm::vec3* pNormals = attributes[1].data();
                const glm::vec3* pTangents = attributes[2].data();
                const glm::vec3* pBitangents = attributes[3].data();
                const glm::vec3* pTexCoords = attributes[4].data();
                for (uint32 i = 0; i < numVertices; ++i)
                {
                    pVertices[i].position = pPositions[i];
                    pVertices[i].normal = pNormals[i];
                    const float handedness = glm::dot(pNormals[i], glm::cross(pTangents[i], pBitangents[i])) >= 0.0f ? 1.0f : -1.0f;
                    pVertices[i].tangent = glm::vec4(pTangents[i], handedness);
                    pVertices[i].texCoord = glm::vec2(pTexCoords[i]);
                }
                completion.ok = true;
            }
        }

        {
            std::scoped_lock lock(m_completionMutex);
            m_completions.push_back(oc::move(completion));
        }
    }
}

void MeshStreamer::drainCompletions()
{
    oc::deque<StreamInCompletion> completed;
    {
        std::scoped_lock lock(m_completionMutex);
        completed.swap(m_completions);
    }
    for (StreamInCompletion& completion : completed)
    {
        m_opsInFlight--;
        MeshSet& set = m_sets[completion.setIdx];
        if (set.removed)
        {
            // Unregistered mid-stream (container destroyed): discard the data, recycle the slot now
            // that the worker can no longer reference it.
            m_freeSetSlots.push_back(completion.setIdx);
            continue;
        }
        if (!completion.ok)
        {
            // Cache file gone/changed mid-run: the set stays evicted (and invisible). It re-cooks on
            // the next app start; don't retry-spam the worker.
            Log::warning(oc::format("MeshStreamer: re-stream from '{}' failed; mesh stays evicted", m_files[set.fileId]));
            set.state = EState::Evicted;
            set.failed = true;
            continue;
        }

        MeshDataManager& meshDataManager = Globals::meshDataManager;
        set.vertexByteOffset = meshDataManager.uploadVertexData(completion.vertexData.get(),
            (size_t)set.numVertices * sizeof(RendererVKLayout::MeshVertex));
        for (uint32 k = 0; k < set.numLevels; ++k)
        {
            const size_t byteSize = (size_t)set.levels[k].indexCount * sizeof(RendererVKLayout::MeshIndex);
            set.levels[k].indexByteOffset = meshDataManager.uploadIndexData(completion.indexData[k].get(), byteSize);
            Globals::rendererVK.setMeshStreamedIn(set.levels[k].meshInfoIdx,
                (int32)(set.vertexByteOffset / sizeof(RendererVKLayout::MeshVertex)),
                (uint32)(set.levels[k].indexByteOffset / sizeof(RendererVKLayout::MeshIndex)),
                set.levels[k].indexCount);
        }
        set.state = EState::Resident;
    }
}

void MeshStreamer::processDeferredFrees(bool releaseAll)
{
    size_t kept = 0;
    for (DeferredFree& deferred : m_deferredFrees)
    {
        if (!releaseAll && m_frameCounter - deferred.frame < RendererVKLayout::NUM_FRAMES_IN_FLIGHT)
        {
            m_deferredFrees[kept++] = deferred;
            continue;
        }
        if (deferred.isVertex)
            Globals::meshDataManager.freeVertexData(deferred.offset, deferred.size);
        else
            Globals::meshDataManager.freeIndexData(deferred.offset, deferred.size);
    }
    m_deferredFrees.resize(kept);
}

void MeshStreamer::issueStreamIns()
{
    if (m_sets.empty())
        return;
    uint64 issuedBytes = 0;
    const uint64 maxBytes = (uint64)m_maxStreamMBPerFrame << 20;
    for (uint32 setIdx = 0; setIdx < (uint32)m_sets.size(); ++setIdx)
    {
        if (m_opsInFlight >= (uint32)m_maxOpsInFlight || issuedBytes >= maxBytes)
            break;
        MeshSet& set = m_sets[setIdx];
        // "Seen this frame while evicted" = an instance wants it back on screen (or in shadows/GI).
        if (set.state != EState::Evicted || set.failed || set.removed || set.lastSeenFrame != m_frameCounter)
            continue;

        StreamInRequest request;
        request.setIdx = setIdx;
        request.filePath = copyPathString(m_files[set.fileId]);
        request.numVertices = set.numVertices;
        memcpy(request.srcAttributeOffsets, set.srcAttributeOffsets, sizeof(request.srcAttributeOffsets));
        request.numLevels = set.numLevels;
        for (uint32 k = 0; k < set.numLevels; ++k)
        {
            request.levels[k].srcIndicesOffset = set.levels[k].srcIndicesOffset;
            request.levels[k].indexCount = set.levels[k].indexCount;
        }
        set.state = EState::StreamingIn;
        m_opsInFlight++;
        issuedBytes += set.totalBytes;
        {
            std::scoped_lock lock(m_requestMutex);
            m_requests.push_back(oc::move(request));
        }
        m_requestCv.notify_one();
    }
}

void MeshStreamer::evictSet(uint32 setIdx)
{
    MeshSet& set = m_sets[setIdx];
    for (uint32 k = 0; k < set.numLevels; ++k)
    {
        Globals::rendererVK.setMeshStreamedOut(set.levels[k].meshInfoIdx);
        m_deferredFrees.push_back(DeferredFree{
            .offset = set.levels[k].indexByteOffset,
            .size = (uint64)set.levels[k].indexCount * sizeof(RendererVKLayout::MeshIndex),
            .frame = m_frameCounter, .isVertex = false });
    }
    m_deferredFrees.push_back(DeferredFree{
        .offset = set.vertexByteOffset,
        .size = (uint64)set.numVertices * sizeof(RendererVKLayout::MeshVertex),
        .frame = m_frameCounter, .isVertex = true });
    set.state = EState::Evicted;
}

void MeshStreamer::solveEvictions()
{
    if (!m_enabled || m_stats.residentBytes <= m_stats.budgetBytes)
        return;

    // Evict least-recently-seen first, cold sets only — an over-budget scene where everything is
    // actively referenced stays over budget rather than flickering meshes in and out.
    oc::vector<uint32> candidates;
    for (uint32 i = 0; i < (uint32)m_sets.size(); ++i)
        if (m_sets[i].state == EState::Resident && m_frameCounter - m_sets[i].lastSeenFrame >= (uint32)m_coldFrames)
            candidates.push_back(i);
    oc::sort(candidates.begin(), candidates.end(),
        [&](uint32 a, uint32 b) { return m_sets[a].lastSeenFrame < m_sets[b].lastSeenFrame; });

    uint64 residentBytes = m_stats.residentBytes;
    for (uint32 setIdx : candidates)
    {
        if (residentBytes <= m_stats.budgetBytes)
            break;
        evictSet(setIdx);
        residentBytes -= m_sets[setIdx].totalBytes;
        m_stats.coldBytes -= oc::min(m_stats.coldBytes, m_sets[setIdx].totalBytes);
    }
    m_stats.residentBytes = residentBytes;
    m_stats.numEvictedSets = 0;
    for (const MeshSet& set : m_sets)
        m_stats.numEvictedSets += !set.removed && set.state != EState::Resident;
}

void MeshStreamer::update()
{
    drainCompletions();
    processDeferredFrees(false);
    issueStreamIns();

    Stats stats = {};
    stats.budgetBytes = (uint64)m_budgetMB << 20;
    for (const MeshSet& set : m_sets)
    {
        if (set.removed)
            continue;
        stats.numSets++;
        stats.streamableBytes += set.totalBytes;
        if (set.state != EState::Resident)
        {
            stats.numEvictedSets++;
            continue;
        }
        stats.residentBytes += set.totalBytes;
        if (m_frameCounter - set.lastSeenFrame >= (uint32)m_coldFrames)
            stats.coldBytes += set.totalBytes;
    }
    m_stats = stats;

    solveEvictions();

    m_frameCounter++;
}
