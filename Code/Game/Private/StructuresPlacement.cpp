module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Transform;
import Entity;
import Force;
import Spatial;
import :Structures;

// GRID PLACEMENT + the spawn/remove seams (see Structures.ixx): the snap, the footprints, the
// per-cell occupancy hash the derived links and every placement run on, cellsFree / planCrossing
// (validated at aim AND at apply - the MP seam), and the one spawn path every structure enters
// through (spawnStructure) with its bookkeeping counterpart (removeStructureBookkeeping).

// Per-type PREFABS - the only consumer is spawnStructure (the names/heights tables live in
// Structures.cpp behind structureTypeName / spawnHeightOf).
static constexpr const char* structurePrefabs[] = {
    "Entities/Game/emitter.pre", "Entities/Game/generator.pre", "Entities/Game/transmitter.pre",
    "Entities/Game/extractor.pre", "Entities/Game/battery.pre", "Entities/Game/fueltank.pre",
    "Entities/Game/solar.pre", "Entities/Game/fabricator.pre", "Entities/Game/bastion.pre",
    "Entities/Game/lance.pre", "Entities/Game/barracks.pre", "Entities/Game/barracksBrute.pre",
    "Entities/Game/barracksRunner.pre", "Entities/Game/barracksSpitter.pre", "Entities/Game/wall.pre",
    "Entities/Game/turret.pre", "Entities/Game/mineralstorage.pre", "Entities/Game/constructor.pre",
    "Entities/Game/base.pre", "Entities/Game/cablePower.pre", "Entities/Game/cablePipe.pre",
    "Entities/Game/cableConveyor.pre", "Entities/Game/crossing.pre", "Entities/Game/crossing.pre",
    "Entities/Game/crossing.pre", "Entities/Game/house.pre",   // the three crossings share one
    "Entities/Game/medic.pre" };                                // mesh; applyStructureTint hues it
static_assert(oc::size(structurePrefabs) == (size_t)EStructureType::Count);

// ---------------------------------------------------------------- grid placement

glm::vec3 StructureSystem::snapToGrid(EStructureType type, const glm::vec3& groundPos)
{
    // Odd footprints center on a CELL, even ones on a corner - either way the footprint covers
    // whole cells exactly.
    const float offset = (footprintCellsOf(type) & 1) ? GridCellSize * 0.5f : 0.0f;
    const auto snap = [&](float v) { return std::round((v - offset) / GridCellSize) * GridCellSize + offset; };
    return glm::vec3(snap(groundPos.x), 0.0f, snap(groundPos.z));
}

glm::ivec2 StructureSystem::footprintExtent(EStructureType t, const glm::quat& rot)
{
    if (!isCrossingType(t))
        return glm::ivec2(footprintCellsOf(t));
    // 1x3 along the facing, quantized to an axis at placement (entity -Z = forward).
    const glm::vec3 forward = rot * glm::vec3(0.0f, 0.0f, -1.0f);
    return glm::abs(forward.x) >= glm::abs(forward.z) ? glm::ivec2(3, 1) : glm::ivec2(1, 3);
}

bool StructureSystem::footprintAreaClear(EStructureType type, const glm::vec3& p, const glm::quat& rot) const
{
    const glm::ivec2 ext = footprintExtent(type, rot);
    const glm::vec2 half(ext.x * GridCellSize * 0.5f, ext.y * GridCellSize * 0.5f);
    if (m_hasBounds && (p.x - half.x < m_boundsMin.x - 1e-3f || p.x + half.x > m_boundsMax.x + 1e-3f
        || p.z - half.y < m_boundsMin.y - 1e-3f || p.z + half.y > m_boundsMax.y + 1e-3f))
        return false; // footprints stay fully inside the arena
    for (const glm::vec4& r : m_terrainBlocked) // co-op rock rects (minX, minZ, maxX, maxZ)
        if (p.x + half.x > r.x + 1e-3f && p.x - half.x < r.z - 1e-3f
            && p.z + half.y > r.y + 1e-3f && p.z - half.y < r.w - 1e-3f)
            return false; // impassable terrain: nothing builds in a rock (unit spawns skip it too)
    // FREE nodes reserve their future extractor's footprint (buildings can't block extraction);
    // extracted nodes rely on the standing extractor's own cells.
    if (type != EStructureType::Extractor)
    {
        const auto overlaps = [&](const glm::vec3& q, float halfB) {
            return glm::abs(p.x - q.x) < half.x + halfB - 1e-3f
                && glm::abs(p.z - q.z) < half.y + halfB - 1e-3f; };
        const float exHalf = footprintCellsOf(EStructureType::Extractor) * GridCellSize * 0.5f;
        for (const Node& n : m_nodes)
            if (!n.extracted
                && overlaps(snapToGrid(EStructureType::Extractor, glm::vec3(n.pos.x, 0.0f, n.pos.z)), exHalf))
                return false;
    }
    return true;
}

