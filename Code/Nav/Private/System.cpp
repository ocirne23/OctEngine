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
    for (TeamSlot& slot : m_goals)
        waitSlot(slot);
}

void NavSystem::initialize()
{
    if (m_initialized)
        return;
    m_initialized = true;
    for (DensityField& d : m_density)
        d.initialize();
    Tweak::boolean("Nav", "Enabled", &m_enabled);
    Tweak::floatVar("Nav", "Field radius", &m_fieldRadius, 10.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Nav", "Rebuild interval", &m_rebuildInterval, 0.05f, 5.0f, 0.05f);
    Tweak::intVar("Nav", "Clearance cost", &m_clearanceCost, 0, 32);
    Tweak::intVar("Nav", "Chunk keep frames", &m_keepFrames, 1, 2000);
    Tweak::intVar("Nav", "Debug draw", &m_debugMode, 0, 3);
    Tweak::intVar("Nav", "Debug team", &m_debugTeam, 0, int(MaxTeams + MaxGoals) - 1); // 8+ = goal fields
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

void NavSystem::setGoal(uint32 slotIndex, const glm::vec3& dest, float radius)
{
    if (slotIndex >= MaxGoals)
        return;
    TeamSlot& slot = m_goals[slotIndex];
    slot.periodic = false;
    const NavSource src{ dest, 0.5f, 0, 2 };
    const bool moved = sourcesChanged(slot.sources, oc::span<const NavSource>(&src, 1));
    if (moved)
    {
        slot.sources.assign(1, src);
        slot.sourcesDirty = true;
    }
    slot.radius = glm::max(radius, 10.0f);
}

void NavSystem::clearGoal(uint32 slotIndex)
{
    if (slotIndex >= MaxGoals)
        return;
    TeamSlot& slot = m_goals[slotIndex];
    slot.sources.clear();
    slot.sourcesDirty = false;
    if (!slot.building)
        slot.published.reset();
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
    if (!m_enabled || slot.sources.empty())
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
    for (TeamSlot& slot : m_teams)
        publish(slot);
    for (TeamSlot& slot : m_goals)
        publish(slot);

    // 2. Kick rebuilds. The obstacle snapshot is shared by every job, so it may only be replaced
    //    while NO build is in flight; a dirty obstacle set waits for the fleet to drain.
    bool anyBuilding = false;
    for (TeamSlot& slot : m_teams)
        anyBuilding |= slot.building;
    for (TeamSlot& slot : m_goals)
        anyBuilding |= slot.building;
    if (m_obstaclesDirty && !anyBuilding)
    {
        m_buildObstacles = m_obstacles;
        m_obstaclesDirty = false;
        for (TeamSlot& slot : m_teams)
            slot.sourcesDirty = true; // every field depends on the raster
        for (TeamSlot& slot : m_goals)
            slot.sourcesDirty = true;
    }
    for (TeamSlot& slot : m_teams)
        tickSlot(slot, deltaSec);
    for (TeamSlot& slot : m_goals)
        tickSlot(slot, deltaSec);

    m_publishedCount = 0;
    for (const TeamSlot& slot : m_teams)
        m_publishedCount += slot.published ? 1u : 0u;

    // 3. Density flip/clear/evict.
    for (DensityField& d : m_density)
        d.update(uint32(glm::max(m_keepFrames, 1)));
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
    for (TeamSlot& slot : m_goals)
        clearSlot(slot);
    for (DensityField& d : m_density)
        d.clear();
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
    // Debug team 0..7 = team fields, 8.. = goal slots (8 = the local player's move order).
    const uint32 sel = uint32(glm::clamp(m_debugTeam, 0, int(MaxTeams + MaxGoals) - 1));
    const uint32 team = glm::min(sel, MaxTeams - 1);
    const TeamField* field = sel < MaxTeams ? m_teams[sel].published.get()
                                            : m_goals[sel - MaxTeams].published.get();
    const float y = 0.15f;
    const glm::vec2 f(focus.x, focus.z);
    const float r2 = m_debugRadius * m_debugRadius;

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
            if (m_debugMode < 2)
                return;
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
    if (m_debugMode >= 3)
    {
        const DensityField& density = m_density[team];
        const uint32 col = packColor(1.0f, 0.8f, 0.2f);
        density.chunks().forEach([&](uint64 key, const DensityField::Chunk& chunk)
        {
            const glm::ivec2 base = chunkFromKey(key) * ChunkCells;
            const uint32 buffer = density.readBuffer();
            for (int i = 0; i < ChunkArea; ++i)
            {
                const uint16 v = chunk.v[buffer][i];
                if (v == 0)
                    continue;
                const glm::vec2 centre = cellCenter(base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits));
                if (glm::dot(centre - f, centre - f) > r2)
                    continue;
                line(glm::vec3(centre.x, y, centre.y), glm::vec3(centre.x, y + 0.5f * v, centre.y), col);
            }
        });
    }
}

}
