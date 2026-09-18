module RendererVK;

import Core;
import Core.glm;
import File;
import :Allocator;
import :Layout;

namespace
{
    // Mirrors of light_grid.inc.glsl / hash_grid.inc.glsl / shared.inc.glsl: keep in sync.
    constexpr uint32 GRID_SIZE = 32;
    constexpr uint32 MAX_LARGE_LIGHTS_PER_GRID = 14;
    constexpr uint32 MAX_LIGHTCELL_LIGHTS = 16;
    constexpr uint32 GRID_HEADER_SIZE = 4 + (MAX_LARGE_LIGHTS_PER_GRID / 2 + 1);
    constexpr uint32 CELL_STRIDE = MAX_LIGHTCELL_LIGHTS / 2 + 1;
    constexpr uint32 EMPTY_ENTRY = 0xFFFFFFFFu;
    constexpr uint32 GATHER_GROUP = 64;        // LIGHT_GRID_GATHER_GROUP
    constexpr uint32 LARGE_BIT = 0x80000000u;
    constexpr uint32 SKIPPED_TOUCH = 0xFFFFFFFFu;
    constexpr uint32 INVALID_SLOT = 0xFFFFFFFFu;
    constexpr uint32 CULL_VEC4S_PER_LIGHT = 3;
    constexpr int MAX_GRIDS_PER_AXIS = 32;     // a kilometre-scale light walks 32^3 grids at most
    constexpr uint32 INITIAL_TOUCHES = 65536;

    // getPositionHash in hash_grid.inc.glsl, bit for bit (uint32 wraparound == GLSL uint).
    uint32 positionHash(const glm::ivec3& p)
    {
        const uint32 x = uint32(p.x) * 1597334673u;
        const uint32 y = uint32(p.y) * 3812015801u;
        const uint32 z = uint32(p.z) * 2798796415u;
        uint32 n = x ^ y ^ z;
        n = n * 747796405u + 2891336453u;
        n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
        n = (n >> 22u) ^ n;
        return n;
    }

    glm::ivec3 gridPosOf(const glm::vec3& pos)
    {
        return glm::ivec3(glm::floor(pos / float(GRID_SIZE)));
    }

    // Distance LOD: level = pow(max(dist - start, 0) / step, power), cellSize = clamp(minCell << level, minCell, maxCell).
    uint32 lodCellSize(const glm::ivec3& gridPos, const glm::vec3& viewPos, const LightGridParams& params)
    {
        const float viewDist = glm::distance(glm::vec3(gridPos) * float(GRID_SIZE) + float(GRID_SIZE / 2), viewPos);
        const float t = glm::max(viewDist - glm::max(params.lodStart, 0.0f), 0.0f) / glm::max(params.lodStep, 0.01f);
        const float power = glm::max(params.lodPower, 0.01f);
        const float level = glm::abs(power - 0.5f) < 1e-3f ? std::sqrt(t) : glm::abs(power - 1.0f) < 1e-3f ? t : std::pow(t, power);
        const int minLog2 = oc::clamp(params.minCellLog2, 0, 5);
        const int maxLog2 = oc::clamp(params.maxCellLog2, minLog2, 5);
        const uint32 cellSize = (1u << minLog2) << uint32(glm::min(level, 8.0f));
        return oc::clamp(cellSize, 1u << minLog2, 1u << maxLog2);
    }

    uint32 gridMemoryUsage(uint32 cellSize)
    {
        const uint32 numCells = GRID_SIZE / cellSize;
        return GRID_HEADER_SIZE + numCells * numCells * numCells * CELL_STRIDE;
    }

    struct LightBounds
    {
        glm::vec3 boxMin, boxMax;
        float sphereRadius; // > 0: point / spot range sphere, cells outside it are skipped
        float reach;        // conservative radius for the large-light threshold
    };