bool StructureSystem::cellsFree(EStructureType type, const glm::vec3& p, const glm::quat& rot,
    bool ignoreCables) const
{
    if (!footprintAreaClear(type, p, rot))
        return false;
    // PER-CELL occupancy (the hash the derived links run on): buildings refuse ANY occupied cell,
    // a cable may slot UNDER a crossing's free middle cell, a crossing may bridge OVER exactly one
    // cable in its middle cell. ignoreCables treats cable cells as open (unit-spawn probe).
    bool free = true;
    int cellIdx = -1;
    forEachFootprintCell(type, p, rot, [&](int cx, int cz)
    {
        ++cellIdx;
        if (!free)
            return;
        const auto it = m_cells.find(cellKey(cx, cz));
        if (it == m_cells.end())
            return; // empty cell
        const CellEntry& entry = it->second;
        const int occIdx = structureIndexById(entry.id);
        const EStructureType occ = occIdx >= 0 ? m_frame[occIdx].type : EStructureType::Emitter;
        if (ignoreCables && isWalkThrough(occ))
            return; // walk-through pieces do not block a unit spawn point
        if (isCableType(type))
        {
            // A cable may enter an existing crossing's MIDDLE cell (its center) while it is free.
            if (occIdx >= 0 && isCrossingType(occ) && entry.underId == 0 && isCrossingCenter(occIdx, cx, cz))
                return;
            free = false;
            return;
        }
        if (isCrossingType(type))
        {
            // The MIDDLE cell (index 1 of the 3) may bridge exactly one plain cable OR another
            // crossing's END cell (an end counts as cable for bridging); the ends must be free.
            const bool middle = cellIdx == 1;
            if (middle && occIdx >= 0 && entry.underId == 0 && isBridgeable(occIdx, cx, cz))
                return;
            free = false;
            return;
        }
        free = false; // buildings refuse any occupied cell (cables included - no building on a cable)
    });
    return free;
}

bool StructureSystem::actorInFootprint(EStructureType type, const glm::vec3& p)
{
    // A player capsule or a unit standing on the cells blocks the placement: the structure would
    // spawn inside them and the solver would fling whatever it engulfs. Checked at aim (red ghost)
    // AND in placeStructure (the MP seam) - actors move between the two.
    // Cables/crossings/solars are WALK-THROUGH (their collider ignores bodies), so standing on the
    // cells never blocks them.
    if (isWalkThrough(type))
        return false;
    constexpr float actorRadius = 0.7f; // capsule/unit body, generous by design
    const float half = footprintCellsOf(type) * GridCellSize * 0.5f + actorRadius;
    bool blocked = false;
    Globals::spatialIndex.forEachInSphere(glm::dvec3(p), half * 1.5f, SpatialLayer_Render, [&](uint64 user)
    {
        const Entity* entity = reinterpret_cast<const Entity*>(user);
        if (!hasComponent<GameUnitComponent>(entity)) // units AND player capsules (puppets)
            return;
        if (glm::abs(entity->pos.x - p.x) < half && glm::abs(entity->pos.z - p.z) < half)
            blocked = true;
    });
    return blocked;
}

// ---------------------------------------------------------------- spawn/destroy

