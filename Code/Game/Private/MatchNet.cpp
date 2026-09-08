module Game;

import Core;
import Core.glm;
import Core.Log;
import Core.Time;
import Core.Transform;
import Entity;
import Force;
import Network;
import :Match;
import :Player;
import :Structures;
import :Npc;

// MULTIPLAYER: the team slots, the per-client capsules and the join replay, every game event —
// the server's broadcasts (GPl/GRm/GSt/GCb/GCf/GRt/GBu/GLt/GWv/GMp/GPz/GDm) and the clients'
// Gq* requests — and the request seams local input shares with them. See the Match.ixx
// MULTIPLAYER comment and Code/Game/CONTEXT.md "Multiplayer sync".

void GameMatch::setLobbyTeams(uint8 numTeams, oc::span<const oc::pair<uint32, uint8>> picks)
{
    m_numTeams = (uint8)glm::clamp((int)numTeams, 2, GameMaxTeams);
    m_lobbyTeams.assign(picks.begin(), picks.end());
}

uint8 GameMatch::allocateClientTeam(uint32 clientId) const
{
    if (m_coop)
        return 0; // co-op: everyone plays on the server's team
    // The lobby pick wins. Otherwise (command-line start, a late joiner) the least-populated
    // playable team, lowest index on a tie. The server holds m_team; each connected client's team
    // lives on its capsule's puppet component, so the live set needs no separate bookkeeping.
    for (const auto& [id, team] : m_lobbyTeams)
        if (id == clientId && team < m_numTeams)
            return team;
    int counts[GameMaxTeams] = {};
    if (clientId != 0 && m_team < m_numTeams)
        ++counts[m_team];
    for (const auto& [id, p] : m_clientPlayers)
        if (p && id != clientId)
            if (const GameUnitComponent* u = getComponent<GameUnitComponent>(p.get()); u && u->team < m_numTeams)
                ++counts[u->team];
    uint8 best = 0;
    for (uint8 t = 1; t < m_numTeams; ++t)
        if (counts[t] < counts[best])
            best = t;
    return best;
}

int GameMatch::clientTeam(uint32 clientId) const
{
    if (clientId == 0)
        return (int)m_team; // the server itself
    const auto it = m_clientPlayers.find(clientId);
    if (it == m_clientPlayers.end() || !it->second)
        return -1; // unknown: no capsule spawned for that id (never grant it a team's authority)
    const GameUnitComponent* u = getComponent<GameUnitComponent>(it->second.get());
    return u ? (int)u->team : -1;
}

void GameMatch::onClientJoined(uint32 clientId)
{
    // Their player: spawned server-side (player.pre carries Component Network), handed over with
    // setOwner in the SAME frame; the client adopts + drives it through the claim stream.
    // Clients spawn beside THEIR team's Base — the team is a freshly allocated slot, and the
    // capsule's puppet component carries it to the owner through the snapshot game blob.
    const uint8 team = allocateClientTeam(clientId);
    const glm::vec3 clientStart = teamStartPos(team);
    EntityPtr player = Globals::world.spawnAssetFile("Entities/Game/player.pre",
        Transform(clientStart + glm::vec3(2.0f * (float)(clientId % 5), 0.0f, 1.5f * (float)(clientId % 3))), true);
    if (player)
    {
        player->setName(("Player " + oc::to_string(clientId)).c_str());
        Globals::networkManager.setOwner(*player, clientId);
        if (GameUnitComponent* unit = getComponent<GameUnitComponent>(player.get()))
            unit->team = team; // the authoritative assignment: everything else reads it from here
        // the server-side twin's field carries the client's team (readbacks, visuals)
        if (ForceComponent* fc = getComponent<ForceComponent>(player.get()))
            fc->emitter.setTeam(team);
        Globals::world.addRootEntity(player);
        m_clientPlayers[clientId] = oc::move(player);
        Log::info("Game: client " + oc::to_string(clientId) + " assigned team " + oc::to_string(team));
    }
    // World-state replay for the late joiner: the co-op map seed FIRST (the joiner builds its
    // terrain + nodes from it before any structure mirror below arrives — reliable ch1 is
    // ordered), then every structure, broadcast (mirrorPlace is idempotent, so already-connected
    // clients shrug the duplicates off; rebuildCoopMap no-ops on the repeated seed). Links need
    // no replay — each client derives them locally from the mirrored cable segments.
    if (m_isServer && m_coopMap.built)
        sendMapSeed(); // co-op inputs or the PvP arena — the joiner's terrain + nodes
    for (int i = 0; i < m_structures.structureCount(); ++i)
        sendStructurePlaced(i);
    for (int i = 0; i < m_structures.structureCount(); ++i)
    {
        if (!m_structures.structureRoute(i).empty())
            sendRoute(i); // barracks waypoint routes replay too
        if (isBarracksType(m_structures.structureType(i)))
            sendUnitType(i); // and the produced unit type
    }
    if (m_paused) // the joiner lands in a paused game: show it the box
    {
        uint8 buffer[4];
        NetWriter writer(buffer);
        writer.write<uint8>(1);
        Globals::networkManager.fireNetworkEvent("GPz", writer.data());
    }
    Log::info("Game: client " + oc::to_string(clientId) + " joined, world replayed");
}

