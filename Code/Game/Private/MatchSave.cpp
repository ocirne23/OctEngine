module Game;

import Core;
import Core.glm;
import Core.Log;
import Entity;
import Nav;
import File; // AssetNode + loadAssetFile/writeAssetText (game save/load)
import :Match;
import :Player;
import :Structures;
import :Npc;

// SAVE / LOAD (F9/F10, server/single player only — clients refuse) and the profiling SCENARIO
// built on the load. See the Match.ixx comments on saveGame/loadGame and runScenario.

static constexpr const char* c_gameSavePath = "Local/gamesave.txt"; // cwd = Assets/

void GameMatch::saveGame()
{
    if (m_isClient)
    {
        Log::warning("Save game: server only");
        return;
    }
    AssetNode root; // unnamed — writeAssetText writes only the children
    if (m_coop && m_coopMap.built)
    {
        // The map's generation inputs: structure/unit positions and node indices only make sense
        // on the exact map the save was played on — loadGame regenerates it from these.
        root.set("MapSeed", oc::to_string((int)m_coopMap.seed));
        root.set("MapFill", m_coopMap.fill);
        root.set("MapLanes", oc::to_string(m_coopMap.lanes));
        // The wave clock: how many waves launched (sizes the next one) and the seconds to it, so a
        // load resumes the escalation instead of restarting at wave 1.
        root.set("WaveIndex", oc::to_string(m_waveIndex));
        root.set("WaveTimer", glm::max(m_waveTimer, 0.0f));
        root.set("MatchTime", m_matchTime); // the HUD clock (sim seconds since the world spawned)
        saveTrickle(root); // the wave/ambient spawns still queued (a save mid-wave loses nothing)
    }
    else if (m_coopMap.built && m_coopMap.pvp)
        root.set("PvpMap", oc::to_string((int)m_coopMap.pvpMap)); // the arena (EPvpMap index)
    // The LOCAL player's body position (its own capsule only — remote players are not saved).
    // Headless (no capsule) writes nothing, and a load without the key leaves the player put.
    if (m_player.entity())
        root.set("PlayerPos", m_player.bodyPos());
    m_structures.saveTo(root);
    m_npcs.saveUnits(root);
    // F9: an explicit user action, main thread.
    if (!FileSystem::writeFileStr(c_gameSavePath, writeAssetText(root), /*allowMainThread*/ true))
    {
        Log::warning(oc::string("Save game: cannot write ") + c_gameSavePath);
        return;
    }
    Log::info(oc::string("Game saved to ") + c_gameSavePath);
}

void GameMatch::loadGame(oc::string_view path)
{
    if (m_isClient)
    {
        Log::warning("Load game: server only");
        return;
    }
    AssetNode root;
    oc::string error;
    if (!loadAssetFile(path.empty() ? oc::string(c_gameSavePath) : oc::string(path), root, error))
    {
        Log::warning("Load game: " + error);
        return;
    }
    // A co-op save carries its map's generation inputs: regenerate the exact map first (clearing
    // the current structures BEFORE the node set swaps, so no stale extractor outlives its node).
    // Connected clients rebuild from the GMp broadcast the same way.
    if (m_coop)
    {
        if (const AssetNode* seedNode = root.find("MapSeed"); seedNode && seedNode->asInt() != 0)
        {
            m_structures.clearAllStructures(); // fires GRm hooks — clients prune ahead of the swap
            rebuildCoopMap((uint32)seedNode->asInt(),
                root.find("MapFill") ? root.find("MapFill")->asFloat() : m_coopMap.fill,
                root.find("MapLanes") ? root.find("MapLanes")->asInt() : m_coopMap.lanes);
            if (m_isServer)
                sendMapSeed();
        }
    }
    else if (const AssetNode* mapNode = root.find("PvpMap")) // a PvP save names its arena
    {
        const EPvpMap map = (EPvpMap)glm::clamp(mapNode->asInt(), 0, (int)EPvpMap::Count - 1);
        if (!m_coopMap.built || !m_coopMap.pvp || m_coopMap.pvpMap != map)
        {
            m_structures.clearAllStructures();
            rebuildPvpMap(map);
            if (m_isServer)
                sendMapSeed();
        }
    }
    // Replace the sim. Structure removal hooks fire during the clear (GRm to clients), then the
    // loaded state re-broadcasts below; unit entities resync through the normal despawn/spawn
    // replication (Component Network in their prefabs).
    m_structures.loadFrom(root);
    m_npcs.loadUnits(root, m_structures);
    m_selectedId = 0;
    // The wave clock (co-op saves; older saves without it keep the current clock).
    if (const AssetNode* n = root.find("WaveIndex"))
        m_waveIndex = glm::max(n->asInt(), 0);
    if (const AssetNode* n = root.find("WaveTimer"))
        m_waveTimer = glm::max(n->asFloat(), 0.0f);
    if (const AssetNode* n = root.find("MatchTime"))
        m_matchTime = glm::max(n->asFloat(), 0.0f);
    // The local player back to where it stood. Both call sites (F10 in updateWindowed, the
    // --scenario timer) are main thread and pre-physics, so the direct body setters are sanctioned.
    // Older saves carry no key and leave the player put; remote players keep their own positions.
    if (const AssetNode* n = root.find("PlayerPos"))
        m_player.teleport(n->asVec3(m_player.bodyPos()));
    loadTrickle(root); // resume the queued wave/ambient spawns (after rebuildCoopMap — see there)
    if (m_isServer)
    {
        for (int i = 0; i < m_structures.structureCount(); ++i)
        {
            sendStructurePlaced(i);
            if (!m_structures.structureRoute(i).empty())
                sendRoute(i);
            if (isBarracksType(m_structures.structureType(i)))
                sendUnitType(i);
        }
        // (No cable replay: clients re-derive links from the mirrored segments.)
    }
}

