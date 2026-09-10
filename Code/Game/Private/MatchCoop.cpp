module Game;

import Core;
import Core.glm;
import Core.Log;
import Entity;
import Network;
import Nav;
import Threading; // the ambient wander rides the post-update batch
import File; // AssetNode (the trickle save state)
import :Match;
import :Structures;
import :Npc;

// ---- CO-OP director -------------------------------------------------------------------------
// Waves, the spawn trickle, the ambient scatter and its wander — authority only (clients never
// spawn). See the Match.ixx CO-OP section for the state.

// Wave ARCHETYPES: every wave rolls ONE recipe — a named mix of up to 4 unit types — gated by the
// wave index so early waves stay simple and the heavy stuff unlocks over time. Single-type rushes
// and combined-arms mixes both happen, but never every type in every wave; queueWave jitters the
// picked recipe's weights so two waves of the same archetype still differ.
namespace
{
    struct WaveArchetype
    {
        const char* name;
        int minWave; // first wave index this recipe can roll (m_waveIndex is 1-based at roll time)
        struct { ENpcType type; float weight; } mix[5]; // weight 0 = unused slot
    };
    // The ELITE tier (Elite/Giant/Titan/Lobber) enters at wave 7 and dominates from ~10 on: the
    // Lobber's splash shells punish packed defences, the Giant/Titan soak turret fire, and the
    // budget growth is what lets a late wave afford a Titan next to its escort.
    constexpr WaveArchetype c_waveArchetypes[] = {
        { "swarm",           1, { { ENpcType::Swarm, 1.0f } } },
        { "swarm + runners", 2, { { ENpcType::Swarm, 0.75f }, { ENpcType::Runner, 0.25f } } },
        { "grunt push",      2, { { ENpcType::Grunt, 0.65f }, { ENpcType::Swarm, 0.35f } } },
        { "runner rush",     3, { { ENpcType::Runner, 1.0f } } },
        { "warrior line",    4, { { ENpcType::Warrior, 0.4f }, { ENpcType::Grunt, 0.3f },
                                  { ENpcType::Swarm, 0.3f } } },
        { "shield wall",     6, { { ENpcType::Warrior, 0.6f }, { ENpcType::Spitter, 0.15f },
                                  { ENpcType::Swarm, 0.25f } } },
        { "spitter siege",   4, { { ENpcType::Spitter, 0.3f }, { ENpcType::Swarm, 0.7f } } },
        { "brute hammer",    5, { { ENpcType::Brute, 0.25f }, { ENpcType::Swarm, 0.75f } } },
        { "combined arms",   6, { { ENpcType::Grunt, 0.3f }, { ENpcType::Runner, 0.25f },
                                  { ENpcType::Spitter, 0.2f }, { ENpcType::Swarm, 0.25f } } },
        { "brute wall",      7, { { ENpcType::Brute, 0.85f }, { ENpcType::Spitter, 0.15f } } },
        { "elite guard",     7, { { ENpcType::Elite, 0.45f }, { ENpcType::Grunt, 0.25f },
                                  { ENpcType::Swarm, 0.3f } } },
        { "the works",       8, { { ENpcType::Swarm, 0.4f }, { ENpcType::Runner, 0.25f },
                                  { ENpcType::Spitter, 0.15f }, { ENpcType::Brute, 0.2f } } },
        { "lobber barrage",  8, { { ENpcType::Lobber, 0.3f }, { ENpcType::Elite, 0.2f },
                                  { ENpcType::Swarm, 0.5f } } },
        { "giant push",      9, { { ENpcType::Giant, 0.15f }, { ENpcType::Elite, 0.35f },
                                  { ENpcType::Swarm, 0.5f } } },
        { "siege column",   10, { { ENpcType::Giant, 0.2f }, { ENpcType::Lobber, 0.3f },
                                  { ENpcType::Brute, 0.2f }, { ENpcType::Swarm, 0.3f } } },
        { "titan",          11, { { ENpcType::Titan, 0.05f }, { ENpcType::Giant, 0.15f },
                                  { ENpcType::Lobber, 0.2f }, { ENpcType::Swarm, 0.6f } } },
        { "endgame",        13, { { ENpcType::Titan, 0.1f }, { ENpcType::Giant, 0.2f },
                                  { ENpcType::Elite, 0.3f }, { ENpcType::Lobber, 0.2f },
                                  { ENpcType::Swarm, 0.2f } } },
        { "hive",            9, { { ENpcType::Spawner, 0.1f }, { ENpcType::Elite, 0.3f },
                                  { ENpcType::Swarm, 0.6f } } },
        { "hive siege",     12, { { ENpcType::Spawner, 0.15f }, { ENpcType::Giant, 0.15f },
                                  { ENpcType::Lobber, 0.2f }, { ENpcType::Swarm, 0.5f } } },
    };
    constexpr int c_numWaveArchetypes = (int)(sizeof(c_waveArchetypes) / sizeof(c_waveArchetypes[0]));
    constexpr int c_maxArchetypeMinWave = [] {
        int m = 1;
        for (const WaveArchetype& a : c_waveArchetypes)
            m = a.minWave > m ? a.minWave : m;
        return m;
    }();

