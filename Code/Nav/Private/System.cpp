module Nav;

import Core;
import Core.glm;
import Core.Tweaks;
import Threading;

namespace Nav
{

NavSystem::~NavSystem()
{
    waitAll();
}

void NavSystem::waitAll()
{
    if (!Globals::jobSystem.isInitialized())
        return;
    const auto waitSlot = [](TeamSlot& slot)
    {
        if (slot.building)
        {
            Globals::jobSystem.wait(slot.counter);
            slot.building = false;
            slot.pending.reset();
        }
    };
    for (TeamSlot& slot : m_teams)
        waitSlot(slot);
    waitSlot(m_raster);
    for (auto& [key, slot] : m_goals)
        waitSlot(*slot);
}

void NavSystem::initialize()
{
    if (m_initialized)
        return;
    m_initialized = true;
    m_raster.rasterOnly = true;
    m_raster.periodic = false;
    for (FlowField& f : m_flow)
        f.initialize();
    for (PressureField& p : m_pressure)
        p.initialize();
    Tweak::boolean("Nav", "Enabled", &m_enabled);
    Tweak::floatVar("Nav", "Flow decay", &m_flowDecay, 0.5f, 0.9999f, 0.0005f);
    Tweak::floatVar("Nav", "Pressure diffusion", &m_pressureDiffusion, 0.0f, 0.25f, 0.005f);
    Tweak::floatVar("Nav", "Pressure decay", &m_pressureDecay, 0.5f, 0.9999f, 0.001f);
    Tweak::floatVar("Nav", "Pressure flow gain", &m_pressureFlowGain, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Nav", "Wall bounce", &m_wallBounce, 0.0f, 1.0f, 0.05f);
    Tweak::floatVar("Nav", "Field radius", &m_fieldRadius, 10.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Nav", "Rebuild interval", &m_rebuildInterval, 0.05f, 5.0f, 0.05f);
    Tweak::intVar("Nav", "Clearance cost", &m_clearanceCost, 0, 32);
    Tweak::intVar("Nav", "Chunk keep frames", &m_keepFrames, 1, 2000);
    Tweak::intVar("Nav", "Debug draw", &m_debugMode, 0, 2); // 1 = chunks + team field, 2 = flow + pressure
    Tweak::intVar("Nav", "Debug team", &m_debugTeam, 0, int(MaxTeams)); // 8 = the player's goal field
    Tweak::floatVar("Nav", "Debug radius", &m_debugRadius, 5.0f, 400.0f, 1.0f);
}

static uint64 hashBytes(const void* data, size_t size)
{
    uint64 h = 1469598103934665603ull; // FNV-1a
    const uint8* p = static_cast<const uint8*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

void NavSystem::setObstacles(oc::span<const NavObstacle> obstacles)
{
    const uint64 h = hashBytes(obstacles.data(), obstacles.size() * sizeof(NavObstacle)) ^ obstacles.size();
    if (h == m_obstacleHash && m_obstacles.size() == obstacles.size())
        return;
    m_obstacleHash = h;
    m_obstacles.assign(obstacles.begin(), obstacles.end());
    m_obstaclesDirty = true;
}

bool NavSystem::sourcesChanged(const oc::vector<NavSource>& a, oc::span<const NavSource> b) const
{
    if (a.size() != b.size())
        return true;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].id != b[i].id || a[i].kind != b[i].kind)
            return true;
        // A source that moved less than half a cell keeps the field; players walking re-kick.
        const glm::vec2 d(a[i].pos.x - b[i].pos.x, a[i].pos.z - b[i].pos.z);
        if (glm::dot(d, d) > 0.25f * CellSize * CellSize)
            return true;
    }
    return false;
}

void NavSystem::setTeamSources(uint32 team, oc::span<const NavSource> sources)
{
    if (team >= MaxTeams)
        return;
    TeamSlot& slot = m_teams[team];
    if (!sourcesChanged(slot.sources, sources))
        return;
    slot.sources.assign(sources.begin(), sources.end());
    slot.sourcesDirty = true;
}

void NavSystem::setGoal(uint64 key, const glm::vec3& dest, float radius)
{
    oc::unique_ptr<TeamSlot>& entry = m_goals[key];
    if (!entry)
        entry = oc::make_unique<TeamSlot>();
    TeamSlot& slot = *entry;
    slot.periodic = false;
    slot.lastSetFrame = m_frame;
    const NavSource src{ dest, 0.5f, 0, 2 };
    const bool moved = sourcesChanged(slot.sources, oc::span<const NavSource>(&src, 1));
    if (moved)
    {
        slot.sources.assign(1, src);
        slot.sourcesDirty = true;
    }
    slot.radius = glm::max(radius, 10.0f);
}

void NavSystem::clearGoal(uint64 key)
{
    const auto it = m_goals.find(key);
    if (it == m_goals.end())
        return;
    TeamSlot& slot = *it->second;
    slot.sources.clear();
    slot.sourcesDirty = false;
    if (!slot.building)
        slot.published.reset();
}

const TeamField* NavSystem::goalField(uint64 key) const
{
    const auto it = m_goals.find(key);
    return it != m_goals.end() ? it->second->published.get() : nullptr;
}

void NavSystem::kickBuild(TeamSlot& slot)
{
    slot.buildSources = slot.sources;
    slot.pending = oc::make_shared<TeamField>();
    slot.building = true;
    slot.sourcesDirty = false;
    slot.timer = m_rebuildInterval;
    const TeamField::BuildParams params{ slot.radius > 0.0f ? slot.radius : m_fieldRadius,
        uint8(glm::clamp(m_clearanceCost, 0, 254)) };
    NavSystem* self = this;
    TeamSlot* slotPtr = &slot;
    Globals::jobSystem.submit([self, slotPtr, params]
    {
        slotPtr->pending->build(self->m_buildObstacles, slotPtr->buildSources, params);
    }, EJobPriority::Low, &slot.counter, "navFieldBuild");
}

void NavSystem::tickSlot(TeamSlot& slot, float deltaSec)
{
    slot.timer -= deltaSec;
    if (slot.building)
        return;
    if (!m_enabled || (slot.sources.empty() && !slot.rasterOnly))
    {
        slot.published.reset(); // no sources = no field (units fall back to local search)
        slot.sourcesDirty = false;
        return;
    }
    const bool due = slot.sourcesDirty || (slot.periodic && slot.timer <= 0.0f) || !slot.published;
    if (due && !m_obstaclesDirty)
        kickBuild(slot);
}

void NavSystem::update(float deltaSec)
{
    ProfileScope scope("Nav update", EProfileCategory::Game);
    if (!m_initialized)
        return;

    // 1. Publish finished builds.
    const auto publish = [](TeamSlot& slot)
    {
        if (slot.building && slot.counter.isDone())
        {
            slot.building = false;
            slot.published = oc::move(slot.pending);
            slot.pending.reset();
        }
    };
    ++m_frame;
    for (TeamSlot& slot : m_teams)
        publish(slot);
    publish(m_raster);
    for (auto& [key, slot] : m_goals)
        publish(*slot);
    // Expire goals nobody set for a while (only idle ones — a building slot waits its turn).
    for (auto it = m_goals.begin(); it != m_goals.end();)
    {
        TeamSlot& slot = *it->second;
        if (!slot.building && m_frame - slot.lastSetFrame > GoalExpireFrames)
            it = m_goals.erase(it);
        else
            ++it;
    }

    // 2. Kick rebuilds. The obstacle snapshot is shared by every job, so it may only be replaced
    //    while NO build is in flight; a dirty obstacle set waits for the fleet to drain.
    bool anyBuilding = false;
    for (TeamSlot& slot : m_teams)
        anyBuilding |= slot.building;
    anyBuilding |= m_raster.building;
    for (auto& [key, slot] : m_goals)
        anyBuilding |= slot->building;
    if (m_obstaclesDirty && !anyBuilding)
    {
        m_buildObstacles = m_obstacles;
        m_obstaclesDirty = false;
        for (TeamSlot& slot : m_teams)
            slot.sourcesDirty = true; // every field depends on the raster
        m_raster.sourcesDirty = true;
        for (auto& [key, slot] : m_goals)
            slot->sourcesDirty = true;
    }
    for (TeamSlot& slot : m_teams)
        tickSlot(slot, deltaSec);
    tickSlot(m_raster, deltaSec);
    for (auto& [key, slot] : m_goals)
        tickSlot(*slot, deltaSec);

    m_publishedCount = 0;
    for (const TeamSlot& slot : m_teams)
        m_publishedCount += slot.published ? 1u : 0u;

    // 3. Flow + pressure steps.
    for (FlowField& f : m_flow)
        f.update(uint32(glm::max(m_keepFrames, 1)), m_flowDecay, m_raster.published.get(), m_wallBounce);
    {
        ProfileScope pscope("Nav pressure diffuse", EProfileCategory::Game);
        for (PressureField& p : m_pressure)
            p.update(m_raster.published.get(), m_pressureDiffusion, m_pressureDecay, uint32(glm::max(m_keepFrames, 1)));
        // Pressure PUSHES the flow (v += -grad p): every pressurized cell splats the negative
        // gradient into the team's lanes, so a jam bends the stream upstream of it and units
        // following the lane are diverted before they arrive.
        if (m_pressureFlowGain > 0.0f)
            for (uint32 t = 0; t < MaxTeams; ++t)
            {
                const PressureField& p = m_pressure[t];
                FlowField& flow = m_flow[t];
                const uint32 buffer = p.readBuffer();
                p.chunks().forEach([&](uint64 key, const PressureField::Chunk& chunk)
                {
                    if (chunk.peak < 0.02f)
                        return;
                    const glm::ivec2 base = chunkFromKey(key) * ChunkCells;
                    for (int i = 0; i < ChunkArea; ++i)
                    {
                        if (chunk.p[buffer][i] < 0.02f)
                            continue;
                        const glm::vec2 centre = cellCenter(base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits));
                        const glm::vec2 g = p.gradient(centre);
                        if (glm::dot(g, g) > 1e-6f)
                            flow.splat(centre, -g * m_pressureFlowGain);
                    }
                });
            }
    }
}