    // The per-type bounds, as the old one-thread-per-light shader computed them.
    LightBounds computeBounds(const RendererVKLayout::LightInfo& light)
    {
        LightBounds b;
        float reach = glm::abs(light.radius);
        b.boxMin = light.pos - glm::vec3(reach);
        b.boxMax = light.pos + glm::vec3(reach);
        b.sphereRadius = reach;
        if (light.width > 0.0f && light.radius < 0.0f)
        {
            // Tube light: capsule along the axis, bound as the two end-cap spheres of radius + absRange.
            b.sphereRadius = 0.0f;
            const float height = glm::length(light.direction);
            const float halfLen = height * 0.5f;
            const glm::vec3 axis = light.direction / height;
            const float pad = light.width - light.radius;
            reach = halfLen + pad;
            const glm::vec3 pa = light.pos - axis * halfLen;
            const glm::vec3 pb = light.pos + axis * halfLen;
            b.boxMin = glm::min(pa, pb) - glm::vec3(pad);
            b.boxMax = glm::max(pa, pb) + glm::vec3(pad);
        }
        else if (light.width > 0.0f)
        {
            // Area light: the front-facing influence box, in the same right/up/normal frame as shading.
            b.sphereRadius = 0.0f;
            const float height = glm::length(light.direction);
            const glm::vec3 up = light.direction / height;
            const glm::vec3 ref = glm::abs(up.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 right0 = glm::normalize(glm::cross(up, ref));
            const glm::vec3 right = right0 * std::cos(light.rotation) + glm::cross(up, right0) * std::sin(light.rotation);
            const glm::vec3 normal = glm::cross(up, right);
            reach += 0.5f * std::sqrt(light.width * light.width + height * height);
            const float heRight = light.width * 0.5f + light.radius;
            const float heUp = height * 0.5f + light.radius;
            const glm::vec3 boxCenter = light.pos + normal * (light.radius * 0.5f);
            const glm::vec3 extent = heRight * glm::abs(right) + heUp * glm::abs(up) + (light.radius * 0.5f) * glm::abs(normal);
            b.boxMin = boxCenter - extent;
            b.boxMax = boxCenter + extent;
        }
        else if (light.width < 0.0f)
        {
            // Spot light: the cone's spherical sector (apex + base disk + axial cap tip).
            const glm::vec3 axis = glm::normalize(light.direction);
            const glm::vec3 tip = light.pos + axis * light.radius;
            const glm::vec3 c = light.pos + axis * (light.radius * std::cos(light.rotation));
            const glm::vec3 e = (light.radius * std::sin(light.rotation)) * glm::sqrt(glm::max(glm::vec3(1.0f) - axis * axis, glm::vec3(0.0f)));
            b.boxMin = glm::min(light.pos, glm::min(tip, c - e));
            b.boxMax = glm::max(light.pos, glm::max(tip, c + e));
        }
        b.reach = reach;
        return b;
    }

    uint32 nextPow2(uint32 v)
    {
        uint32 p = 1;
        while (p < v)
            p *= 2;
        return p;
    }
}

void LightGridComputePipeline::initialize()
{
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.inIndirectCommandBuffer.initialize(sizeof(vk::DispatchIndirectCommand),
            vk::BufferUsageFlagBits2::eIndirectBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "LightGridCmd", BufferHostAccess::eSequentialWrite);
        perFrame.mappedIndirectCommands = perFrame.inIndirectCommandBuffer.mapMemory<vk::DispatchIndirectCommand>();
        perFrame.mappedIndirectCommands[0] = vk::DispatchIndirectCommand{ .x = 0, .y = 1, .z = 1 };
        perFrame.inIndirectCommandBuffer.flushMappedMemory(vk::WholeSize);

        perFrame.inLightCullBuffer.initialize(sizeof(glm::vec4) * CULL_VEC4S_PER_LIGHT * RendererVKLayout::MAX_LIGHTS,
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "LightCull", BufferHostAccess::eSequentialWrite);
        perFrame.mappedLightCull = perFrame.inLightCullBuffer.mapMemory<glm::vec4>();
    }
    createHostBuffers();
    createClaimTables();
    m_touches.resize(INITIAL_TOUCHES);
    m_overflowLights.resize(RendererVKLayout::MAX_LIGHTS);

    ComputePipelineLayout computePipelineLayout;
    buildComputeLayout(computePipelineLayout);
    m_computePipeline.initialize(computePipelineLayout);
}