void GameMatch::onClientLeft(uint32 clientId)
{
    if (const auto it = m_clientPlayers.find(clientId); it != m_clientPlayers.end())
    {
        if (it->second)
            Globals::world.removeRootEntity(it->second.get());
        m_clientPlayers.erase(it);
    }
}

// ---- the server's broadcasts -------------------------------------------------------------------

void GameMatch::sendStructurePlaced(int index)
{
    uint8 buffer[32];
    NetWriter writer(buffer);
    const glm::vec3 pos = m_structures.structurePos(index);
    const glm::vec2 facing = m_structures.structureFacing(index);
    writer.write<uint32>(m_structures.structureId(index));
    writer.write<uint8>((uint8)m_structures.structureType(index));
    writer.write<float>(pos.x);
    writer.write<float>(pos.y);
    writer.write<float>(pos.z);
    writer.write<float>(facing.x);
    writer.write<float>(facing.y);
    writer.write<int16>((int16)m_structures.structureNodeIndex(index));
    writer.write<uint8>(m_structures.structureTeam(index));
    writer.write<uint8>(m_structures.structureBlueprint(index) ? 0u : 1u); // built flag (Base replay)
    Globals::networkManager.fireNetworkEvent("GPl", writer.data());
}

void GameMatch::sendRoute(int index)
{
    uint8 buffer[80];
    NetWriter writer(buffer);
    const oc::span<const glm::vec3> route = m_structures.structureRoute(index);
    writer.write<uint32>(m_structures.structureId(index));
    writer.write<uint8>((uint8)route.size());
    for (const glm::vec3& p : route)
    {
        writer.write<float>(p.x);
        writer.write<float>(p.z);
    }
    Globals::networkManager.fireNetworkEvent("GRt", writer.data());
}

void GameMatch::sendUnitType(int index)
{
    uint8 buffer[8];
    NetWriter writer(buffer);
    writer.write<uint32>(m_structures.structureId(index));
    writer.write<uint8>(m_structures.structureUnitType(index));
    Globals::networkManager.fireNetworkEvent("GBu", writer.data());
}