    // One unit from an archetype's authored mix (the ambient scatter's per-spawn roll — the wave
    // path samples its stored, jittered copy instead).
    ENpcType sampleArchetype(const WaveArchetype& arch)
    {
        float total = 0.0f;
        for (const auto& e : arch.mix)
            total += e.weight;
        float r = glm::linearRand(0.0f, glm::max(total, 1e-3f));
        for (const auto& e : arch.mix)
        {
            r -= e.weight;
            if (e.weight > 0.0f && r <= 0.0f)
                return e.type;
        }
        return ENpcType::Swarm;
    }
}

ENpcType GameMatch::sampleMix(const oc::fixed_vector<WaveMixEntry, 4>& mix) const
{
    float total = 0.0f;
    for (const WaveMixEntry& e : mix)
        total += e.weight;
    if (total <= 0.0f)
        return ENpcType::Swarm; // no rolled mix (shouldn't happen): the classic mass
    float r = glm::linearRand(0.0f, total);
    for (const WaveMixEntry& e : mix)
    {
        r -= e.weight;
        if (r <= 0.0f)
            return e.type;
    }
    return mix.back().type;
}

// The wave clock (authority, co-op only). The actual entity spawns are TRICKLED by tickCoopSpawns
// — a They-are-Billions-sized wave materialized in one frame would be a multi-hundred-spawn hitch.
void GameMatch::tickWaves(float deltaSec)
{
    m_waveTimer -= deltaSec;
    if (m_waveTimer > 0.0f)
        return;
    m_waveTimer = m_waveInterval;
    queueWave();
}

// Live units — ambient and previous waves alike (player-team units too): the component's own
// count, kept at its spawn / destroy edges.
int GameMatch::aiAliveCount() const
{
    return NpcSystem::countUnits();
}

// The NEXT wave's budget in points, before the "Max enemy units" cap: base + growth per wave so
// far, where the growth itself climbs by "Wave growth growth" every wave —
// base + growth*i + growthGrowth * (0 + 1 + ... + (i-1)) for the 0-based wave index i.
float GameMatch::nextWaveBudget() const
{
    const float wave = (float)m_waveIndex;
    return (float)m_waveBudget + m_waveBudgetGrowth * wave + m_waveGrowthGrowth * wave * (wave - 1.0f) * 0.5f;
}

