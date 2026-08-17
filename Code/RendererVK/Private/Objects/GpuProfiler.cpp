module RendererVK;

import Core;
import Core.Log;

import :VK;
import :Device;
import :GpuProfiler;
import :Layout;

void GpuProfiler::initialize()
{
    const vk::PhysicalDevice physicalDevice = Globals::device.getPhysicalDevice();
    const vk::PhysicalDeviceProperties properties = physicalDevice.getProperties();
    const oc::vector<vk::QueueFamilyProperties> queueFamilies = physicalDevice.getQueueFamilyProperties();
    const uint32 timestampValidBits = queueFamilies[Globals::device.getGraphicsQueueIndex()].timestampValidBits;
    if (timestampValidBits == 0 || properties.limits.timestampPeriod <= 0.0f)
    {
        Log::warning("GpuProfiler: timestamps not supported on the graphics queue, GPU profiling disabled");
        return;
    }
    m_timestampPeriodNs = (double)properties.limits.timestampPeriod;
    m_timestampMask = timestampValidBits >= 64 ? ~0ull : ((1ull << timestampValidBits) - 1ull);
    m_useCalibration = Globals::device.supportsCalibratedTimestamps();

    const vk::Device device = Globals::device.getDevice();
    for (FrameSlot& slot : m_slots)
    {
        const vk::QueryPoolCreateInfo poolInfo{
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = MAX_SCOPES * 2,
        };
        auto createResult = device.createQueryPool(poolInfo);
        if (createResult.result != vk::Result::eSuccess)
        {
            Log::warning("GpuProfiler: failed to create timestamp query pool, GPU profiling disabled");
            return;
        }
        slot.queryPool = createResult.value;
    }

    m_track = Globals::profiler.createNamedTrack("GPU", 1);
    m_supported = m_track != nullptr;
}

void GpuProfiler::collect(uint32 frameIdx)
{
    if (!m_supported)
        return;
    FrameSlot& slot = m_slots[frameIdx];
    if (!slot.pending || slot.numScopes == 0)
    {
        slot.pending = false;
        return;
    }
    slot.pending = false;

    const vk::Device device = Globals::device.getDevice();
    oc::array<uint64, MAX_SCOPES * 2> queryResults;
    const uint32 numQueries = slot.numScopes * 2;
    const vk::Result result = device.getQueryPoolResults(slot.queryPool, 0, numQueries,
        numQueries * sizeof(uint64), queryResults.data(), sizeof(uint64), vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) // the slot's fence was waited, so eNotReady means something went sideways - just drop the frame
        return;

    Profiler& profiler = Globals::profiler;
    const double ticksPerNs = profiler.getTicksPerMs() * 1e-6;

    // Map GPU timestamps onto the CPU tick timeline. Calibrated: sample {GPU now, QPC now} and place
    // each timestamp by its distance behind GPU-now. Fallback: anchor the frame's first timestamp at
    // the CPU submit tick (drops the queue latency, keeps all durations exact).
    uint64 anchorTick = slot.cpuSubmitTick;
    uint64 anchorGpu = queryResults[0] & m_timestampMask;
    if (m_useCalibration)
    {
        const oc::array<vk::CalibratedTimestampInfoKHR, 2> infos{
            vk::CalibratedTimestampInfoKHR{ .timeDomain = vk::TimeDomainKHR::eDevice },
            vk::CalibratedTimestampInfoKHR{ .timeDomain = vk::TimeDomainKHR::eQueryPerformanceCounter },
        };
        oc::array<uint64, 2> timestamps = {};
        uint64 maxDeviation = 0;
        const vk::Result calibResult = device.getCalibratedTimestampsKHR(2, infos.data(), timestamps.data(), &maxDeviation);
        if (calibResult == vk::Result::eSuccess)
        {
            anchorGpu = timestamps[0] & m_timestampMask;
            anchorTick = profiler.ticksFromQpc(timestamps[1]);
        }
        else
        {
            Log::warning("GpuProfiler: vkGetCalibratedTimestampsKHR failed, falling back to submit-time anchoring");
            m_useCalibration = false;
        }
    }

    const auto toTick = [&](uint64 gpuTimestamp) -> uint64
    {
        // Signed distance from the anchor (calibrated: the anchor is sampled AFTER the frame ran, so
        // deltas are negative). With < 64 valid timestamp bits, subtract in the masked domain and
        // sign-extend around its half range.
        int64 delta;
        if (m_timestampMask == ~0ull)
            delta = (int64)(gpuTimestamp - anchorGpu);
        else
        {
            const uint64 masked = (gpuTimestamp - anchorGpu) & m_timestampMask;
            const uint64 half = (m_timestampMask >> 1) + 1;
            delta = masked >= half ? (int64)masked - (int64)(m_timestampMask + 1) : (int64)masked;
        }
        return anchorTick + (int64)((double)delta * m_timestampPeriodNs * ticksPerNs);
    };

    oc::array<ProfileRecord, MAX_SCOPES> records;
    for (uint32 i = 0; i < slot.numScopes; ++i)
    {
        records[i].start = toTick(queryResults[i * 2 + 0] & m_timestampMask);
        records[i].end = toTick(queryResults[i * 2 + 1] & m_timestampMask);
        records[i].name = slot.scopes[i].name;
        records[i].depth = slot.scopes[i].depth;
        records[i].category = (uint8)EProfileCategory::GPU;
    }
    // The track contract wants pushes ordered by END time (scopes are stored in begin order; a
    // parent ends after its children but was stored first).
    oc::sort(records.begin(), records.begin() + slot.numScopes,
        [](const ProfileRecord& a, const ProfileRecord& b) { return a.end < b.end; });
    for (uint32 i = 0; i < slot.numScopes; ++i)
        m_track->push(records[i].start, records[i].end, records[i].name, records[i].depth, EProfileCategory::GPU);
}

void GpuProfiler::beginRecord(vk::CommandBuffer cmd, uint32 frameIdx)
{
    if (!m_supported)
        return;
    m_recordSlot = frameIdx;
    m_openDepth = 0;
    FrameSlot& slot = m_slots[frameIdx];
    slot.numScopes = 0;
    cmd.resetQueryPool(slot.queryPool, 0, MAX_SCOPES * 2);
}

void GpuProfiler::beginScope(vk::CommandBuffer cmd, const char* name)
{
    if (!m_supported)
        return;
    FrameSlot& slot = m_slots[m_recordSlot];
    if (slot.numScopes >= MAX_SCOPES || m_openDepth >= MAX_SCOPES)
    {
        assert(false && "GpuProfiler: MAX_SCOPES exceeded");
        return;
    }
    const uint32 idx = slot.numScopes++;
    slot.scopes[idx] = ScopeMeta{ .name = name, .depth = (uint16)m_openDepth };
    m_openStack[m_openDepth++] = idx;
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, slot.queryPool, idx * 2);
}

void GpuProfiler::endScope(vk::CommandBuffer cmd)
{
    if (!m_supported)
        return;
    FrameSlot& slot = m_slots[m_recordSlot];
    if (m_openDepth == 0)
    {
        assert(false && "GpuProfiler: endScope without matching beginScope");
        return;
    }
    const uint32 idx = m_openStack[--m_openDepth];
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, slot.queryPool, idx * 2 + 1);
}

void GpuProfiler::onSubmit(uint32 frameIdx)
{
    if (!m_supported)
        return;
    FrameSlot& slot = m_slots[frameIdx];
    slot.cpuSubmitTick = Profiler::tick();
    slot.pending = slot.numScopes > 0;
}