void LightGridComputePipeline::createHostBuffers()
{
    for (PerFrameData& perFrame : m_perFrameData)
    {
        perFrame.inGridJobsBuffer.initialize(sizeof(GridJob) * m_gridCapacity,
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "LightGridJobs", BufferHostAccess::eSequentialWrite);
        perFrame.mappedGridJobs = perFrame.inGridJobsBuffer.mapMemory<GridJob>();
        perFrame.inWorkgroupsBuffer.initialize(sizeof(uint32) * 2 * m_workgroupCapacity,
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "LightGridWorkgroups", BufferHostAccess::eSequentialWrite);
        perFrame.mappedWorkgroups = perFrame.inWorkgroupsBuffer.mapMemory<uint32>();
        perFrame.inLightListBuffer.initialize(sizeof(uint32) * m_lightListCapacity,
            vk::BufferUsageFlagBits2::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible, false, "LightGridLightList", BufferHostAccess::eSequentialWrite);
        perFrame.mappedLightList = perFrame.inLightListBuffer.mapMemory<uint32>();
    }
}

void LightGridComputePipeline::createClaimTables()
{
    m_grids.resize(m_gridCapacity);
    m_cellCounts.resize(m_gridCapacity);
    m_largeCounts.resize(m_gridCapacity);
    m_cellCursor.resize(m_gridCapacity);
    m_largeCursor.resize(m_gridCapacity);
    m_claimTable.assign(m_gridCapacity * 4, EMPTY_ENTRY);
}

bool LightGridComputePipeline::hostBuffersFit(const Demand& demand) const
{
    return demand.numGrids <= m_gridCapacity && demand.numWorkgroups <= m_workgroupCapacity && demand.lightListSize <= m_lightListCapacity;
}

void LightGridComputePipeline::resizeHostBuffers(const Demand& demand)
{
    while (m_gridCapacity < demand.numGrids)
        m_gridCapacity *= 2;
    while (m_workgroupCapacity < demand.numWorkgroups)
        m_workgroupCapacity *= 2;
    while (m_lightListCapacity < demand.lightListSize)
        m_lightListCapacity *= 2;
    createHostBuffers(); // rewritten every upload(): nothing to preserve
    // The claim state is consumed (the frame's build is done, nothing adds until the next beginFrame).
    createClaimTables();
}

void LightGridComputePipeline::reloadShaders()
{
    ComputePipelineLayout computePipelineLayout;
    buildComputeLayout(computePipelineLayout);
    if (!m_computePipeline.reloadShaders(computePipelineLayout))
        printf("LightGridComputePipeline: shader reload failed, keeping previous pipeline\n");
}

void LightGridComputePipeline::buildComputeLayout(ComputePipelineLayout& computePipelineLayout)
{
    computePipelineLayout.computeShaderDebugFilePath = "Shaders/light_grid.cs.glsl";
    computePipelineLayout.computeShaderText = FileSystem::readFileStr(computePipelineLayout.computeShaderDebugFilePath);
    auto& descriptorSetBindings = computePipelineLayout.descriptorSetLayoutBindings;
    descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{ // UBO (shared.inc.glsl)
        .binding = 0,
        .descriptorType = vk::DescriptorType::eUniformBuffer,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    });
    for (uint32 binding = 1; binding <= 5; ++binding) // light cull, grid jobs, workgroups, light list, out grid data
    {
        descriptorSetBindings.push_back(vk::DescriptorSetLayoutBinding{
            .binding = binding,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute
        });
    }
}

// ---------------------------------------------------------------------------------------------
// 1. The inline, per-light phase (any thread).

void LightGridComputePipeline::beginFrame(uint32 frameIdx, const LightGridParams& params, const glm::vec3& viewPos)
{
    m_frameIdx = frameIdx;
    m_params = params;
    m_viewPos = viewPos;
    m_gridCursor.store(0, oc::memory_order_relaxed);
    m_touchCursor.store(0, oc::memory_order_relaxed);
    m_overflowCursor.store(0, oc::memory_order_relaxed);
    memset(m_claimTable.data(), 0xFF, m_claimTable.size() * sizeof(uint32));
    memset(m_cellCounts.data(), 0, m_cellCounts.size() * sizeof(uint32));
    memset(m_largeCounts.data(), 0, m_largeCounts.size() * sizeof(uint32));
}