// The wave blob's radius for `budget` points of the CURRENT mix. Area — not radius — scales with
// the expected body count, so the areal density is the same in wave 1 and wave 20: the old formula
// grew the radius LINEARLY off the REMAINING budget and capped at 30 m, which meant a big wave
// stacked hundreds of bodies on one disc (parked bodies do not push apart, so the pile only
// resolved as physics shoved them out once a player came near) AND the disc SHRANK as the wave
// trickled, packing the tail tightest.
//
// The blob is clamped to the ground plane and any point inside the barrier square is pushed back
// out (see tickCoopSpawns), so an oversized disc becomes a wide BAND hugging the barrier — the
// only direction with room, since the spawn band is just c_coopGroundEdge - c_coopHalfSize deep.
float GameMatch::waveSpawnRadius(float budget) const
{
    float weight = 0.0f, cost = 0.0f;
    for (const WaveMixEntry& e : m_waveMix)
    {
        weight += e.weight;
        cost += e.weight * waveCostOf(e.type);
    }
    const float meanCost = weight > 0.0f ? glm::max(cost / weight, 0.1f) : 1.0f;
    const float bodies = glm::max(budget, 0.0f) / meanCost;
    const float area = bodies * glm::max(m_waveSpawnAreaPerUnit, 1.0f);
    // 8 m floor keeps a handful of bodies from spawning on one spot; the ceiling is the world.
    return glm::clamp(std::sqrt(area / glm::pi<float>()), 8.0f, c_coopGroundEdge);
}

void GameMatch::queueWave()
{
    const int aiAlive = aiAliveCount(); // ambient + previous waves both count against the cap
    // BUDGET points, not a unit count: the alive cap converts conservatively at the CHEAPEST cost
    // (the worst-case body count a budget could buy).
    float cheapest = FLT_MAX;
    for (int t = 0; t < (int)ENpcType::Count; ++t)
        cheapest = glm::min(cheapest, waveCostOf((ENpcType)t));
    // Wave i (0-based) = base + growth per wave so far, where the growth itself climbs by "Wave
    // growth growth" every wave: base + growth*i + growthGrowth * (0 + 1 + ... + (i-1)).
    const float budget = glm::min(nextWaveBudget(),
        (float)(m_waveMaxAlive - aiAlive) * cheapest - m_ambientPendingBudget - m_wavePendingBudget);
    ++m_waveIndex;
    if (budget <= 0.0f)
        return; // at the cap: the clock (and the scaling) still advanced
    // A random compass direction, projected onto the barrier square and stepped OUTSIDE it: the
    // swarm clusters in the open ring beyond the barrier (its collider only matches players) and
    // pushes at the Base's near face (a point INSIDE the footprint would fail the A* and the move
    // order alike — the pointOutsideFootprint rule). The locked order releases on arrival; the AI
    // takes over there. The carved lanes guarantee a route in from any side.
    const float angle = glm::linearRand(0.0f, glm::two_pi<float>());
    const glm::vec3 dir(std::cos(angle), 0.0f, std::sin(angle));
    const float k = (c_coopHalfSize + 8.0f) / glm::max(glm::abs(dir.x), glm::abs(dir.z));
    m_waveOrigin = dir * k;
    m_waveDest = dir * 6.0f;
    m_wavePendingBudget += budget;
    // Roll this wave's COMPOSITION among the archetypes the index has unlocked — never the same
    // recipe twice in a row when a choice exists — then jitter its weights so repeats still vary.
    int eligible[c_numWaveArchetypes];
    int numEligible = 0;
    for (int i = 0; i < c_numWaveArchetypes; ++i)
        if (m_waveIndex >= c_waveArchetypes[i].minWave && (i != m_lastArchetype || numEligible == 0))
            eligible[numEligible++] = i;
    if (numEligible > 1 && eligible[0] == m_lastArchetype) // slot 0 was only a can't-be-empty seed
    {
        eligible[0] = eligible[numEligible - 1];
        --numEligible;
    }
    const int pick = eligible[glm::clamp((int)(glm::linearRand(0.0f, 1.0f) * (float)numEligible),
        0, numEligible - 1)];
    m_lastArchetype = pick;
    m_waveMix.clear();
    for (const auto& entry : c_waveArchetypes[pick].mix)
        if (entry.weight > 0.0f)
            m_waveMix.push_back({ entry.type, entry.weight * glm::linearRand(0.6f, 1.4f) });
    // Size the blob for EVERYTHING still queued (a previous wave's tail included) — once, with the
    // mix that will spend it, so the density holds from the first body to the last.
    m_waveRadius = waveSpawnRadius(m_wavePendingBudget);
    // One planned lane from the spawn ring to the Base — the swarm commits to it, and the units'
    // own periodic seed requests keep it fresh (Nav's proximity dedup makes the wave one plan).
    Globals::navSystem.seedPath(CoopAiTeam, m_waveOrigin, m_waveDest, laneSeedSpeed(), laneSeedWidth());
    Log::info(oc::format("Co-op: wave {} incoming — budget {:.0f} ({}) from ({:.0f}, {:.0f})", m_waveIndex,
        budget, c_waveArchetypes[pick].name, m_waveOrigin.x, m_waveOrigin.z));
    if (m_isServer)
    {
        uint8 buffer[4];
        NetWriter writer(buffer);
        writer.write<uint16>((uint16)m_waveIndex);
        Globals::networkManager.fireNetworkEvent("GWv", writer.data());
    }
}