void GameMatch::sendStats()
{
    ProfileScope scope("Game send stats", EProfileCategory::Game);
    // Volatile mirror state at ~5 Hz: resource totals + per-structure fractions (u8-quantized).
    uint8 buffer[1000];
    NetWriter writer(buffer);
    for (int t = 0; t < GameMaxTeams; ++t)
        writer.write<float>(m_structures.minerals((uint8)t));
    for (int t = 0; t < GameMaxTeams; ++t)
        writer.write<float>(m_structures.fuel((uint8)t));
    writer.write<float>(m_structures.gridEnergy());
    writer.write<float>(m_structures.gridEnergyCapacity());
    writer.write<float>(m_structures.energyGenPerSec());
    writer.write<float>(m_structures.energyUsePerSec());
    // CABLE SEGMENTS are excluded: they carry no stores, hundreds of them would blow the 79-record
    // cap, and their built flip mirrors through the GPl re-send instead (onStructureBuilt).
    constexpr int c_maxRecords = 79; // 11B each + the 146B header stays under the 1024B event cap
    int indices[c_maxRecords];
    int count = 0;
    for (int i = 0; i < m_structures.structureCount() && count < c_maxRecords; ++i)
        if (!isCableOrCrossing(m_structures.structureType(i)))
            indices[count++] = i;
    writer.write<uint16>((uint16)count);
    const auto frac8 = [](float v, float max) {
        return (uint8)glm::clamp(max > 0.0f ? v / max * 255.0f : 0.0f, 0.0f, 255.0f); };
    for (int k = 0; k < count; ++k)
    {
        const int i = indices[k];
        writer.write<uint32>(m_structures.structureId(i));
        writer.write<uint8>(frac8(m_structures.structureHealth(i), m_structures.structureHealthMax()));
        writer.write<uint8>(frac8(m_structures.structureCharge(i), m_structures.structureCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureFuel(i), m_structures.structureFuelCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureMinerals(i), m_structures.structureMineralCapacity(i)));
        // The output byte doubles as the barracks' POPULATION tally (no emitter there).
        writer.write<uint8>(isBarracksType(m_structures.structureType(i))
            ? (uint8)glm::clamp(m_structures.structurePopulation(i), 0, 255)
            : frac8(m_structures.structureOutputFrac(i), 1.0f));
        writer.write<uint8>(frac8(m_structures.structureFlowUtil(i), 1.0f));
        writer.write<uint8>((uint8)((m_structures.structurePowered(i) ? 1u : 0u)
            | (m_structures.structureBlueprint(i) ? 2u : 0u))); // status bits (health IS progress)
    }
    Globals::networkManager.fireNetworkEvent("GSt", writer.data());

    // GCb: build progress of cable/crossing BLUEPRINTS only (the segments under construction — a
    // small, changing set; built ones are silent). 5 B per record; a long unbuilt run past the cap
    // rotates through m_cableSyncCursor over consecutive sends.
    constexpr int c_maxCableRecords = 190; // 2 + 190 * 5 = 952 B, under the 1024 B event cap
    uint8 cableBuffer[1000];
    NetWriter cableWriter(cableBuffer);
    cableWriter.write<uint16>(0);
    uint16 cableCount = 0;
    const int total = m_structures.structureCount();
    int i = total > 0 ? m_cableSyncCursor % total : 0;
    for (int visited = 0; visited < total && cableCount < c_maxCableRecords; ++visited, i = (i + 1) % total)
    {
        if (!isCableOrCrossing(m_structures.structureType(i)) || !m_structures.structureBlueprint(i))
            continue;
        cableWriter.write<uint32>(m_structures.structureId(i));
        cableWriter.write<uint8>(frac8(m_structures.structureHealth(i), m_structures.structureHealthMaxOf(i)));
        ++cableCount;
    }
    m_cableSyncCursor = i;
    if (cableCount == 0)
        return;
    cableWriter.writeAt(0, cableCount);
    Globals::networkManager.fireNetworkEvent("GCb", cableWriter.data());

    // GCf: the cable FILLS + throughput (transport cells per segment, the ~2 s average as a
    // fraction of the rate), rotating through the cable nodes from m_cableFillCursor — the
    // clients' cable visuals and the selected-cable label. 6 B each.
    {
        constexpr int c_maxFillRecords = 160; // 2 + 160 * 6 = 962 B, under the 1024 B event cap
        oc::vector<StructureSystem::CableMirror> fills;
        m_structures.collectCableFills(fills, m_cableFillCursor, c_maxFillRecords);
        if (!fills.empty())
        {
            uint8 fillBuffer[1000];
            NetWriter fillWriter(fillBuffer);
            fillWriter.write<uint16>((uint16)fills.size());
            for (const StructureSystem::CableMirror& f : fills)
            {
                fillWriter.write<uint32>(f.id);
                fillWriter.write<uint8>(f.fill);
                fillWriter.write<uint8>(f.util);
            }
            Globals::networkManager.fireNetworkEvent("GCf", fillWriter.data());
        }
    }
}