LightGridComputePipeline::LightWalk LightGridComputePipeline::walkOf(uint32 lightIdx, const RendererVKLayout::LightInfo& light)
{
    LightWalk w;
    const LightBounds b = computeBounds(light);
    // A NaN bound (a zero-length area/tube axis) would walk forever: skip the light.
    const glm::vec3 boxSpan = b.boxMax - b.boxMin;
    w.valid = std::isfinite(boxSpan.x + boxSpan.y + boxSpan.z + b.boxMin.x + b.boxMin.y + b.boxMin.z);
    if (!w.valid)
        return w;
    PerFrameData& frameData = m_perFrameData[m_frameIdx];
    frameData.mappedLightCull[lightIdx * CULL_VEC4S_PER_LIGHT + 0] = glm::vec4(b.boxMin, b.sphereRadius);
    frameData.mappedLightCull[lightIdx * CULL_VEC4S_PER_LIGHT + 1] = glm::vec4(b.boxMax, 0.0f);
    frameData.mappedLightCull[lightIdx * CULL_VEC4S_PER_LIGHT + 2] = glm::vec4(light.pos, 0.0f);

    w.gridMin = gridPosOf(b.boxMin);
    w.gridMax = glm::min(gridPosOf(b.boxMax), w.gridMin + (MAX_GRIDS_PER_AXIS - 1));
    w.boxMinCell = glm::ivec3(glm::floor(b.boxMin));
    w.boxMaxCell = glm::ivec3(glm::floor(b.boxMax));
    w.largeEverywhere = b.reach > float(GRID_SIZE / 2);
    return w;
}

bool LightGridComputePipeline::isLargeIn(const LightWalk& walk, const glm::ivec3& gridPos, uint32 cellSize) const
{
    if (walk.largeEverywhere)
        return true;
    // The cells this light's box spans INSIDE this grid, at this grid's resolution.
    const glm::ivec3 gridMinW = gridPos * int(GRID_SIZE);
    const glm::ivec3 minCell = glm::max(walk.boxMinCell - gridMinW, glm::ivec3(0)) / int(cellSize);
    const glm::ivec3 maxCell = glm::min(walk.boxMaxCell - gridMinW, glm::ivec3(int(GRID_SIZE) - 1)) / int(cellSize);
    const glm::ivec3 span = maxCell - minCell + 1;
    return span.x * span.y * span.z > glm::max(m_params.cellBudget, 1);
}

uint32 LightGridComputePipeline::claimGrid(const glm::ivec3& gridPos)
{
    const uint32 mask = (uint32)m_claimTable.size() - 1;
    uint32 idx = positionHash(gridPos) & mask;
    uint32 mySlot = INVALID_SLOT; // allocated lazily, on the first EMPTY probe
    while (true)
    {
        oc::atomic_ref<uint32> entry(m_claimTable[idx]);
        uint32 seen = entry.load(oc::memory_order_acquire);
        if (seen == EMPTY_ENTRY)
        {
            if (mySlot == INVALID_SLOT)
            {
                mySlot = m_gridCursor.fetch_add(1, oc::memory_order_relaxed);
                if (mySlot >= m_gridCapacity)
                    return INVALID_SLOT; // the cursor keeps counting: build reads the true demand
                GridJob& g = m_grids[mySlot];
                g.pos = gridPos;
                g.cellSize = lodCellSize(gridPos, m_viewPos, m_params);
            }
            if (entry.compare_exchange_strong(seen, mySlot, oc::memory_order_acq_rel, oc::memory_order_acquire))
                return mySlot;
            // Lost the race for this table entry: `seen` now holds the winner, fall through.
        }
        if (m_grids[seen].pos == gridPos)
        {
            if (mySlot != INVALID_SLOT)
                m_grids[mySlot].cellSize = 0; // dead slot: another thread claimed this grid first
            return seen;
        }
        idx = (idx + 1) & mask;
    }
}