// The trickle spawner (authority, per frame): a fixed budget of entity spawns serves the wave
// first, then the world-start ambient scatter — thousands of units enter the world over a few
// seconds instead of one giant frame hitch.
void GameMatch::tickCoopSpawns()
{
    // The frame's spawns are ROLLED first (budget math + RNG stay serial on main — glm's linearRand
    // is not thread-safe), then materialized in ONE NpcSystem::spawnLooseUnits batch: the entity
    // creations fan out over the job system instead of running one by one.
    ProfileScope scope("Coop spawn trickle", EProfileCategory::Game);
    oc::small_vector<NpcSystem::LooseSpawn, 64> spawns;
    int budget = glm::max(m_spawnsPerFrame, 1);
    while (budget > 0 && m_wavePendingBudget > 0.0f)
    {
        --budget;
        // Roll the type from the wave's mix, then SPEND its cost. A roll the remaining budget
        // can't afford downgrades to the cheapest type in the mix; if even that doesn't fit, the
        // wave is done (the tail rounds down instead of overspending into the next wave's points).
        ENpcType type = sampleMix(m_waveMix);
        if (waveCostOf(type) > m_wavePendingBudget)
        {
            for (const WaveMixEntry& e : m_waveMix)
                if (waveCostOf(e.type) < waveCostOf(type))
                    type = e.type;
            if (waveCostOf(type) > m_wavePendingBudget)
            {
                m_wavePendingBudget = 0.0f;
                break;
            }
        }
        m_wavePendingBudget -= waveCostOf(type);
        // Cluster around the ring point in the blob queueWave sized for THIS wave's body count
        // (m_waveRadius — constant for the whole trickle, so the tail is as loose as the head).
        // The blob stays in the band OUTSIDE the barrier but inside the ground plane: a point that
        // drifted through the barrier line pushes back out along the wave's dominant axis, which
        // is what turns a big wave's oversized disc into a wide band along the barrier face.
        // A few rolls against the recent spawn points: a spot inside a body placed a moment ago
        // is rejected (parked bodies never push each other apart until a player is near).
        constexpr float c_spawnSpacing = 2.0f; // > two grunt radii
        glm::vec3 pos;
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            const float a = glm::linearRand(0.0f, glm::two_pi<float>());
            const float r = m_waveRadius * std::sqrt(glm::linearRand(0.0f, 1.0f)); // uniform by area
            pos = m_waveOrigin + glm::vec3(std::cos(a) * r, 1.0f, std::sin(a) * r);
            pos.x = glm::clamp(pos.x, -c_coopGroundEdge, c_coopGroundEdge);
            pos.z = glm::clamp(pos.z, -c_coopGroundEdge, c_coopGroundEdge);
            if (glm::abs(pos.x) < c_coopHalfSize + 1.5f && glm::abs(pos.z) < c_coopHalfSize + 1.5f)
            {
                if (glm::abs(m_waveOrigin.x) >= glm::abs(m_waveOrigin.z))
                    pos.x = glm::sign(m_waveOrigin.x) * (c_coopHalfSize + 2.0f + glm::linearRand(0.0f, 10.0f));
                else
                    pos.z = glm::sign(m_waveOrigin.z) * (c_coopHalfSize + 2.0f + glm::linearRand(0.0f, 10.0f));
            }
            bool clear = true;
            for (int i = 0; i < m_waveRecentCount && clear; ++i)
                clear = glm::distance(m_waveRecent[i], glm::vec2(pos.x, pos.z)) >= c_spawnSpacing;
            if (clear)
                break;
        }
        m_waveRecent[m_waveRecentNext] = glm::vec2(pos.x, pos.z);
        m_waveRecentNext = (m_waveRecentNext + 1) % c_waveRecentSpawns;
        m_waveRecentCount = glm::min(m_waveRecentCount + 1, c_waveRecentSpawns);
        spawns.push_back({ .pos = pos,
            .orderDest = m_waveDest + glm::vec3(glm::linearRand(-4.0f, 4.0f), 0.0f,
                glm::linearRand(-4.0f, 4.0f)),
            .type = type, .team = (uint8)CoopAiTeam, .hasOrder = true });
    }
    while (budget > 0 && m_ambientPendingBudget > 0.0f)
    {
        --budget;
        // AMBIENT units come in small GROUPS: a cluster anchored on a random REACHABLE open cell
        // of the generated map (uniform by area, outside the safe ring, reachable from the Base
        // by the flood fill's guarantee), its bodies scattered in a disc around the anchor and
        // slid off any rock edge — a loose blob per group instead of one body per cell, which
        // lined up on the 10 m lattice. The Nav team fields never pull them (they cover the whole
        // map, which marched every scattered unit to the base) — only the local search aggroes
        // them, so an ambient group holds its patch until players expand near it. Wave units
        // above stay field-driven after their order releases.
        if (m_coopMap.reachable.empty())
            break;
        if (m_ambientSpawn.remaining <= 0)
        {
            const int cell = m_coopMap.reachable[glm::clamp(
                (int)(glm::linearRand(0.0f, 1.0f) * (float)m_coopMap.reachable.size()),
                0, (int)m_coopMap.reachable.size() - 1)];
            const glm::vec3 center = coopCellCenter(cell);
            if (glm::length(glm::vec2(center.x, center.z)) < m_ambientSafeRadius)
                continue; // safe-ring reject: costs one budget tick, never the points
            // DISTANCE = DIFFICULTY: the group's archetype is gated by GEODESIC depth (BFS
            // distance from the Base over the generated map) exactly like waves gate by index —
            // the BAND is a window of recipes: a group rolls only recipes whose wave gate sits
            // within "Ambient recipe window" BELOW its depth band, so the near ring only rolls the
            // early recipes (swarm-grade) and the deep map rolls ONLY the elite-tier ones (giants,
            // titans, lobbers never appear near the Base, and the outer map never wastes a group
            // on a plain swarm). Costs then make far groups FEWER, TOUGHER bodies for the same
            // points. One recipe per group, so it reads as a unit type holding ground rather than
            // a random assortment.
            // "Ambient depth scale" < 1 reaches the top band before the map's deepest cell, so the
            // final tier (titans) is not confined to the corners: at 0.9 the mid-edges qualify.
            const float depth = glm::min((float)m_coopMap.depth[cell]
                / ((float)m_coopMap.maxDepth * m_ambientDepthScale), 1.0f);
            const int band = 1 + (int)(depth * (float)(c_maxArchetypeMinWave - 1) + 0.5f);
            int eligible[c_numWaveArchetypes];
            int numEligible = 0;
            for (int i = 0; i < c_numWaveArchetypes; ++i)
                if (c_waveArchetypes[i].minWave <= band
                    && c_waveArchetypes[i].minWave >= band - m_ambientRecipeWindow)
                    eligible[numEligible++] = i;
            if (numEligible == 0) // a gap in the gate table under the window: fall back to all unlocked
                for (int i = 0; i < c_numWaveArchetypes; ++i)
                    if (c_waveArchetypes[i].minWave <= band)
                        eligible[numEligible++] = i;
            m_ambientSpawn.archetype = eligible[glm::clamp(
                (int)(glm::linearRand(0.0f, 1.0f) * (float)numEligible), 0, numEligible - 1)];
            m_ambientSpawn.center = center + glm::vec3(glm::linearRand(-4.0f, 4.0f), 0.0f,
                glm::linearRand(-4.0f, 4.0f));
            m_ambientSpawn.radius = glm::linearRand(4.0f, 9.0f);
            m_ambientSpawn.remaining = (int)glm::linearRand(3.0f, 9.99f);
        }
        --m_ambientSpawn.remaining;
        ENpcType type = sampleArchetype(c_waveArchetypes[m_ambientSpawn.archetype]);
        if (waveCostOf(type) > m_ambientPendingBudget)
        {
            type = ENpcType::Swarm; // the tail rounds down to the cheapest body
            if (waveCostOf(type) > m_ambientPendingBudget)
            {
                m_ambientPendingBudget = 0.0f;
                break;
            }
        }
        m_ambientPendingBudget -= waveCostOf(type);
        spawns.push_back({ .pos = ambientPointNear(m_ambientSpawn.center, m_ambientSpawn.radius),
            .type = type, .team = (uint8)CoopAiTeam });
    }
    m_npcs.spawnLooseUnits(oc::span<const NpcSystem::LooseSpawn>(spawns.data(), spawns.size()));
}

