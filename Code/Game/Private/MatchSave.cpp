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
import :Sound;

// SAVE / LOAD (F9/F10, server/single player only - clients refuse) and the profiling SCENARIO
// built on the load. See the Match.ixx comments on saveGame/loadGame and runScenario.

static constexpr const char* c_gameSavePath = "Local/gamesave.txt"; // cwd = Assets/

void GameMatch::saveGame()
{
    if (m_isClient)
    {
        Log::warning("Save game: server only");
        return;
    }
    AssetNode root; // unnamed - writeAssetText writes only the children
    if (m_coop)
    {
        // The wave clock: how many waves launched and the seconds to the next, so a load resumes the night. The light
        // rules: the sun is the wave clock, so the time of day rides along. (The terrain is the session's, so no map.)
        root.set("WaveIndex", oc::to_string(m_waveIndex));
        root.set("WaveTimer", glm::max(m_waveTimer, 0.0f));
        root.set("NightIndex", oc::to_string(m_nightIndex));
        root.set("Night", m_night);
        root.set("TimeOfDay", Globals::settings.sky.timeOfDay);
        // The night's build-up: the waves launched this night and the size rolled for the next one ("Next wave power").
        root.set("NightWave", oc::to_string(m_nightWave));
        root.set("WaveScale", m_waveScale);
        root.set("MatchTime", m_matchTime); // the HUD clock (sim seconds since the world spawned)
        saveTrickle(root); // the wave spawns still queued (a save mid-wave loses nothing)
    }
    else if (m_coopMap.built && m_coopMap.pvp)
        root.set("PvpMap", oc::to_string((int)m_coopMap.pvpMap)); // the arena (EPvpMap index)
    // The LOCAL player's body position (its own capsule only - remote players are not saved).
    // Headless (no capsule) writes nothing, and a load without the key leaves the player put.
    if (m_player.entity())
        root.set("PlayerPos", m_player.bodyPos());
    if (m_rallyActive) // the rally flag (the friendly barracks units gather there)
        root.set("RallyPos", m_rallyPos);
    if (m_ship.exists()) // the ship's pose (at rest on the load); a ship that is landing loads landed
    {
        root.set("ShipPos", m_ship.grounded() ? m_ship.landedPos() : m_ship.position());
        root.set("ShipYaw", m_ship.yaw());
        if (m_ship.grounded())
        {
            root.set("ShipLanded", true);
            root.set("ShipTurns", oc::to_string(m_ship.quarterTurns()));
        }
        if (m_hasShipHome) // where it last landed: the landing ghost snaps back to it
        {
            root.set("ShipHome", m_shipHome);
            root.set("ShipHomeTurns", oc::to_string(m_shipHomeTurns));
        }
    }
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
    // A PvP save names its arena: build it first (clearing the current structures BEFORE the node set swaps). Connected
    // clients rebuild from the GMp broadcast the same way.
    if (const AssetNode* mapNode = root.find("PvpMap"); mapNode && !m_coop)
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
    // THE SHIP FIRST: the structures on its deck load in ship space, onto the ship's loaded pose (landed: they join the
    // world at once).
    if (const AssetNode* n = root.find("ShipPos"); n && m_ship.exists())
    {
        m_piloting = false;
        // Lift the present deck out of the world first (its cells, its stairs), so a landed load joins it at the new
        // place (tickShipStructures acts on the edge only).
        m_structures.setShipPose(true, m_ship.position(), m_ship.rotation(), false);
        m_structures.tickShipStructures();
        const glm::vec3 shipPos = n->asVec3(m_ship.position());
        if (root.find("ShipLanded") && root.find("ShipLanded")->asBool())
            m_ship.landAt(shipPos, root.find("ShipTurns") ? root.find("ShipTurns")->asInt() : 0);
        else
            m_ship.teleport(shipPos, root.find("ShipYaw") ? root.find("ShipYaw")->asFloat() : m_ship.yaw());
        m_structures.setShipPose(true, m_ship.position(), m_ship.rotation(), m_ship.landed());
        m_structures.tickShipStructures();
        m_landPlacing = false;
        m_hasShipHome = root.find("ShipHome") != nullptr;
        if (m_hasShipHome)
        {
            m_shipHome = root.find("ShipHome")->asVec3();
            m_shipHomeTurns = root.find("ShipHomeTurns") ? (root.find("ShipHomeTurns")->asInt() & 3) : 0;
        }
    }
    // Replace the sim. Structure removal hooks fire during the clear (GRm to clients), then the
    // loaded state re-broadcasts below; unit entities resync through the normal despawn/spawn
    // replication (Component Network in their prefabs).
    m_structures.loadFrom(root);
    m_structures.ensureShipRails(); // a save without them (or with fewer): the ship's wall is whole
    m_npcs.loadUnits(root, m_structures);
    GameSound::discardPosted(); // the clear's collapses and the load's placements are no events of the game: silent
    m_selectedId = 0;
    // The wave clock (co-op saves; older saves without it keep the current clock).
    if (const AssetNode* n = root.find("WaveIndex"))
        m_waveIndex = glm::max(n->asInt(), 0);
    if (const AssetNode* n = root.find("WaveTimer"))
        m_waveTimer = glm::max(n->asFloat(), 0.0f);
    if (m_coop)
    {
        if (const AssetNode* n = root.find("NightIndex"))
            m_nightIndex = glm::max(n->asInt(), 0);
        if (const AssetNode* n = root.find("Night"))
            m_night = n->asBool();
        if (const AssetNode* n = root.find("TimeOfDay"))
            Globals::settings.sky.timeOfDay = glm::clamp(n->asFloat(), 0.0f, 23.99f);
        if (const AssetNode* n = root.find("NightWave"))
            m_nightWave = glm::max(n->asInt(), 0);
        if (const AssetNode* n = root.find("WaveScale"))
            m_waveScale = glm::max(n->asFloat(), 0.0f);
        // The save holds no faction land: the layer is made again around the loaded Base, and its sources' end state is
        // put down before the next wave (tickFaction's settle; tickWaves waits for it).
        m_faction.clear();
        m_factionSettleIn = 0.5f;
    }
    if (const AssetNode* n = root.find("MatchTime"))
        m_matchTime = glm::max(n->asFloat(), 0.0f);
    // The local player back to where it stood. Both call sites (F10 in updateWindowed, the
    // --scenario timer) are main thread and pre-physics, so the direct body setters are sanctioned.
    // Older saves carry no key and leave the player put; remote players keep their own positions.
    if (const AssetNode* n = root.find("RallyPos")) // the rally flag as saved; none = none
        setRally(true, n->asVec3());
    else
        setRally(false, glm::vec3(0.0f));
    if (const AssetNode* n = root.find("PlayerPos"))
        m_player.teleport(n->asVec3(m_player.bodyPos()));
    loadTrickle(root); // resume the queued wave spawns
    if (m_isServer)
    {
        for (int i = 0; i < m_structures.structureCount(); ++i)
        {
            if (m_structures.structureOnShip(i))
                continue; // the ship's structures are the authority's (a client sees no ship)
            sendStructurePlaced(i);
            if (!m_structures.structureRoute(i).empty())
                sendRoute(i);
            if (hasOrderByte(m_structures.structureType(i)))
                sendUnitType(i);
        }
        // (No cable replay: clients re-derive links from the mirrored segments.)
    }
}