void LightGridComputePipeline::touchGrids(const LightWalk& walk, uint32 lightIdx, Touch* out)
{
    for (int x = walk.gridMin.x; x <= walk.gridMax.x; ++x)
    {
        for (int y = walk.gridMin.y; y <= walk.gridMax.y; ++y)
        {
            for (int z = walk.gridMin.z; z <= walk.gridMax.z; ++z)
            {
                const glm::ivec3 gridPos(x, y, z);
                const uint32 slot = claimGrid(gridPos);
                if (slot == INVALID_SLOT)
                {
                    out->slot = INVALID_SLOT; // the caller retracts the whole block
                    return;
                }
                const bool large = isLargeIn(walk, gridPos, m_grids[slot].cellSize);
                oc::atomic_ref<uint32>(large ? m_largeCounts[slot] : m_cellCounts[slot]).fetch_add(1, oc::memory_order_relaxed);
                *out++ = Touch{ .slot = slot, .light = lightIdx | (large ? LARGE_BIT : 0u) };
            }
        }
    }
}

void LightGridComputePipeline::addLight(uint32 lightIdx, const RendererVKLayout::LightInfo& light)
{
    const LightWalk walk = walkOf(lightIdx, light);
    if (!walk.valid)
        return;
    const glm::ivec3 span = walk.gridMax - walk.gridMin + 1;
    const uint32 numTouches = uint32(span.x * span.y * span.z);
    const uint32 begin = m_touchCursor.fetch_add(numTouches, oc::memory_order_relaxed);
    if (begin + numTouches > (uint32)m_touches.size())
    {
        m_overflowLights[m_overflowCursor.fetch_add(1, oc::memory_order_relaxed)] = lightIdx;
        return;
    }
    Touch* block = m_touches.data() + begin;
    block[0].slot = 0;
    touchGrids(walk, lightIdx, block);
    // The block is only ever read by build(), after the last add: no publish needed beyond the
    // counter reads there. A grid-capacity miss retracts the block (build re-walks the light) - the
    // counts it already bumped are corrected by the retraction too.
    bool retracted = false;
    for (uint32 i = 0; i < numTouches; ++i)
    {
        if (block[i].slot == INVALID_SLOT || retracted)
        {
            retracted = true;
            block[i] = Touch{ .slot = 0, .light = SKIPPED_TOUCH };
        }
    }
    if (retracted)
    {
        for (uint32 i = 0; i < numTouches && block[i].light != SKIPPED_TOUCH; ++i)
        {
            const bool large = (block[i].light & LARGE_BIT) != 0;
            oc::atomic_ref<uint32>(large ? m_largeCounts[block[i].slot] : m_cellCounts[block[i].slot]).fetch_sub(1, oc::memory_order_relaxed);
            block[i] = Touch{ .slot = 0, .light = SKIPPED_TOUCH };
        }
        m_overflowLights[m_overflowCursor.fetch_add(1, oc::memory_order_relaxed)] = lightIdx;
    }
}

// ---------------------------------------------------------------------------------------------
// 2. The merge (a job, after the last addLight).