// AMBIENT WANDER (co-op authority): an IDLE AI unit now and then takes a short stroll, its heading
// biased toward the Base. Performance: the EXPECTED number of strolls this frame is unit count /
// "Ambient wander interval" × dt (5000 units at 90 s and 60 fps = ~0.93 a frame), carried as a
// fractional budget so every frame issues that many on average — a steady trickle instead of a
// burst — and each stroll costs at most c_wanderScanCap random probes into the World's root list
// (every unit is a root; a non-unit hit is a wasted probe) to find an idle unit. No per-unit
// timer. The order is a wander (orderWander): it never seeds a lane and
// self-expires after "Ambient wander timeout", so a target behind rock cannot pin the unit. Far
// (LOD-skipped) units walk it through the far tick's teleport like any order. Hunting, locked or
// routing units are skipped as candidates. Runs on a POST-UPDATE job (see update): its own RNG,
// since the shared C rand is not a thing to share with main.
void GameMatch::tickAmbientWander(float deltaSec)
{
    if (!m_coop || m_ambientWanderInterval <= 0.0f)
        return;
    // The root list only mutates on main, after this post-update job joins.
    const oc::vector<EntityPtr>& roots = Globals::world.rootEntities();
    const int n = (int)roots.size();
    const int aliveUnits = NpcSystem::countUnits(); // the component's edge-maintained count (any thread)
    if (n == 0 || aliveUnits == 0)
        return;
    const auto rand01 = [&] { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_wanderRng); };
    // The budget is sized from the SELECTED units, not all of them: only selected units are
    // candidates, so a whole-population budget would land every far unit's strolls on the few
    // near a player. The selected fraction is estimated from the random unit probes below (each
    // is a fair sample), smoothed — no walk.
    m_wanderBudget += (float)aliveUnits * m_wanderSelectedFrac * deltaSec / m_ambientWanderInterval;
    int issue = (int)m_wanderBudget;
    if (issue <= 0)
        return;
    m_wanderBudget -= (float)issue;
    constexpr int c_wanderScanCap = 32; // random probes per stroll before giving up this frame
    glm::vec3 basePos(0.0f);
    for (int i = 0; i < m_structures.structureCount(); ++i)
        if (m_structures.structureType(i) == EStructureType::Base && m_structures.structureTeam(i) == 0)
        {
            basePos = m_structures.structurePos(i);
            break;
        }
    for (; issue > 0; --issue)
    {
        // Pre-emption point every 16 strolls (a stroll is up to 32 probes plus a ground clamp):
        // a Normal post-update job, so High work gets through. A stroll is issued whole.
        if ((issue & 15) == 0)
            Globals::jobSystem.preemptionPoint();
        GameUnitComponent* u = nullptr;
        Entity* e = nullptr;
        for (int scan = 0; scan < c_wanderScanCap && !u; ++scan)
        {
            e = roots[glm::clamp((int)(rand01() * (float)n), 0, n - 1)].get();
            u = e ? getComponent<GameUnitComponent>(e) : nullptr;
            if (!u || u->puppet)
            {
                u = nullptr; // not a unit (structure, rock, shot, player capsule): no sample either
                continue;
            }
            // Only SELECTED units (inside the SIM LOD's outer tier of some player) stroll: a far
            // unit is invisible and would walk its order by the far tick's teleport, then arrive
            // in view mid-stroll — a whole patch "starting to wander" the moment a player came
            // near. Tier 2 and closer all behave alike.
            const bool selected = Globals::world.simLodSelected(*e);
            m_wanderSelectedFrac += ((selected ? 1.0f : 0.0f) - m_wanderSelectedFrac) * 0.02f;
            if (u->team != CoopAiTeam || !u->alive() || u->targetLocked || u->hasTarget
                || u->routeIndex < u->routeCount || !selected)
                u = nullptr;
        }
        if (!u)
            continue; // no idle unit found in the probe budget: this stroll is simply dropped
        const float angle = rand01() * 6.2831853f;
        glm::vec2 dir(glm::cos(angle), glm::sin(angle));
        const glm::vec2 toBase(basePos.x - e->pos.x, basePos.z - e->pos.z);
        if (const float len = glm::length(toBase); len > 1e-3f)
            dir += toBase / len * m_ambientWanderBaseBias; // the bias tilts the stroll toward the Base
        if (glm::dot(dir, dir) < 1e-4f)
            continue;
        dir = glm::normalize(dir);
        const float dist = (0.4f + 0.6f * rand01()) * m_ambientWanderDistance;
        const glm::vec3 target = clampToOpenGround(e->pos + glm::vec3(dir.x, 0.0f, dir.y) * dist);
        u->orderWander(target, m_ambientWanderTimeout);
    }
}