int StructureSystem::spawnStructure(uint32 id, EStructureType type, const glm::vec3& pos,
    const glm::quat& rot, uint8 team, bool built, int nodeIndex)
{
    EntityPtr entity = Globals::world.spawnAssetFile(structurePrefabs[(int)type],
        Transform(pos, 1.0f, rot), true);
    if (!entity)
        return -1;
    GameStructureComponent* state = getComponent<GameStructureComponent>(entity.get());
    if (!state)
    {
        Log::warning(oc::string(structurePrefabs[(int)type])
            + " has no GameStructure component - placement refused");
        return -1;
    }
    entity->setName(structureTypeName(type));
    Globals::world.addRootEntity(entity);
    state->structureId = id;
    state->team = team;
    const float healthMax = isCableOrCrossing(type) ? m_cableHealthMax : m_structureHealthMax;
    state->healthMax = healthMax;
    state->blueprint = !built;
    state->health = built ? healthMax : 1.0f; // health IS the build progress
    if (ForceComponent* fc = getComponent<ForceComponent>(entity.get()))
        fc->emitter.setTeam(team); // prefabs author team 0 - the builder's team owns the field
    Ref ref;
    ref.owner = entity; // owning: the roster's raw pointers can never dangle
    ref.entity = entity.get();
    ref.state = state;
    ref.type = type;
    ref.nodeIndex = type == EStructureType::Extractor ? nodeIndex : -1;
    if (ref.nodeIndex >= 0 && ref.nodeIndex < (int)m_nodes.size())
        m_nodes[ref.nodeIndex].extracted = true;
    if (isCableType(type)) // cache the 4 render-only arm children (see updateArms)
    {
        static constexpr const char* armNames[4] = { "ArmPX", "ArmNX", "ArmPZ", "ArmNZ" };
        if (const SceneComponent* sc = getComponent<SceneComponent>(entity.get()))
            for (const EntityPtr& child : sc->children)
                for (int a = 0; a < 4; ++a)
                    if (oc::string_view(child->getName()) == armNames[a])
                        ref.arms[a] = child.get();
    }
    stampTuning(ref);
    applyStructureTint(ref); // blueprint gray until built
    m_nextStructureId = glm::max(m_nextStructureId, id + 1);
    m_byId[id] = (int)m_frame.size();
    m_frame.push_back(ref);
    insertCells(m_frame.back()); // the occupancy hash the derived links + placement run on
    m_linksDirty = true;
    return (int)m_frame.size() - 1;
}

// Deregister + full bookkeeping, WITHOUT touching the world's root list - shared by the game's own
// removal (destroyStructureAt) and by onWorldRootRemoved for out-of-band deletions.
void StructureSystem::removeStructureBookkeeping(size_t index)
{
    const Ref s = m_frame[index]; // owning copy: the entity stays alive through the bookkeeping
    const uint32 id = s.state->structureId;
    if (s.nodeIndex >= 0 && s.nodeIndex < (int)m_nodes.size())
        m_nodes[s.nodeIndex].extracted = false; // a removed extractor frees its node
    eraseCells(s); // promotes an under-cable back to primary occupant
    m_linksDirty = true; // the transport graph rebuilds before its next tick (roster pointers)
    m_frame.erase(m_frame.begin() + index);
    m_byId.clear();
    for (int i = 0; i < (int)m_frame.size(); ++i)
        m_byId[m_frame[i].state->structureId] = i;
    if (onStructureRemoved)
        onStructureRemoved(id);
}

void StructureSystem::destroyStructureAt(size_t index)
{
    Entity* entity = m_frame[index].entity;
    const EntityPtr keepAlive = m_frame[index].owner; // outlive the roster erase below
    removeStructureBookkeeping(index); // deregister FIRST: removeRootEntity's callback then no-ops
    Globals::world.removeRootEntity(entity);
}

// Any root leaving the world (editor delete, script destroy request - paths that never reach
// destroyStructureAt). Ours are deregistered by then, so this only fires for out-of-band removals.
void StructureSystem::onWorldRootRemoved(const Entity* entity)
{
    const int index = structureIndexByEntity(entity);
    if (index >= 0)
        removeStructureBookkeeping((size_t)index);
}

void StructureSystem::applyDemolishRequest(uint32 id, uint8 team)
{
    const int index = structureIndexById(id);
    if (index < 0)
        return; // already gone
    if (m_frame[index].type == EStructureType::Base)
        return; // the respawn anchor is not deletable
    if (m_frame[index].state->team != team)
        return; // only the owning team demolishes its structures
    Log::info(oc::string(structureTypeName(m_frame[index].type)) + " demolished");
    destroyStructureAt((size_t)index);
}