// ---- the profiling scenario ----

bool GameMatch::runScenario(oc::string_view savePath)
{
    if (!m_enabled || m_isClient)
    {
        Log::warning("Scenario: needs --game on the authority (single player or server)");
        return false;
    }
    // THE SANDBOX WORLD has no Base and no player until the terrain is in under the anchor (tickBaseSite). A load before
    // that would leave the player where the session puts it later (no capsule to move yet), and the session would add
    // its own Base to the save's. So the load WAITS for the site: tickBaseSite runs it right after it spawns the player.
    if (m_sandboxWorld && !m_siteFound)
    {
        m_scenarioWaitsForSite = true;
        m_scenarioSave = oc::string(savePath);
        Log::info("Scenario: waits for the Base site");
        return true;
    }
    {
        const FileSystem::AllowMainThreadIO scenarioIo; // the deferred load runs from the game tick, not from main's timer
        loadGame(savePath);
    }
    Log::info(oc::format("Scenario: loaded '{}'", savePath.empty() ? oc::string_view(c_gameSavePath) : savePath));
    // PvP: the loaded units' spatial entries link at the next commitFrame - the select-all query
    // runs from the next update() (issueScenarioOrder), marching everything on the other Base.
    // CO-OP: no order - there is no enemy Base to march on, and the session is measured as saved
    // (the player holds position, the AI waves carry on).
    m_scenarioOrderPending = !m_coop;
    m_scenarioOrderTries = 0;
    return true;
}

void GameMatch::issueScenarioOrder()
{
    // Select ALL live own-team units (not just the visible ones - the box select's query is a
    // frustum), then the same order the RMB press gives: locked move target + one seeded lane.
    // The World's root list serves loaded units immediately (no spatial-link latency), but the
    // enemy Base view and the published nav raster still arrive frames later - the retry loop
    // stays (bounded, in case the save held none).
    m_selectedUnits.clear();
    oc::vector<Entity*> units;
    NpcSystem::queryAllUnits(units); // puppets already excluded
    for (Entity* e : units)
        if (const GameUnitComponent* u = getComponent<GameUnitComponent>(e); u->alive() && u->team == (uint32)m_team)
            m_selectedUnits.push_back(EntityPtr(e));
    // The same order a right-click on the other team's Base gives: clicked at its centre, so the
    // destination lands on the face toward the player (pushed out of the footprint - the centre
    // itself is blocked cells and the A* would fail). The structure view (m_frame) is a per-frame
    // spatial query refreshed AFTER this point in update, so the loaded Base shows up a frame
    // after the units do: wait for it as well.
    int enemyBase = -1;
    for (int i = 0; i < m_structures.structureCount() && enemyBase < 0; ++i)
        if (m_structures.structureType(i) == EStructureType::Base && m_structures.structureTeam(i) != (uint8)m_team)
            enemyBase = i;
    // Also wait for the Nav obstacle raster: the order's lane is an A* over it, and a unit's own
    // plan requests need it too - an order before it is published walks straight into the walls.
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
