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
    Tweak::floatVar("Nav", "Flow half-life (s)", &m_flowHalfLife, 0.05f, 60.0f, 0.05f);
    Tweak::floatVar("Nav", "Pressure diffusion", &m_pressureDiffusion, 0.0f, 0.25f, 0.005f);
    Tweak::floatVar("Nav", "Pressure half-life (s)", &m_pressureHalfLife, 0.02f, 30.0f, 0.02f);
    Tweak::floatVar("Nav", "Pressure floor", &m_pressureFloor, 0.0f, 5.0f, 0.02f);
    Tweak::floatVar("Nav", "Pressure flow gain 10^x", &m_pressureFlowGainExp, -2.0f, 3.0f, 0.02f);
    Tweak::floatVar("Nav", "Wall bounce", &m_wallBounce, 0.0f, 1.0f, 0.05f);
    Tweak::floatVar("Nav", "Flow viscosity", &m_flowViscosity, 0.0f, 0.25f, 0.005f);
    Tweak::floatVar("Nav", "Flow max (m/s)", &m_flowMaxSpeed, 0.0f, 40.0f, 0.5f);
    Tweak::floatVar("Nav", "Viscosity floor (m/s)", &m_flowViscosityFloor, 0.0f, 8.0f, 0.05f);
    Tweak::floatVar("Nav", "Viscosity backflow", &m_flowViscosityBackflow, 0.0f, 1.0f, 0.02f);
    Tweak::floatVar("Nav", "Seed area (m)", &m_seedArea, 2.0f, 64.0f, 1.0f);
    Tweak::floatVar("Nav", "Seed cooldown (s)", &m_seedCooldown, 0.0f, 30.0f, 0.25f);
    Tweak::intVar("Nav", "Seed max/frame", &m_seedMaxPerFrame, 0, 16);
    Tweak::floatVar("Nav", "Seed trough", &m_seedTrough, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Nav", "Seed trough squeeze", &m_seedSqueeze, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Nav", "Seed range (m)", &m_seedRange, 0.0f, 400.0f, 2.0f); // 0 = the whole path
    Tweak::floatVar("Nav", "Field radius", &m_fieldRadius, 10.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Nav", "Rebuild interval", &m_rebuildInterval, 0.05f, 5.0f, 0.05f);
    Tweak::intVar("Nav", "Clearance cost", &m_clearanceCost, 0, 32);
    Tweak::intVar("Nav", "Chunk keep frames", &m_keepFrames, 1, 2000);
    Tweak::intVar("Nav", "Debug draw", &m_debugMode, 0, 2); // 1 = chunks + team field, 2 = flow + pressure
    Tweak::intVar("Nav", "Debug team", &m_debugTeam, 0, int(MaxTeams)); // 8 = the player's goal field
    Tweak::floatVar("Nav", "Debug radius", &m_debugRadius, 5.0f, 400.0f, 1.0f);
    Tweak::floatVar("Nav", "Debug flow min (m/s)", &m_debugFlowMin, 0.0f, 8.0f, 0.05f);
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

bool NavSystem::seedPath(uint32 team, const glm::vec3& from, const glm::vec3& to, float speed,
    float laneWidth, float clearance, oc::vector<glm::vec2>* outPath)
{
    ProfileScope scope("Nav seed path", EProfileCategory::Game);
    const TeamField* raster = m_raster.published.get();
    if (!raster || team >= MaxTeams)
        return false;
    oc::vector<glm::vec2> path;
    if (!raster->findPath(glm::vec2(from.x, from.z), glm::vec2(to.x, to.z), 8192, clearance * 0.5f, path))
        return false;
    // The A* runs to the real destination (a truncated SEARCH would pick the wrong way round an
    // obstacle), but only the first "Seed range" metres are WRITTEN: a lane far ahead of the group
    // is stale by the time anyone gets there, and the group re-seeds from where it actually is.
    if (m_seedRange > 0.0f)
    {
        float remaining = m_seedRange;
        for (size_t i = 0; i + 1 < path.size(); ++i)
        {
            const float len = glm::distance(path[i], path[i + 1]);
            if (len >= remaining)
            {
                path[i + 1] = path[i] + (path[i + 1] - path[i]) * (remaining / glm::max(len, 1e-4f));
                path.resize(i + 2);
                break;
            }
            remaining -= len;
        }
    }
    m_flow[team].seedPath(path, speed, laneWidth * 0.5f, raster);
    // ... and carve a pressure TROUGH along the same route: pressure is where "attraction" lives
    // (the steering reads -grad p and the flow is pushed by -grad p), so the lane pulls units and
    // surrounding flow into itself instead of only existing where it was drawn.
    if (m_seedTrough > 0.0f)
        m_pressure[team].seedPath(path, m_seedTrough, laneWidth * 0.5f, raster, m_seedSqueeze);
    if (outPath)
        *outPath = path;
    return true;
}

bool NavSystem::requestSeedPath(uint32 team, const glm::vec3& from, const glm::vec3& to, float speed,
    float laneWidth, float clearance)
{
    if (team >= MaxTeams || m_seedsThisFrame >= m_seedMaxPerFrame)
        return false;
    const glm::vec2 f(from.x, from.z), t(to.x, to.z);
    const float area = glm::max(m_seedArea, 0.5f);
    const float areaSq = area * area;
    const glm::ivec2 home(int32(glm::floor(f.x / area)), int32(glm::floor(f.y / area)));
    // Suppressed when a recent plan of this team started within `area` of here AND went to within
    // `area` of the same destination: a crowd walking the same way is one plan, a unit heading
    // somewhere else is not blocked by it. The test is a DISTANCE (a bucket border used to let two
    // plans through), but the candidates come from the 3x3 buckets around the start — bucket size
    // IS the radius, so nothing within range can sit outside that neighbourhood, and the cost per
    // request is constant no matter how many plans are alive.
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const auto it = m_seedBuckets.find(seedBucketKey(team, home + glm::ivec2(dx, dz)));
            if (it == m_seedBuckets.end())
                continue;
            // Read-only: retiring is the expiry queue's job (removing here would break the
            // append-only order it relies on). A stamp that expired since the last update is
            // simply skipped — at most one frame's worth ever sits here.
            for (const SeedStamp& stamp : it->second)
                if (m_time - stamp.time < m_seedCooldown
                    && glm::dot(stamp.from - f, stamp.from - f) < areaSq
                    && glm::dot(stamp.to - t, stamp.to - t) < areaSq)
                    return false;
        }
    const uint64 key = seedBucketKey(team, home);
    m_seedBuckets[key].push_back(SeedStamp{ f, t, m_time }); // stamped even when the plan below
    m_seedExpiry.push_back(oc::pair<float, uint64>(m_time, key)); // fails: a hopeless pair must
    ++m_seedsThisFrame;                                          // not retry every frame
    return seedPath(team, from, to, speed, laneWidth, clearance);
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
    m_time += deltaSec;
    m_seedsThisFrame = 0;
    // Retire expired seed stamps: the queue is in time order and each bucket is append-only, so
    // the front of the queue always names the oldest stamp of that bucket. Nothing is scanned.
    while (!m_seedExpiry.empty() && m_time - m_seedExpiry.front().first >= m_seedCooldown)
    {
        const auto it = m_seedBuckets.find(m_seedExpiry.front().second);
        if (it != m_seedBuckets.end())
        {
            it->second.erase(it->second.begin()); // the oldest stamp in that bucket
            if (it->second.empty())
                m_seedBuckets.erase(it);
        }
        m_seedExpiry.pop_front();
    }
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

    // 3. Flow + pressure steps, PER-CHUNK parallel across ALL teams: each field's serial
    // pre-work (drain/flip/evict/gather/snapshot) runs here on main, then every team's chunks go
    // into ONE fan-out per phase — two barriers total instead of one per field, and one team's
    // lone big field no longer serializes behind the empty ones. The flow phase must fully settle
    // before the pressure phase: the pressure push splats ATOMICALLY into the flow write buffers,
    // which the flow tasks write plainly. Pressure PUSHES the flow (v += -grad p): a jam bends the
    // stream upstream of it and a seeded trough sucks the surrounding lanes in; the push rides the
    // diffusion step itself, which hands each active cell's gradient to the callback instead of a
    // second pass re-resolving neighbours.
    {
        ProfileScope pscope("Nav field steps", EProfileCategory::Game);
        const uint32 keepFrames = uint32(glm::max(m_keepFrames, 1));
        const TeamField* raster = m_raster.published.get();
        const float gain = pressureFlowGain();
        oc::vector<FlowField::StepItem> flowItems;
        oc::vector<PressureField::StepItem> pressureItems;
        PressureField::CellVisit pushes[MaxTeams];
        for (uint32 t = 0; t < MaxTeams; ++t)
        {
            m_flow[t].beginStep(deltaSec, keepFrames, m_flowHalfLife, raster, m_wallBounce,
                m_flowViscosity, m_flowMaxSpeed, m_flowViscosityFloor, m_flowViscosityBackflow,
                flowItems);
            FlowField& flow = m_flow[t];
            pushes[t] = [&flow, gain](const glm::vec2& centre, const glm::vec2& g)
            {
                if (glm::dot(g, g) > 1e-6f)
                    flow.splat(centre, -g * gain);
            };
            const size_t first = pressureItems.size();
            m_pressure[t].beginStep(deltaSec, raster, m_pressureDiffusion, m_pressureHalfLife,
                keepFrames, m_pressureFloor, pressureItems);
            if (gain > 0.0f)
                for (size_t i = first; i < pressureItems.size(); ++i)
                    pressureItems[i].push = &pushes[t];
        }
        struct Ctx
        {
            oc::vector<FlowField::StepItem>* flow;
            oc::vector<PressureField::StepItem>* pressure;
        };
        Ctx ctx{ &flowItems, &pressureItems };
        Globals::jobSystem.parallelFor(0u, uint32(flowItems.size()), 2u, [c = &ctx](uint32 begin, uint32 end)
        {
            ProfileScope scope("Nav flow step", EProfileCategory::Game);
            for (uint32 i = begin; i < end; ++i)
            {
                FlowField::StepItem& item = (*c->flow)[i];
                item.field->stepChunk(item.key, *item.chunk);
            }
        });
        Globals::jobSystem.parallelFor(0u, uint32(pressureItems.size()), 2u, [c = &ctx](uint32 begin, uint32 end)
        {
            ProfileScope scope("Nav pressure step", EProfileCategory::Game);
            for (uint32 i = begin; i < end; ++i)
            {
                PressureField::StepItem& item = (*c->pressure)[i];
                item.field->stepChunk(item.key, *item.chunk, item.push);
            }
        });
        for (PressureField& p : m_pressure)
            p.endStep();
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
    m_seedBuckets.clear();
    m_seedExpiry.clear();
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
                if (glm::abs(v) < 0.02f)
                    continue;
                const glm::vec2 c = cellCenter(base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits));
                if (glm::dot(c - f, c - f) > r2)
                    continue;
                // Log scale so a jam does not flatten the map; SIGNED: jams rise yellow->red,
                // seeded troughs hang below in cyan->blue.
                const float t = glm::clamp(std::log1p(glm::abs(v)) / std::log1p(20.0f), 0.0f, 1.0f);
                const uint32 col = v >= 0.0f ? packColor(1.0f, 1.0f - 0.8f * t, 0.1f)   // jam: yellow -> red
                                             : packColor(0.25f, 0.45f - 0.35f * t, 1.0f); // trough: light -> deep blue
                const float h = (0.3f + 4.0f * t) * (v >= 0.0f ? 1.0f : -0.35f); // troughs dip
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
        const uint32 foutline = packColor(0.15f, 0.45f, 0.2f); // flow chunks: dim green
        // Yardstick = the per-cell cap the field itself enforces ("Nav/Flow max"), so arrow length
        // and colour mean the same thing from frame to frame and place to place.
        const float flowScale = 1.0f / glm::max(m_flowMaxSpeed, 0.1f);
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
                if (mag < m_debugFlowMin) // a haze of near-zero arrows hides the lanes
                    continue;
                const glm::vec2 centre = cellCenter(base + glm::ivec2(i & (ChunkCells - 1), i >> ChunkBits));
                if (glm::dot(centre - f, centre - f) > r2)
                    continue;
                const float t = glm::clamp(mag * flowScale, 0.0f, 1.0f); // 1 = the strongest on screen
                const uint32 col = packColor(0.15f + 0.85f * t, 1.0f, 0.2f + 0.6f * t); // flow: green -> white
                const glm::vec2 dir = v / mag;
                const glm::vec2 side(-dir.y, dir.x);
                const float len = 0.5f + 1.1f * t;
                const float ya = y + 0.06f;
                const glm::vec2 tail = centre - dir * len * 0.4f;
                const glm::vec2 tip = centre + dir * len * 0.6f;
                // A line-list arrow is nearly invisible at RTS camera distance, so the shaft is
                // drawn as a THICK bar (parallel offsets) and the head as a filled triangle of
                // stacked chords — same primitive, an order of magnitude more readable.
                const float halfW = 0.05f + 0.05f * t;
                for (int o = -1; o <= 1; ++o)
                {
                    const glm::vec2 off = side * (float(o) * halfW);
                    line(glm::vec3(tail.x + off.x, ya, tail.y + off.y), glm::vec3(tip.x + off.x, ya, tip.y + off.y), col);
                }
                const float headLen = 0.28f + 0.22f * t;
                const float headHalf = 0.16f + 0.12f * t;
                const glm::vec2 baseL = tip - dir * headLen + side * headHalf;
                const glm::vec2 baseR = tip - dir * headLen - side * headHalf;
                line(glm::vec3(tip.x, ya, tip.y), glm::vec3(baseL.x, ya, baseL.y), col);
                line(glm::vec3(tip.x, ya, tip.y), glm::vec3(baseR.x, ya, baseR.y), col);
                line(glm::vec3(baseL.x, ya, baseL.y), glm::vec3(baseR.x, ya, baseR.y), col);
                for (int k = 1; k < 4; ++k) // chords across the head = a solid-looking triangle
                {
                    const float f2 = float(k) / 4.0f;
                    const glm::vec2 l = glm::mix(tip, baseL, f2), r = glm::mix(tip, baseR, f2);
                    line(glm::vec3(l.x, ya, l.y), glm::vec3(r.x, ya, r.y), col);
                }
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