LightGridComputePipeline::Demand LightGridComputePipeline::build(oc::span<const RendererVKLayout::LightInfo> lights)
{
    ProfileScope scope("Light grid build", EProfileCategory::Renderer);
    const uint32 touchDemand = m_touchCursor.load(oc::memory_order_acquire);
    const uint32 numTouches = glm::min(touchDemand, (uint32)m_touches.size());
    const uint32 gridDemand = m_gridCursor.load(oc::memory_order_acquire);
    m_numGrids = glm::min(gridDemand, m_gridCapacity);

    // The overflow lights, serially (the same claim path; a capacity miss now drops the light for
    // this frame - the demand below grows everything for the next one).
    m_extraTouches.clear();
    const uint32 numOverflow = m_overflowCursor.load(oc::memory_order_acquire);
    for (uint32 i = 0; i < numOverflow; ++i)
    {
        const uint32 lightIdx = m_overflowLights[i];
        const LightWalk walk = walkOf(lightIdx, lights[lightIdx]);
        if (!walk.valid)
            continue;
        const glm::ivec3 span = walk.gridMax - walk.gridMin + 1;
        const size_t first = m_extraTouches.size();
        m_extraTouches.resize(first + size_t(span.x * span.y * span.z));
        m_extraTouches[first].slot = 0;
        touchGrids(walk, lightIdx, m_extraTouches.data() + first);
        for (size_t t = first; t < m_extraTouches.size(); ++t)
        {
            if (m_extraTouches[t].slot == INVALID_SLOT)
            {
                // Retract what this light bumped (an in-order prefix of its block) and drop it.
                for (size_t u = first; u < t; ++u)
                {
                    const bool large = (m_extraTouches[u].light & LARGE_BIT) != 0;
                    --(large ? m_largeCounts[m_extraTouches[u].slot] : m_cellCounts[m_extraTouches[u].slot]);
                }
                m_extraTouches.resize(first);
                break;
            }
        }
    }
    m_numGrids = glm::min(m_gridCursor.load(oc::memory_order_relaxed), m_gridCapacity);
    if (touchDemand > (uint32)m_touches.size())
        m_touches.resize(nextPow2(touchDemand + touchDemand / 2)); // consumed below before anything adds again

    // Prefix sums over the slots: per-grid list ranges, data offsets, workgroup counts.
    uint32 listSize = 0;
    uint32 dataOffset = 0;
    uint32 numWorkgroups = 0;
    for (uint32 slot = 0; slot < m_numGrids; ++slot)
    {
        GridJob& g = m_grids[slot];
        g.lightCount = m_cellCounts[slot];
        g.largeCount = m_largeCounts[slot];
        g.lightBegin = listSize;
        listSize += g.lightCount;
        g.largeBegin = listSize;
        listSize += g.largeCount;
        m_cellCursor[slot] = g.lightBegin;
        m_largeCursor[slot] = g.largeBegin;
        g.dataOffset = dataOffset;
        if (g.cellSize == 0) // dead slot
            continue;
        dataOffset += gridMemoryUsage(g.cellSize);
        const uint32 numCells = GRID_SIZE / g.cellSize;
        numWorkgroups += (numCells * numCells * numCells + GATHER_GROUP - 1) / GATHER_GROUP;
    }

    // The stable scatter into per-grid [cell candidates..., large lights...] ranges.
    m_lightList.resize(listSize);
    const auto scatter = [&](const Touch& t)
    {
        if (t.light == SKIPPED_TOUCH)
            return;
        uint32& cursor = (t.light & LARGE_BIT) ? m_largeCursor[t.slot] : m_cellCursor[t.slot];
        m_lightList[cursor++] = t.light & ~LARGE_BIT;
    };
    for (uint32 i = 0; i < numTouches; ++i)
        scatter(m_touches[i]);
    for (const Touch& t : m_extraTouches)
        scatter(t);

    // List order is the claim order (a thread race). It only matters past the per-cell cap, which
    // is a deliberate non-goal.
    m_workgroups.resize(numWorkgroups * 2);
    uint32 wg = 0;
    for (uint32 slot = 0; slot < m_numGrids; ++slot)
    {
        if (m_grids[slot].cellSize == 0)
            continue;
        const uint32 numCells = GRID_SIZE / m_grids[slot].cellSize;
        const uint32 totalCells = numCells * numCells * numCells;
        for (uint32 cellBegin = 0; cellBegin < totalCells; cellBegin += GATHER_GROUP)
        {
            m_workgroups[wg++] = slot;
            m_workgroups[wg++] = cellBegin;
        }
    }

    m_demand = Demand{
        .numGrids = glm::max(gridDemand, m_numGrids), // failed claims count: growth fits the burst
        .gridDataBytes = (size_t)dataOffset * sizeof(uint32),
        .numWorkgroups = numWorkgroups,
        .lightListSize = listSize,
    };
    return m_demand;
}

// ---------------------------------------------------------------------------------------------
// 3. The upload (the build job, or the main thread after a growth).