void StructureSystem::spawnBase(const glm::vec3& groundPos, uint8 team)
{
    // Same grid snap every placement gets (3x3 = odd footprint -> centered on a CELL): an
    // unsnapped Base sat half a cell off, so nothing placed next to it could line up flush.
    const glm::vec3 pos = snapToGrid(EStructureType::Base, groundPos)
        + glm::vec3(0.0f, spawnHeightOf(EStructureType::Base), 0.0f);
    const int index = spawnStructure(m_nextStructureId++, EStructureType::Base, pos,
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), team, /*built*/ true, -1);
    if (index >= 0) // the starting war chest + a FULL shield battery (it drains like any emitter)
    {
        m_frame[index].state->store[2] = glm::min(m_startMinerals, m_mineralBaseCapacity);
        m_frame[index].state->store[0] = m_frame[index].state->capacity[0];
    }
}

void StructureSystem::placeStructure(EStructureType type, const glm::vec3& groundPos, int nodeIndex,
    const glm::vec3& facing, uint8 team)
{
    if (!isPlaceableType(type) || (int)team >= GameMaxTeams)
        return; // the Base only enters through spawnBase; the Connector is retired
    // Orientation FIRST (a crossing's footprint depends on it), then the grid validation.
    // Lance: the AIMED facing from the two-click placement when given, else auto - away from the
    // own Base. Crossings: the facing quantized to an axis (default +X).
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    if (isCrossingType(type))
    {
        glm::vec2 dir(1.0f, 0.0f);
        if (glm::abs(facing.x) >= glm::abs(facing.z) && glm::abs(facing.x) > 1e-4f)
            dir = glm::vec2(facing.x > 0.0f ? 1.0f : -1.0f, 0.0f);
        else if (glm::abs(facing.z) > 1e-4f)
            dir = glm::vec2(0.0f, facing.z > 0.0f ? 1.0f : -1.0f);
        rot = glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    // GRID: every placement snaps (extractors snap the node's position too) and occupied cells
    // refuse - validated HERE, the MP seam, not just at aim time. A CROSSING goes through
    // planCrossing instead: its END cells may hold own-medium cables, which it REPLACES (below,
    // right before the spawn, so the cells are free when insertCells runs).
    const glm::vec3 snappedGround = snapToGrid(type, groundPos);
    CrossingPlan crossing;
    if (isCrossingType(type))
        crossing = planCrossing(type, snappedGround, rot, team);
    else
        crossing.valid = cellsFree(type, snappedGround, rot);
    if (!crossing.valid || actorInFootprint(type, snappedGround))
        return; // overlap raced the ghost - silently refused (it already showed red)
    if (type == EStructureType::Extractor)
    {
        if (nodeIndex < 0 || nodeIndex >= (int)m_nodes.size() || m_nodes[nodeIndex].extracted)
            return; // two same-frame requests for one node race - validated at apply
    }
    const glm::vec3 pos = snappedGround + glm::vec3(0.0f, spawnHeightOf(type), 0.0f);
    if (type == EStructureType::Lance)
    {
        glm::vec2 dir(0.0f);
        if (glm::dot(glm::vec2(facing.x, facing.z), glm::vec2(facing.x, facing.z)) > 1e-4f)
            dir = glm::normalize(glm::vec2(facing.x, facing.z));
        else
            for (const Ref& other : m_frame)
                if (other.type == EStructureType::Base && other.state->team == team)
                {
                    const glm::vec2 d = glm::vec2(pos.x, pos.z)
                        - glm::vec2(other.entity->pos.x, other.entity->pos.z);
                    if (glm::dot(d, d) > 1e-4f)
                        dir = glm::normalize(d);
                    break;
                }
        if (glm::dot(dir, dir) > 0.5f)
            rot = glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    // The crossing's END cells give way to it: the own-medium segments they held are redundant
    // (an end conducts that medium) and would refuse the placement. Their networks re-derive.
    for (const uint32 id : crossing.replace)
        if (id != 0)
            if (const int idx = structureIndexById(id); idx >= 0)
                destroyStructureAt((size_t)idx);
    // CHEAT ("Free instant build", Synced - the server's value rules): skip the blueprint phase.
    const int index = spawnStructure(m_nextStructureId++, type, pos, rot, team,
        /*built*/ m_cheatInstantBuild, type == EStructureType::Extractor ? nodeIndex : -1);
    if (index >= 0 && onStructurePlaced)
        onStructurePlaced(index);
}

// ---------------------------------------------------------------- cells + derived links

void StructureSystem::insertCells(const Ref& s)
{
    const uint32 id = s.state->structureId;
    forEachFootprintCell(s.type, s.entity->pos, s.entity->rot, [&](int cx, int cz)
    {
        CellEntry& entry = m_cells[cellKey(cx, cz)];
        if (entry.id == 0)
        {
            entry.id = id;
            return;
        }
        // Sharing is only ever a crossing's middle over a cable or over another crossing's end
        // (cellsFree enforced it). Whichever arrives
        // second, the CROSSING is the primary occupant and the cable rides underId.
        if (isCrossingType(s.type))
        {
            entry.underId = entry.id;
            entry.id = id;
        }
        else
            entry.underId = id;
    });
}

void StructureSystem::eraseCells(const Ref& s)
{
    const uint32 id = s.state->structureId;
    forEachFootprintCell(s.type, s.entity->pos, s.entity->rot, [&](int cx, int cz)
    {
        const auto it = m_cells.find(cellKey(cx, cz));
        if (it == m_cells.end())
            return;
        CellEntry& entry = it->second;
        if (entry.id == id)
        {
            if (entry.underId != 0) // the under-cable becomes the primary occupant again
            {
                entry.id = entry.underId;
                entry.underId = 0;
            }
            else
                m_cells.erase(it);
        }
        else if (entry.underId == id)
            entry.underId = 0;
    });
}

int StructureSystem::cableSegmentAt(int cx, int cz, bool builtOnly) const
{
    const auto it = m_cells.find(cellKey(cx, cz));
    if (it == m_cells.end())
        return -1;
    // The under-cable stands in when a crossing bridges the cell - which is also what keeps a
    // crossing from unioning with the cable passing under it (the crossing itself is not a cable).
    for (const uint32 id : { it->second.id, it->second.underId })
    {
        if (id == 0)
            continue;
        const int index = structureIndexById(id);
        if (index >= 0 && isCableType(m_frame[index].type)
            && (!builtOnly || !m_frame[index].state->blueprint))
            return index;
    }
    return -1;
}

bool StructureSystem::isCrossingCenter(int index, int cx, int cz) const
{
    const glm::vec3& p = m_frame[index].entity->pos;
    return (int)std::lround(p.x / GridCellSize - 0.5f) == cx
        && (int)std::lround(p.z / GridCellSize - 0.5f) == cz;
}

bool StructureSystem::isBridgeable(int index, int cx, int cz) const
{
    const EStructureType t = m_frame[index].type;
    return isCableType(t) || (isCrossingType(t) && !isCrossingCenter(index, cx, cz));
}

int StructureSystem::bridgeableAt(const glm::vec3& p) const
{
    const int cx = (int)std::lround(p.x / GridCellSize - 0.5f);
    const int cz = (int)std::lround(p.z / GridCellSize - 0.5f);
    const auto it = m_cells.find(cellKey(cx, cz));
    if (it == m_cells.end() || it->second.underId != 0)
        return -1;
    const int index = structureIndexById(it->second.id);
    return index >= 0 && isBridgeable(index, cx, cz) ? index : -1;
}

StructureSystem::CrossingPlan StructureSystem::planCrossing(EStructureType type, const glm::vec3& p,
    const glm::quat& rot, uint8 team) const
{
    CrossingPlan plan;
    if (!isCrossingType(type) || !footprintAreaClear(type, p, rot))
        return plan;
    const int medium = crossingMediumOf(type);
    int cellIdx = -1;
    int replaceCount = 0;
    bool blocked = false;
    forEachFootprintCell(type, p, rot, [&](int cx, int cz)
    {
        ++cellIdx;
        if (blocked)
            return;
        const auto it = m_cells.find(cellKey(cx, cz));
        if (it == m_cells.end())
            return; // empty cell
        const CellEntry& entry = it->second;
        const int occIdx = structureIndexById(entry.id);
        if (occIdx < 0 || entry.underId != 0)
        {
            blocked = true; // a stale id, or a cell that already bridges something
            return;
        }
        if (cellIdx == 1) // the MIDDLE bridges one plain cable or another crossing's END
        {
            blocked = !isBridgeable(occIdx, cx, cz);
            return;
        }
        // An END: only this crossing's OWN medium as a plain cable, own team - that segment is
        // redundant under the end and gets replaced. Anything else blocks.
        if (cableMediumOf(m_frame[occIdx].type) == medium && m_frame[occIdx].state->team == team)
            plan.replace[replaceCount++] = entry.id;
        else
            blocked = true;
    });
    plan.valid = !blocked;
    return plan;
}