void NavSystem::clear()
{
    waitAll();
    const auto clearSlot = [](TeamSlot& slot)
    {
        slot.sources.clear();
        slot.buildSources.clear();
        slot.published.reset();
        slot.sourcesDirty = false;
    };
    for (TeamSlot& slot : m_teams)
        clearSlot(slot);
    clearSlot(m_raster);
    m_goals.clear();
    for (FlowField& f : m_flow)
        f.clear();
    for (PressureField& p : m_pressure)
        p.clear();
    m_obstacles.clear();
    m_buildObstacles.clear();
    m_obstacleHash = 0;
    m_obstaclesDirty = false;
    m_publishedCount = 0;
}

static uint32 packColor(float r, float g, float b)
{
    return uint32(r * 255.0f) | (uint32(g * 255.0f) << 8) | (uint32(b * 255.0f) << 16) | 0xFF000000u;
}

void NavSystem::drawDebug(const glm::vec3& focus,
    const oc::function<void(const glm::vec3&, const glm::vec3&, uint32)>& line) const
{
    if (m_debugMode <= 0)
        return;
    // Debug team 0..7 = team fields, 8 = the local player's move-order goal.
    const uint32 sel = uint32(glm::clamp(m_debugTeam, 0, int(MaxTeams)));
    const uint32 team = glm::min(sel, MaxTeams - 1);
    const TeamField* field = sel < MaxTeams ? m_teams[sel].published.get() : goalField(GoalKeyPlayer);
    const float y = 0.15f;
    const glm::vec2 f(focus.x, focus.z);
    const float r2 = m_debugRadius * m_debugRadius;

    if (m_debugMode == 2)
    {
        // FLOW + PRESSURE of `team` in one view. Pressure = a filled-looking cell (a diamond +
        // vertical bar, height and colour yellow->red by value); flow = an arrow per cell (length +
        // colour cyan->magenta by strength) drawn on top. Chunk outlines: pressure dim red, flow dim
        // cyan.
        const PressureField& pressure = m_pressure[team];
        const uint32 pbuf = pressure.readBuffer();
        const uint32 poutline = packColor(0.5f, 0.2f, 0.2f);
        pressure.chunks().forEach([&](uint64 key, const PressureField::Chunk& chunk)
        {
            const glm::vec2 mn = chunkMinWorld(chunkFromKey(key));
            const glm::vec2 mx = mn + ChunkSize;
            line(glm::vec3(mn.x, y, mn.y), glm::vec3(mx.x, y, mn.y), poutline);
            line(glm::vec3(mx.x, y, mn.y), glm::vec3(mx.x, y, mx.y), poutline);
            line(glm::vec3(mx.x, y, mx.y), glm::vec3(mn.x, y, mx.y), poutline);
            line(glm::vec3(mn.x, y, mx.y), glm::vec3(mn.x, y, mn.y), poutline);
            const glm::ivec2 base = chunkFromKey(key) * ChunkCells;
            for (int i = 0; i < ChunkArea; ++i)
            {
                const float v = chunk.p[pbuf][i];
                if (v < 0.02f)
                    continue;
                const glm::vec2 c = cellCenter(base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits));
                if (glm::dot(c - f, c - f) > r2)
                    continue;
                const float t = glm::clamp(v / 2.0f, 0.0f, 1.0f);
                const uint32 col = packColor(1.0f, 1.0f - 0.8f * t, 0.1f);
                const float h = 0.3f + 4.0f * t;
                const float e = 0.3f + 0.6f * t; // diamond half-size grows with pressure
                // Diamond on the ground + a bar + a diamond at the top: reads as a column.
                const glm::vec3 g0(c.x - e, y, c.y), g1(c.x, y, c.y - e), g2(c.x + e, y, c.y), g3(c.x, y, c.y + e);
                line(g0, g1, col); line(g1, g2, col); line(g2, g3, col); line(g3, g0, col);
                const glm::vec3 up(0.0f, h, 0.0f);
                line(g0 + up, g1 + up, col); line(g1 + up, g2 + up, col); line(g2 + up, g3 + up, col); line(g3 + up, g0 + up, col);
                line(g0, g0 + up, col); line(g2, g2 + up, col);
            }
        });
        const FlowField& flow = m_flow[team];
        const uint32 fbuf = flow.readBuffer();
        const uint32 foutline = packColor(0.2f, 0.5f, 0.6f);
        flow.chunks().forEach([&](uint64 key, const FlowField::Chunk& chunk)
        {
            const glm::vec2 mn = chunkMinWorld(chunkFromKey(key));
            const glm::vec2 mx = mn + ChunkSize;
            line(glm::vec3(mn.x, y, mn.y), glm::vec3(mx.x, y, mn.y), foutline);
            line(glm::vec3(mx.x, y, mn.y), glm::vec3(mx.x, y, mx.y), foutline);
            line(glm::vec3(mx.x, y, mx.y), glm::vec3(mn.x, y, mx.y), foutline);
            line(glm::vec3(mn.x, y, mx.y), glm::vec3(mn.x, y, mn.y), foutline);
            const glm::ivec2 base = chunkFromKey(key) * ChunkCells;
            for (int i = 0; i < ChunkArea; ++i)
            {
                const glm::vec2 v = glm::vec2(chunk.vx[fbuf][i], chunk.vz[fbuf][i]) / FlowField::Scale;
                const float mag = glm::length(v);
                if (mag < 0.05f)
                    continue;
                const glm::vec2 centre = cellCenter(base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits));
                if (glm::dot(centre - f, centre - f) > r2)
                    continue;
                const float t = glm::clamp(mag / 8.0f, 0.0f, 1.0f);
                const uint32 col = packColor(t, 1.0f - t, 1.0f);
                const glm::vec2 dir = v / mag;
                const float len = 0.3f + 0.6f * t;
                const glm::vec2 tip = centre + dir * len;
                const glm::vec2 side(-dir.y, dir.x);
                const float ya = y + 0.05f;
                line(glm::vec3(centre.x, ya, centre.y), glm::vec3(tip.x, ya, tip.y), col);
                line(glm::vec3(tip.x, ya, tip.y), glm::vec3(tip.x - dir.x * 0.25f + side.x * 0.15f, ya, tip.y - dir.y * 0.25f + side.y * 0.15f), col);
                line(glm::vec3(tip.x, ya, tip.y), glm::vec3(tip.x - dir.x * 0.25f - side.x * 0.15f, ya, tip.y - dir.y * 0.25f - side.y * 0.15f), col);
            }
        });
        return;
    }
    if (field)
    {
        const uint32 outline = packColor(0.3f, 0.6f, 1.0f);
        field->chunks().forEach([&](uint64 key, const TeamField::Chunk& chunk)
        {
            const glm::vec2 mn = chunkMinWorld(chunkFromKey(key));
            const glm::vec2 mx = mn + ChunkSize;
            line(glm::vec3(mn.x, y, mn.y), glm::vec3(mx.x, y, mn.y), outline);
            line(glm::vec3(mx.x, y, mn.y), glm::vec3(mx.x, y, mx.y), outline);
            line(glm::vec3(mx.x, y, mx.y), glm::vec3(mn.x, y, mx.y), outline);
            line(glm::vec3(mn.x, y, mx.y), glm::vec3(mn.x, y, mn.y), outline);
            const glm::ivec2 base = chunkFromKey(key) * ChunkCells;
            for (int i = 0; i < ChunkArea; ++i)
            {
                const glm::ivec2 c = base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits);
                const glm::vec2 centre = cellCenter(c);
                if (glm::dot(centre - f, centre - f) > r2)
                    continue;
                if (chunk.cost[i] == TeamField::Blocked)
                {
                    const uint32 red = packColor(1.0f, 0.2f, 0.2f);
                    line(glm::vec3(centre.x - 0.6f, y, centre.y - 0.6f), glm::vec3(centre.x + 0.6f, y, centre.y + 0.6f), red);
                    line(glm::vec3(centre.x - 0.6f, y, centre.y + 0.6f), glm::vec3(centre.x + 0.6f, y, centre.y - 0.6f), red);
                    continue;
                }
                if (chunk.dist[i] == TeamField::Unreached)
                    continue;
                const TeamField::Sample s = field->sample(centre, 0);
                if (!s.valid)
                    continue;
                const float t = glm::clamp(s.dist / glm::max(m_fieldRadius, 1.0f), 0.0f, 1.0f);
                const uint32 col = packColor(t, 1.0f - t, 0.2f);
                const glm::vec2 tip = centre + s.descentDir * 0.8f;
                const glm::vec2 side(-s.descentDir.y, s.descentDir.x);
                line(glm::vec3(centre.x, y, centre.y), glm::vec3(tip.x, y, tip.y), col);
                line(glm::vec3(tip.x, y, tip.y), glm::vec3(tip.x - s.descentDir.x * 0.3f + side.x * 0.2f, y, tip.y - s.descentDir.y * 0.3f + side.y * 0.2f), col);
                line(glm::vec3(tip.x, y, tip.y), glm::vec3(tip.x - s.descentDir.x * 0.3f - side.x * 0.2f, y, tip.y - s.descentDir.y * 0.3f - side.y * 0.2f), col);
            }
        });
    }
}

}