void LightGridComputePipeline::upload(Buffer& tableBuffer, uint32 tableEntries)
{
    ProfileScope scope("Light grid upload", EProfileCategory::Renderer);
    PerFrameData& frameData = m_perFrameData[m_frameIdx];
    assert(m_numGrids <= m_gridCapacity && m_demand.numWorkgroups <= m_workgroupCapacity && m_demand.lightListSize <= m_lightListCapacity);

    if (m_numGrids > 0)
    {
        memcpy(frameData.mappedGridJobs.data(), m_grids.data(), m_numGrids * sizeof(GridJob));
        frameData.inGridJobsBuffer.flushMappedMemory(m_numGrids * sizeof(GridJob));
    }
    if (m_demand.numWorkgroups > 0)
    {
        memcpy(frameData.mappedWorkgroups.data(), m_workgroups.data(), m_demand.numWorkgroups * 2 * sizeof(uint32));
        frameData.inWorkgroupsBuffer.flushMappedMemory(m_demand.numWorkgroups * 2 * sizeof(uint32));
    }
    if (m_demand.lightListSize > 0)
    {
        memcpy(frameData.mappedLightList.data(), m_lightList.data(), m_demand.lightListSize * sizeof(uint32));
        frameData.inLightListBuffer.flushMappedMemory(m_demand.lightListSize * sizeof(uint32));
    }

    // The hash table the readers probe: header + open-addressed slots holding grid data offsets.
    const size_t tableBytes = 3 * sizeof(uint32) + tableEntries * sizeof(uint32);
    oc::span<uint32> table = tableBuffer.mapMemory<uint32>(0, tableBytes);
    uint32 liveGrids = 0;
    uint32* slots = table.data() + 3;
    memset(slots, 0xFF, tableEntries * sizeof(uint32));
    const uint32 mask = tableEntries - 1;
    for (uint32 slot = 0; slot < m_numGrids; ++slot)
    {
        const GridJob& g = m_grids[slot];
        if (g.cellSize == 0)
            continue;
        ++liveGrids;
        uint32 idx = positionHash(g.pos) & mask;
        while (slots[idx] != EMPTY_ENTRY)
            idx = (idx + 1) & mask;
        slots[idx] = g.dataOffset;
    }
    table[0] = liveGrids;
    table[1] = (uint32)(m_demand.gridDataBytes / sizeof(uint32));
    table[2] = tableEntries;
    tableBuffer.flushMappedMemory(tableBytes);

    frameData.mappedIndirectCommands[0] = vk::DispatchIndirectCommand{ .x = m_demand.numWorkgroups, .y = 1, .z = 1 };
    frameData.inIndirectCommandBuffer.flushMappedMemory(vk::WholeSize);
}

void LightGridComputePipeline::record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& recordParams)
{
    PerFrameData& frameData = m_perFrameData[frameIdx];
    const auto bufInfo = [](Buffer& b) { return vk::DescriptorBufferInfo{ .buffer = b.getBuffer(), .range = b.getSize() }; };
    oc::array<DescriptorSetUpdateInfo, 6> computeDescriptorSetUpdateInfos
    {
        DescriptorSetUpdateInfo{ .binding = 0, .type = vk::DescriptorType::eUniformBuffer, .bufferInfos = { bufInfo(recordParams.ubo) } },
        DescriptorSetUpdateInfo{ .binding = 1, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(frameData.inLightCullBuffer) } },
        DescriptorSetUpdateInfo{ .binding = 2, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(frameData.inGridJobsBuffer) } },
        DescriptorSetUpdateInfo{ .binding = 3, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(frameData.inWorkgroupsBuffer) } },
        DescriptorSetUpdateInfo{ .binding = 4, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(frameData.inLightListBuffer) } },
        DescriptorSetUpdateInfo{ .binding = 5, .type = vk::DescriptorType::eStorageBuffer, .bufferInfos = { bufInfo(recordParams.outLightGridBuffer) } },
    };

    vk::CommandBuffer vkCommandBuffer = commandBuffer.getCommandBuffer();
    vk::DescriptorSet descriptorSet = recordParams.descriptorSet.getDescriptorSet();
    vkCommandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_computePipeline.getPipeline());
    commandBuffer.cmdUpdateDescriptorSets(m_computePipeline.getPipelineLayout(), vk::PipelineBindPoint::eCompute, descriptorSet, computeDescriptorSetUpdateInfos);
    vkCommandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_computePipeline.getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);

    // No clear: every header and every cell of every grid is written by the gather, and the table
    // comes from the CPU. The previous use of this frame slot's buffers was fenced at the loop top.
    vkCommandBuffer.dispatchIndirect(frameData.inIndirectCommandBuffer.getBuffer(), 0);

    {
        vk::MemoryBarrier2 memoryBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
        };
        vkCommandBuffer.pipelineBarrier2(vk::DependencyInfo{ .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier });
    }
}