// ---- the profiling scenario --------------------------------------------------------------------

bool GameMatch::runScenario(oc::string_view savePath)
{
    if (!m_enabled || m_isClient)
    {
        Log::warning("Scenario: needs --game on the authority (single player or server)");
        return false;
    }
    loadGame(savePath);
    Log::info(oc::format("Scenario: loaded '{}'", savePath.empty() ? oc::string_view(c_gameSavePath) : savePath));
    // PvP: the loaded units' spatial entries link at the next commitFrame — the select-all query
    // runs from the next update() (issueScenarioOrder), marching everything on the other Base.
    // CO-OP: no order — there is no enemy Base to march on, and the session is measured as saved
    // (the player holds position, the AI waves and ambient groups carry on).
    m_scenarioOrderPending = !m_coop;
    m_scenarioOrderTries = 0;
    return true;
}

void GameMatch::issueScenarioOrder()
{
    // Select ALL live own-team units (not just the visible ones — the box select's query is a
    // frustum), then the same order the RMB press gives: locked move target + one seeded lane.
    // The World's root list serves loaded units immediately (no spatial-link latency), but the
    // enemy Base view and the published nav raster still arrive frames later — the retry loop
    // stays (bounded, in case the save held none).
    m_selectedUnits.clear();
    oc::vector<Entity*> units;
    NpcSystem::queryAllUnits(units); // puppets already excluded
    for (Entity* e : units)
        if (const GameUnitComponent* u = getComponent<GameUnitComponent>(e); u->alive() && u->team == (uint32)m_team)
            m_selectedUnits.push_back(EntityPtr(e));
    // The same order a right-click on the other team's Base gives: clicked at its centre, so the
    // destination lands on the face toward the player (pushed out of the footprint — the centre
    // itself is blocked cells and the A* would fail). The structure view (m_frame) is a per-frame
    // spatial query refreshed AFTER this point in update, so the loaded Base shows up a frame
    // after the units do: wait for it as well.
    int enemyBase = -1;
    for (int i = 0; i < m_structures.structureCount() && enemyBase < 0; ++i)
        if (m_structures.structureType(i) == EStructureType::Base && m_structures.structureTeam(i) != (uint8)m_team)
            enemyBase = i;
    // Also wait for the Nav obstacle raster: the order's lane is an A* over it, and a unit's own
    // plan requests need it too — an order before it is published walks straight into the walls.
    const bool ready = !m_selectedUnits.empty() && enemyBase >= 0 && Globals::navSystem.raster() != nullptr;
    if (!ready && ++m_scenarioOrderTries < 600)
        return; // try again next update (bounded: ~10 s, in case the save holds no units / Nav is off)
    if (!ready)
        Log::warning(oc::format("Scenario: giving up after {} frames ({} units, enemy Base {}, raster {})", m_scenarioOrderTries,
            (uint32)m_selectedUnits.size(), enemyBase >= 0 ? "found" : "missing", Globals::navSystem.raster() != nullptr ? "published" : "missing"));
    m_scenarioOrderPending = false;
    const glm::vec3 target = enemyBase >= 0 ? pointOutsideFootprint(m_structures.structurePos(enemyBase), enemyBase)
                                            : (m_team == 0 ? m_enemyBasePos : m_basePos);
    const bool laneSeeded = moveOrderAt(target);
    Log::info(oc::format("Scenario: {} units ordered to ({:.0f}, {:.0f}) after {} frames, lane {}", (uint32)m_selectedUnits.size(),
        m_player.moveTarget().x, m_player.moveTarget().z, m_scenarioOrderTries, laneSeeded ? "plan queued" : "NOT queued (no raster yet)"));
}