// ---- the trickle's save state --------------------------------------------------------------

// The PENDING TRICKLE: a wave is sized in points at queueWave and the bodies materialize over the
// following frames (tickCoopSpawns, "Spawns per frame"). Saving mid-wave used to drop everything
// not yet spawned, so an F9 during a big wave quietly shrank it. Everything the trickle reads is
// written here — the remaining points, WHERE they enter (m_waveOrigin) and march (m_waveDest), and
// the wave's ROLLED, JITTERED mix, which cannot be re-derived from the archetype table. The
// spacing ring (m_waveRecent) is deliberately left out: it only rejects spots for a few spawns.
void GameMatch::saveTrickle(AssetNode& root) const
{
    AssetNode& wave = root.addChild("WaveTrickle");
    wave.set("Pending", m_wavePendingBudget);
    wave.set("Origin", m_waveOrigin);
    wave.set("Dest", m_waveDest);
    wave.set("Radius", m_waveRadius); // the blob queueWave sized for this wave's body count
    wave.set("LastArchetype", oc::to_string(m_lastArchetype)); // keeps "never twice in a row"
    for (const WaveMixEntry& e : m_waveMix)
    {
        AssetNode& entry = wave.addChild("Mix"); // Type is the raw ENpcType int — append-only
        entry.values = { oc::to_string((int)e.type), oc::to_string(e.weight) };
    }
    // The world-start SCATTER trickles through the same budget, plus the group it is mid-way
    // through placing (anchored on this map's cells, which the load regenerates identically).
    AssetNode& ambient = root.addChild("AmbientTrickle");
    ambient.set("Pending", m_ambientPendingBudget);
    ambient.set("Center", m_ambientSpawn.center);
    ambient.set("Radius", m_ambientSpawn.radius);
    ambient.set("Remaining", oc::to_string(m_ambientSpawn.remaining));
    ambient.set("Archetype", oc::to_string(m_ambientSpawn.archetype));
}