// ---- the request seams (local queue on the authority, Gq* events from a client) ---------------
// The SAME validation runs server-side either way.

void GameMatch::requestPlace(EStructureType type, const glm::vec3& pos, int nodeIndex, const glm::vec3& facing)
{
    if (!m_isClient)
    {
        m_structures.queuePlaceRequest(type, pos, nodeIndex, facing, (uint8)m_team);
        return;
    }
    uint8 buffer[32];
    NetWriter writer(buffer);
    writer.write<uint8>((uint8)type);
    writer.write<float>(pos.x);
    writer.write<float>(pos.z);
    writer.write<int16>((int16)nodeIndex);
    writer.write<float>(facing.x);
    writer.write<float>(facing.z);
    Globals::networkManager.fireNetworkEvent("GqP", writer.data());
}

void GameMatch::requestDemolish(uint32 id)
{
    if (!m_isClient)
    {
        m_structures.queueDemolishRequest(id, (uint8)m_team);
        return;
    }
    uint8 buffer[8];
    NetWriter writer(buffer);
    writer.write<uint32>(id);
    Globals::networkManager.fireNetworkEvent("GqD", writer.data());
}

void GameMatch::requestSetUnitType(uint32 id, uint8 unitType)
{
    if (!m_isClient)
    {
        m_structures.queueUnitTypeRequest(id, unitType, (uint8)m_team);
        return;
    }
    uint8 buffer[8];
    NetWriter writer(buffer);
    writer.write<uint32>(id);
    writer.write<uint8>(unitType);
    Globals::networkManager.fireNetworkEvent("GqU", writer.data());
}

void GameMatch::requestSetRoute(uint32 id, oc::span<const glm::vec3> points)
{
    const size_t count = glm::min(points.size(), (size_t)StructureSystem::MaxRouteWaypoints);
    if (!m_isClient)
    {
        m_structures.queueRouteRequest(id, points.subspan(0, count), (uint8)m_team);
        return;
    }
    uint8 buffer[64]; // 6 waypoints = 53B, inside the Gq* request cap
    NetWriter writer(buffer);
    writer.write<uint32>(id);
    writer.write<uint8>((uint8)count);
    for (size_t k = 0; k < count; ++k)
    {
        writer.write<float>(points[k].x);
        writer.write<float>(points[k].z);
    }
    Globals::networkManager.fireNetworkEvent("GqW", writer.data());
}

// ---- the shared pause --------------------------------------------------------------------------

void GameMatch::applyPause(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    Globals::time.setPaused(paused);
    Log::info(paused ? "Game paused" : "Game resumed");
}

void GameMatch::requestPause(bool paused)
{
    if (m_isClient)
    {
        uint8 buffer[4];
        NetWriter writer(buffer);
        writer.write<uint8>(paused ? 1 : 0);
        Globals::networkManager.fireNetworkEvent("GqZ", writer.data());
        return;
    }
    applyPause(paused);
    if (m_isServer)
    {
        uint8 buffer[4];
        NetWriter writer(buffer);
        writer.write<uint8>(paused ? 1 : 0);
        Globals::networkManager.fireNetworkEvent("GPz", writer.data());
    }
}

// ---- the event dispatch ------------------------------------------------------------------------