// Counterpart of saveTrickle. MUST run after rebuildCoopMap, which voids the in-progress ambient
// group (its anchor belongs to the old map). A save with NO trickle nodes — every save before this
// existed — clears both budgets: that file IS the complete state, and letting the running session's
// own scatter continue on top of it would double-spawn.
void GameMatch::loadTrickle(const AssetNode& root)
{
    m_wavePendingBudget = 0.0f;
    m_ambientPendingBudget = 0.0f;
    m_ambientSpawn.remaining = 0;
    if (const AssetNode* wave = root.find("WaveTrickle"))
    {
        m_wavePendingBudget = glm::max(wave->find("Pending") ? wave->find("Pending")->asFloat() : 0.0f, 0.0f);
        m_waveOrigin = wave->find("Origin") ? wave->find("Origin")->asVec3() : m_waveOrigin;
        m_waveDest = wave->find("Dest") ? wave->find("Dest")->asVec3() : m_waveDest;
        m_lastArchetype = wave->find("LastArchetype") ? wave->find("LastArchetype")->asInt() : -1;
        m_waveMix.clear();
        for (const AssetNode* entry : wave->findAll("Mix"))
        {
            if (m_waveMix.size() >= m_waveMix.capacity())
                break; // fixed_vector: a hand-edited save cannot overrun it
            const int type = glm::clamp(entry->asInt(0), 0, (int)ENpcType::Count - 1);
            m_waveMix.push_back({ (ENpcType)type, glm::max(entry->asFloat(1), 0.0f) });
        }
        // Older trickle saves have no Radius: re-derive it from what is LEFT, which is the best
        // this end can do — the original wave's total is not in the file.
        m_waveRadius = wave->find("Radius") ? glm::clamp(wave->find("Radius")->asFloat(), 8.0f, c_coopGroundEdge)
                                            : waveSpawnRadius(m_wavePendingBudget);
        if (m_waveMix.empty())
            m_wavePendingBudget = 0.0f; // nothing to sample: dropping the points beats a wrong mix
        else if (m_wavePendingBudget > 0.0f) // re-seed the lane the trickled units will follow
            Globals::navSystem.seedPath(CoopAiTeam, m_waveOrigin, m_waveDest, laneSeedSpeed(), laneSeedWidth());
    }
    if (const AssetNode* ambient = root.find("AmbientTrickle"))
    {
        m_ambientPendingBudget = glm::max(ambient->find("Pending") ? ambient->find("Pending")->asFloat() : 0.0f, 0.0f);
        m_ambientSpawn.center = ambient->find("Center") ? ambient->find("Center")->asVec3() : glm::vec3(0.0f);
        m_ambientSpawn.radius = glm::max(ambient->find("Radius") ? ambient->find("Radius")->asFloat() : 6.0f, 0.0f);
        m_ambientSpawn.remaining = glm::max(ambient->find("Remaining") ? ambient->find("Remaining")->asInt() : 0, 0);
        m_ambientSpawn.archetype = glm::clamp(ambient->find("Archetype") ? ambient->find("Archetype")->asInt() : 0,
            0, c_numWaveArchetypes - 1);
    }
}