void GameMatch::handleNetEvent(oc::string_view name)
{
    ProfileScope scope("Game net event", EProfileCategory::Game);
    // Both roles share the hook; each side reacts only to the names meant for it (our own
    // broadcasts self-dispatch locally and fall through harmlessly).
    if (name.size() < 3 || name[0] != 'G')
        return;
    NetReader reader(Globals::networkManager.currentEventData());
    if (m_isClient)
    {
        if (name == "GPl")
        {
            const uint32 id = reader.read<uint32>();
            const uint8 type = reader.read<uint8>();
            const float x = reader.read<float>(), y = reader.read<float>(), z = reader.read<float>();
            const float fx = reader.read<float>(), fz = reader.read<float>();
            const int16 nodeIndex = reader.read<int16>();
            const uint8 team = reader.read<uint8>();
            const uint8 built = reader.read<uint8>();
            if (!reader.overflowed())
                m_structures.mirrorPlace(id, (EStructureType)type, glm::vec3(x, y, z), glm::vec2(fx, fz),
                    nodeIndex, team, built != 0);
        }
        else if (name == "GRm")
        {
            const uint32 id = reader.read<uint32>();
            if (!reader.overflowed())
                m_structures.mirrorRemove(id);
        }
        else if (name == "GSt")
        {
            float minerals[GameMaxTeams], fuel[GameMaxTeams];
            for (float& m : minerals)
                m = reader.read<float>();
            for (float& f : fuel)
                f = reader.read<float>();
            const float energy = reader.read<float>(), cap = reader.read<float>();
            const float gen = reader.read<float>(), use = reader.read<float>();
            const uint16 count = reader.read<uint16>();
            for (uint16 i = 0; i < count && !reader.overflowed(); ++i)
            {
                const uint32 id = reader.read<uint32>();
                const uint8 health = reader.read<uint8>(), charge = reader.read<uint8>();
                const uint8 fuelFrac = reader.read<uint8>(), mineralFrac = reader.read<uint8>();
                const uint8 output = reader.read<uint8>(), util = reader.read<uint8>();
                const uint8 status = reader.read<uint8>();
                if (!reader.overflowed())
                    m_structures.mirrorStructureState(id, health / 255.0f, charge / 255.0f,
                        fuelFrac / 255.0f, mineralFrac / 255.0f, output / 255.0f, util / 255.0f,
                        (status & 1u) != 0, (status & 2u) != 0);
            }
            if (!reader.overflowed())
                m_structures.mirrorTotals(minerals, fuel, energy, cap, gen, use);
            // (mirrorTotals takes the spans by value into its own arrays — safe past this scope)
        }
        else if (name == "GCb")
        {
            const uint16 count = reader.read<uint16>();
            for (uint16 i = 0; i < count && !reader.overflowed(); ++i)
            {
                const uint32 id = reader.read<uint32>();
                const uint8 health = reader.read<uint8>();
                if (!reader.overflowed())
                    m_structures.mirrorCableProgress(id, health / 255.0f);
            }
        }
        else if (name == "GCf")
        {
            const uint16 count = reader.read<uint16>();
            for (uint16 i = 0; i < count && !reader.overflowed(); ++i)
            {
                const uint32 id = reader.read<uint32>();
                const uint8 fill = reader.read<uint8>();
                const uint8 util = reader.read<uint8>();
                if (!reader.overflowed())
                    m_structures.mirrorCableFill(id, fill, util);
            }
        }
        else if (name == "GDm")
        {
            // Unit melee damage the server's sim dealt to OUR player (we own our health).
            const uint8 count = reader.read<uint8>();
            for (uint8 k = 0; k < count && !reader.overflowed(); ++k)
            {
                const uint32 clientId = reader.read<uint32>();
                const float damage = reader.read<float>();
                if (!reader.overflowed() && clientId == Globals::networkManager.localClientId()
                    && damage > 0.0f && damage < 1000.0f)
                    m_player.applyDamage(damage);
            }
        }
        else if (name == "GRt")
        {
            const uint32 id = reader.read<uint32>();
            const uint8 count = reader.read<uint8>();
            glm::vec3 points[StructureSystem::MaxRouteWaypoints];
            const uint8 used = glm::min<uint8>(count, StructureSystem::MaxRouteWaypoints);
            for (uint8 k = 0; k < used; ++k)
            {
                const float x = reader.read<float>(), z = reader.read<float>();
                points[k] = glm::vec3(x, 0.0f, z);
            }
            if (!reader.overflowed())
                m_structures.mirrorRoute(id, oc::span<const glm::vec3>(points, used));
        }
        else if (name == "GBu")
        {
            const uint32 id = reader.read<uint32>();
            const uint8 unitType = reader.read<uint8>();
            if (!reader.overflowed())
                m_structures.mirrorUnitType(id, unitType);
        }
        else if (name == "GLt")
        {
            const float fx = reader.read<float>(), fy = reader.read<float>(), fz = reader.read<float>();
            const float tx = reader.read<float>(), ty = reader.read<float>(), tz = reader.read<float>();
            if (!reader.overflowed())
                m_npcs.addBeam(glm::vec3(fx, fy, fz), glm::vec3(tx, ty, tz));
        }
        else if (name == "GPz")
        {
            const uint8 paused = reader.read<uint8>();
            if (!reader.overflowed())
                applyPause(paused != 0);
        }
        else if (name == "GWv")
        {
            // Co-op wave announcement (the wave itself arrives as replicated unit entities).
            const uint16 wave = reader.read<uint16>();
            if (!reader.overflowed())
                Log::info(oc::format("Co-op: wave {} incoming", (int)wave));
        }
        else if (name == "GMp")
        {
            // The server's terrain: the co-op map's inputs or the PvP arena — build the identical
            // terrain (+ barrier) + nodes locally (the rebuilds are no-ops on the re-broadcasts
            // later joiners trigger). Sent FIRST in the join replay, so the map exists before any
            // structure mirror lands.
            const uint8 mode = reader.read<uint8>();
            if (mode == 1)
            {
                const uint8 map = reader.read<uint8>();
                if (!reader.overflowed() && !m_coop && map < (uint8)EPvpMap::Count)
                    rebuildPvpMap((EPvpMap)map);
            }
            else
            {
                const uint32 seed = reader.read<uint32>();
                const float fill = reader.read<float>();
                const uint8 lanes = reader.read<uint8>();
                if (!reader.overflowed() && m_coop && seed != 0 && fill >= 0.0f && fill <= 1.0f)
                    rebuildCoopMap(seed, fill, (int)lanes);
            }
        }
        // (shield/materials state rides the entity snapshot's game blob now — no GSh event)
        return;
    }
    if (!m_isServer || name[1] != 'q')
        return;
    // Client requests, validated here + through the same authority seams local input uses. A
    // sender with no capsule has no assigned team, so it gets no authority to act as one.
    const uint32 sender = Globals::networkManager.currentEventSender();
    if (clientTeam(sender) < 0)
        return;
    if (name == "GqZ") // any seated player may pause/resume for everyone
    {
        const uint8 paused = reader.read<uint8>();
        if (!reader.overflowed())
            requestPause(paused != 0);
        return;
    }
    if (name == "GqP")
    {
        const uint8 type = reader.read<uint8>();
        const float x = reader.read<float>(), z = reader.read<float>();
        const int16 nodeIndex = reader.read<int16>();
        const float fx = reader.read<float>(), fz = reader.read<float>();
        if (!reader.overflowed() && isPlaceableType((EStructureType)type))
            m_structures.queuePlaceRequest((EStructureType)type, glm::vec3(x, 0.0f, z), nodeIndex,
                glm::vec3(fx, 0.0f, fz), requestTeam(sender));
    }
    else if (name == "GqD")
    {
        const uint32 id = reader.read<uint32>();
        if (!reader.overflowed())
            m_structures.queueDemolishRequest(id, requestTeam(sender));
    }
    else if (name == "GqW")
    {
        const uint32 id = reader.read<uint32>();
        const uint8 count = reader.read<uint8>();
        glm::vec3 points[StructureSystem::MaxRouteWaypoints];
        const uint8 used = glm::min<uint8>(count, StructureSystem::MaxRouteWaypoints);
        for (uint8 k = 0; k < used; ++k)
        {
            const float x = reader.read<float>(), z = reader.read<float>();
            points[k] = glm::vec3(x, 0.0f, z);
        }
        if (!reader.overflowed()) // team/barracks ownership validated at apply
            m_structures.queueRouteRequest(id, oc::span<const glm::vec3>(points, used), requestTeam(sender));
    }
    else if (name == "GqU")
    {
        const uint32 id = reader.read<uint32>();
        const uint8 unitType = reader.read<uint8>();
        if (!reader.overflowed()) // team/barracks ownership + type range validated at apply
            m_structures.queueUnitTypeRequest(id, unitType, requestTeam(sender));
    }
    // (GqE — the owner's shield self-report — now rides the claim stream's game blob, applied by
    // NetworkManager to the twin's GameUnitComponent + emitter. GqS/GqM went with player combat.
    // Unknown Gq* names from stale builds simply fall through here.)
}
