module Game;

import Core;
import Core.glm;
import Core.SDL;
import Core.Log;
import Core.Tweaks;
import Core.Camera;
import Core.Rect;
import Core.Transform;
import Core.GameHud;
import Input;
import UI;
import Entity;
import Physics;
import Force;
import RendererVK;
import Network;
import :Match;
import :GameCamera;
import :Player;
import :Structures;
import :Npc;

// The grid hotbar's category tables. Key 1..3 picks a category at the top level; inside one,
// keys 1..N arm an item and key 0 backs out.
static constexpr const char* c_buildCategories[3] = { "Emitters", "Production", "Distribution" };
static constexpr BuildItem c_emitterItems[] = {
    { "Emitter", false, EStructureType::Emitter, ECableType::Basic },
    { "Bastion", false, EStructureType::Bastion, ECableType::Basic },
    { "Lance", false, EStructureType::Lance, ECableType::Basic },
    { "Wall", false, EStructureType::Wall, ECableType::Basic },
    { "Turret", false, EStructureType::Turret, ECableType::Basic },
};
static constexpr float c_wallSegmentSpacing = 2.0f; // one segment per box width along the line
static constexpr int c_wallMaxSegments = 16;
static constexpr BuildItem c_productionItems[] = {
    { "Generator", false, EStructureType::Generator, ECableType::Basic },
    { "Solar", false, EStructureType::Solar, ECableType::Basic },
    { "Extractor", false, EStructureType::Extractor, ECableType::Basic },
    { "Fabricator", false, EStructureType::Fabricator, ECableType::Basic },
    { "Barracks", false, EStructureType::Barracks, ECableType::Basic },
    { "B-Brute", false, EStructureType::BarracksBrute, ECableType::Basic },
    { "B-Runner", false, EStructureType::BarracksRunner, ECableType::Basic },
    { "B-Spitter", false, EStructureType::BarracksSpitter, ECableType::Basic },
    { "Constructor", false, EStructureType::Constructor, ECableType::Basic },
};
static constexpr BuildItem c_distributionItems[] = {
    { "Connector", false, EStructureType::Connector, ECableType::Basic },
    { "Cable", true, EStructureType::Emitter, ECableType::Basic },
    { "H-Cable", true, EStructureType::Emitter, ECableType::Heavy },
    { "Pipe", true, EStructureType::Emitter, ECableType::Pipe },
    { "Conveyor", true, EStructureType::Emitter, ECableType::Conveyor },
    { "Battery", false, EStructureType::Battery, ECableType::Basic },
    { "Fuel tank", false, EStructureType::FuelTank, ECableType::Basic },
    { "M-Silo", false, EStructureType::MineralSilo, ECableType::Basic },
};
static std::span<const BuildItem> buildCategoryItems(int category)
{
    switch (category)
    {
    case 0: return c_emitterItems;
    case 1: return c_productionItems;
    case 2: return c_distributionItems;
    default: return {};
    }
}

static uint32 packColor(const glm::vec3& c)
{
    const glm::vec3 s = glm::clamp(c, 0.0f, 1.0f) * 255.0f;
    return (uint32)s.x | ((uint32)s.y << 8) | ((uint32)s.z << 16) | 0xFF000000u;
}

static void drawCircle(const glm::vec3& center, float radius, uint32 color, int segments = 32)
{
    glm::vec3 prev = center + glm::vec3(radius, 0.0f, 0.0f);
    for (int i = 1; i <= segments; ++i)
    {
        const float a = float(i) / segments * glm::two_pi<float>();
        const glm::vec3 p = center + glm::vec3(std::cos(a) * radius, 0.0f, std::sin(a) * radius);
        Globals::rendererVK.addDebugLine(prev, p, color);
        prev = p;
    }
}

// PvP arena: a walled corridor along X — combat funnels through the middle.
static constexpr float c_pvpCorridorHalfLength = 65.0f; // x extent of the play area
static constexpr float c_pvpCorridorHalfWidth = 20.0f;   // z extent
static constexpr float c_pvpWallStep = 10.0f;            // border block size (borderwall.pre)

GameMatch::GameMatch(bool enabled, bool pvp) : m_enabled(enabled), m_pvp(pvp)
{
    if (!m_enabled)
        return;
    if (m_pvp)
    {
        // The corridor map: one Base at each end, walls all around (spawnCorridorWalls).
        m_basePos = glm::vec3(-55.0f, 0.0f, 0.0f);
        m_playerStart = m_basePos + glm::vec3(0.0f, 1.0f, -6.0f);
        m_pvpEnemyBasePos = glm::vec3(55.0f, 0.0f, 0.0f);
        Log::info("PvP TEST mode: corridor arena, no ambient field/enemies; per-client teams + resources");
    }

    {
        // Gameplay tweaks persist between runs and the server's values overrule the clients'.
        const Tweak::ScopedFlags scoped(ETweakFlags::Saved | ETweakFlags::Synced);
        Tweak::floatVar("Game/World", "Safe radius", &m_safeRadius, 2.0f, 500.0f, 1.0f);
        Tweak::floatVar("Game/World", "Field gradient", &m_fieldSlope, 0.001f, 0.5f, 0.001f);
        Tweak::floatVar("Game/World", "Field max strength", &m_fieldMaxStrength, 0.2f, 10.0f, 0.05f);
        Tweak::floatVar("Game/Construction", "Refill radius", &m_refillRadius, 1.0f, 30.0f, 0.25f);
        Tweak::floatVar("Game/Construction", "Refill rate", &m_refillRate, 0.5f, 100.0f, 0.5f);
        Tweak::floatVar("Game/Construction", "Player build radius", &m_buildRadius, 1.0f, 30.0f, 0.25f);
        Tweak::floatVar("Game/Construction", "Player build rate", &m_playerBuildRate, 0.5f, 100.0f, 0.5f);
    }
    m_camera.registerTweaks();
    m_player.registerTweaks();
    m_structures.registerTweaks();
    m_npcs.registerTweaks();

    m_mouse = Globals::input.addMouseListener();
    m_mouse->onMouseMoved = [this](const SDL_MouseMotionEvent& evt)
    {
        const glm::vec2 pos(float(evt.x), float(evt.y));
        if (m_rmbDown)
            m_dragDeltaX += pos.x - m_mousePos.x;
        m_mousePos = pos;
    };
    m_mouse->onMousePressed = [this](const SDL_MouseButtonEvent& evt)
    {
        if (evt.button == 3)
            m_rmbDown = true;
        if (evt.button == 1 && Globals::input.isWindowHasFocus() && Globals::ui.isViewportFocused()
            && !Globals::input.isMouseCaptured())
            m_placeClicked = true;
    };
    m_mouse->onMouseReleased = [this](const SDL_MouseButtonEvent& evt)
    {
        if (evt.button == 3)
            m_rmbDown = false;
    };
    m_mouse->onMouseWheelMoved = [this](const SDL_MouseWheelEvent& evt)
    {
        m_wheelAccum += float(evt.y);
    };
}

GameMatch::~GameMatch()
{
    if (!m_enabled)
        return;
    Globals::forceSystem.setAmbientField(1u, glm::vec2(0.0f), 0.0f, 0.0f, 0.0f); // slope 0 = off
    m_npcs.clear();
    m_structures.clear();
    m_player.despawn();
    if (m_ground)
        Globals::world.removeRootEntity(m_ground.get()); // corridor walls are its children — they go with it
}

void GameMatch::spawnWorld()
{
    if (!m_enabled)
        return;
    m_isServer = Globals::networkManager.role() == ENetRole::Server;
    m_isClient = Globals::networkManager.role() == ENetRole::Client;

    m_ground = Globals::world.spawnAssetFile("Entities/Game/ground.pre",
        Transform(glm::vec3(0.0f, -0.5f, 0.0f)), true); // box top = walkable y 0
    if (m_ground)
    {
        m_ground->setName("Ground");
        Globals::world.addRootEntity(m_ground);
    }

    Globals::networkManager.setOnGameEvent([this](std::string_view name) { handleNetEvent(name); });
    if (m_isServer)
    {
        // Structure changes broadcast to the client mirrors; only Gq* requests may come FROM clients.
        m_structures.onStructurePlaced = [this](int index) { sendStructurePlaced(index); };
        m_structures.onStructureRemoved = [this](uint32 id)
        {
            uint8 buffer[8];
            NetWriter writer(buffer);
            writer.write<uint32>(id);
            Globals::networkManager.fireNetworkEvent("GRm", writer.data());
        };
        m_structures.onCableChanged = [this](uint32 a, uint32 b, ECableType t, bool removed)
        {
            sendCableChanged(a, b, t, removed);
        };
        Globals::networkManager.setEventFilter([](uint32, std::string_view name, std::span<const uint8> data, Entity*)
        {
            return name.size() >= 2 && name[0] == 'G' && name[1] == 'q' && data.size() <= 64;
        });
    }

    m_structures.setPvp(m_pvp);
    m_npcs.setPvp(m_pvp);
    m_structures.spawnNodes(); // deterministic on every instance (no sync needed)
    if (m_pvp)
    {
        spawnCorridorWalls();  // deterministic local scenery too — every role builds its own copy
        // Placement stays INSIDE the arena (footprints may not clip the border wall ring).
        m_structures.setPlacementBounds(
            glm::vec2(-c_pvpCorridorHalfLength, -c_pvpCorridorHalfWidth),
            glm::vec2(c_pvpCorridorHalfLength, c_pvpCorridorHalfWidth));
    }
    if (!m_isClient)
    {
        m_structures.spawnBase(m_basePos); // clients get it through the GPl mirror stream
        if (m_pvp)
            m_structures.spawnBase(m_pvpEnemyBasePos, 1); // the opposing team's anchor + income
        else
            m_npcs.spawnEnemyCamps(m_basePos); // camp/unit/shot entities replicate (Component Network)
        m_player.spawn(m_playerStart);     // clients ADOPT the capsule the server spawns for them
        // The server's own capsule is a PRIMARY: never handed to a client by the proximity
        // transfer, and it re-claims transferred objects it walks up to (the client symmetric).
        if (m_isServer && m_player.entity())
            Globals::networkManager.setServerPrimary(*m_player.entity(), true);
    }

    Globals::gameHud.setHotbarVisible(false); // hidden until Build mode (B); setMode populates the
    Globals::gameHud.selectSlot(0);           // grid hotbar (categories -> items) when entered

    Log::info("Game mode: B = build (hotbar 1-7, LMB/F confirms), X = delete, V = select. The enemy "
              "field surrounds you — build powered Emitters on the frontier to expand the safe zone");
}

void GameMatch::spawnCorridorWalls()
{
    // A ring of 10 m blocks: two long sides whose inner faces sit ON the half-width, and two end
    // caps behind the bases (~52 segments). Parented under the Ground entity — the scene root
    // stays lean and the whole ring dies with it. The static body bakes at the WORLD pose at
    // spawn; the pos rewrite below re-expresses it in the ground's local space for the render.
    const auto spawnSegment = [this](float x, float z)
    {
        const glm::vec3 worldPos(x, c_pvpWallStep * 0.5f, z);
        EntityPtr wall = Globals::world.spawnAssetFile("Entities/Game/borderwall.pre",
            Transform(worldPos), true);
        if (!wall)
            return;
        wall->setName("Border");
        if (m_ground)
        {
            wall->reparentEntity(m_ground.get());
            wall->pos = worldPos - m_ground->pos; // ground has identity rot/scale
        }
        else
            Globals::world.addRootEntity(wall);
    };
    const float half = c_pvpWallStep * 0.5f;
    for (float x = -c_pvpCorridorHalfLength + half; x < c_pvpCorridorHalfLength; x += c_pvpWallStep)
    {
        spawnSegment(x, -c_pvpCorridorHalfWidth - half);
        spawnSegment(x, c_pvpCorridorHalfWidth + half);
    }
    for (float z = -c_pvpCorridorHalfWidth - c_pvpWallStep + half;
         z < c_pvpCorridorHalfWidth + c_pvpWallStep; z += c_pvpWallStep)
    {
        spawnSegment(-c_pvpCorridorHalfLength - half, z);
        spawnSegment(c_pvpCorridorHalfLength + half, z);
    }
}

void GameMatch::onClientJoined(uint32 clientId)
{
    // Their player: spawned server-side (player.pre carries Component Network), handed over with
    // setOwner in the SAME frame; the client adopts + drives it through the claim stream.
    // PvP: clients spawn beside THEIR team's Base instead of the server's.
    const glm::vec3 clientStart = m_pvp ? m_pvpEnemyBasePos + glm::vec3(0.0f, 1.0f, -6.0f) : m_playerStart;
    EntityPtr player = Globals::world.spawnAssetFile("Entities/Game/player.pre",
        Transform(clientStart + glm::vec3(2.0f * (float)(clientId % 5), 0.0f, 1.5f * (float)(clientId % 3))), true);
    if (player)
    {
        player->setName(("Player " + std::to_string(clientId)).c_str());
        Globals::networkManager.setOwner(*player, clientId);
        if (m_pvp) // the server-side twin's field carries the client's team (readbacks, visuals)
            if (ForceComponent* fc = getComponent<ForceComponent>(player.get()))
                fc->emitter.setTeam(teamOfClient(clientId));
        Globals::world.addRootEntity(player);
        m_clientPlayers[clientId] = std::move(player);
    }
    // World-state replay for the late joiner: every structure + cable, broadcast (mirrorPlace/
    // mirrorCable are idempotent, so already-connected clients shrug the duplicates off).
    for (int i = 0; i < m_structures.structureCount(); ++i)
        sendStructurePlaced(i);
    uint32 idA, idB;
    ECableType type;
    for (int i = 0; i < m_structures.cableTotal(); ++i)
    {
        m_structures.cableAt(i, idA, idB, type);
        sendCableChanged(idA, idB, type, false);
    }
    Log::info("Game: client " + std::to_string(clientId) + " joined, world replayed");
}

void GameMatch::onClientLeft(uint32 clientId)
{
    if (const auto it = m_clientPlayers.find(clientId); it != m_clientPlayers.end())
    {
        if (it->second)
        {
            if (const NetworkComponent* net = getComponent<NetworkComponent>(it->second.get()))
                m_remoteShields.erase(net->netId);
            Globals::world.removeRootEntity(it->second.get());
        }
        m_clientPlayers.erase(it);
    }
    m_clientFireCooldown.erase(clientId);
    m_clientMeleeCooldown.erase(clientId);
    m_clientMaterials.erase(clientId);
}

// Shield mirror quantization: fractions as u8; output as u8 over a fixed range (covers the player
// max output and every unit multiplier at ~0.03 resolution).
constexpr float c_shieldOutputRange = 8.0f;
static uint8 packFrac8(float v)
{
    return (uint8)glm::clamp(v * 255.0f, 0.0f, 255.0f);
}

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

void GameMatch::sendCableChanged(uint32 idA, uint32 idB, ECableType type, bool removed)
{
    uint8 buffer[16];
    NetWriter writer(buffer);
    writer.write<uint32>(idA);
    writer.write<uint32>(idB);
    writer.write<uint8>((uint8)type);
    writer.write<uint8>(removed ? 1u : 0u);
    Globals::networkManager.fireNetworkEvent("GCb", writer.data());
}

void GameMatch::sendStats()
{
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
    const int count = glm::min(m_structures.structureCount(), 79); // 11B each + the 146B header
                                                                   // stays under the 1024B event cap
    writer.write<uint16>((uint16)count);
    const auto frac8 = [](float v, float max) {
        return (uint8)glm::clamp(max > 0.0f ? v / max * 255.0f : 0.0f, 0.0f, 255.0f); };
    for (int i = 0; i < count; ++i)
    {
        writer.write<uint32>(m_structures.structureId(i));
        writer.write<uint8>(frac8(m_structures.structureHealth(i), m_structures.structureHealthMax()));
        writer.write<uint8>(frac8(m_structures.structureCharge(i), m_structures.structureCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureFuel(i), m_structures.structureFuelCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureMinerals(i), m_structures.structureMineralCapacity(i)));
        writer.write<uint8>(frac8(m_structures.structureOutputFrac(i), 1.0f));
        writer.write<uint8>(frac8(m_structures.connectorUtilization(i), 1.0f));
        writer.write<uint8>((uint8)((m_structures.structurePowered(i) ? 1u : 0u)
            | (m_structures.structureBlueprint(i) ? 2u : 0u))); // status bits (health IS progress)
    }
    Globals::networkManager.fireNetworkEvent("GSt", writer.data());
}

void GameMatch::sendShieldStates()
{
    // Full netId-keyed shield snapshot (~10 Hz + an immediate flush on any collapse/reboot edge):
    // units + friendlies from the npc lists, our own player, and every client player as last
    // reported through GqE. Snapshot semantics — the receiver replaces its map wholesale, so dead
    // entities drop out without pruning.
    std::vector<ShieldNetState> records;
    m_npcs.collectShieldStates(records);
    if (Entity* own = m_player.entity())
    {
        const NetworkComponent* net = getComponent<NetworkComponent>(own);
        if (net && net->netId != 0)
        {
            ShieldNetState s;
            s.netId = net->netId;
            s.healthFrac = m_player.health() / glm::max(m_player.healthMax(), 1e-3f);
            s.energyFrac = m_player.shieldFrac();
            if (const ForceComponent* fc = getComponent<ForceComponent>(own))
                s.output = fc->emitter.getOutput();
            s.collapsed = m_player.shieldCollapsed();
            s.kind = 2;
            s.team = (uint8)m_team;
            s.materialsFrac = m_player.materials() / glm::max(m_player.materialsMax(), 1e-3f);
            records.push_back(s);
        }
    }
    // Client players carry their server-authoritative materials (netId -> clientId resolved here).
    std::unordered_map<uint32, uint32> clientByNetId;
    for (const auto& [clientId, p] : m_clientPlayers)
        if (p)
            if (const NetworkComponent* net = getComponent<NetworkComponent>(p.get()); net && net->netId != 0)
                clientByNetId.emplace(net->netId, clientId);
    for (const auto& [netId, rs] : m_remoteShields)
    {
        ShieldNetState s{ netId, rs.healthFrac, rs.energyFrac, rs.output, rs.collapsed, rs.kind, rs.team };
        if (const auto it = clientByNetId.find(netId); it != clientByNetId.end())
            if (const auto materials = m_clientMaterials.find(it->second); materials != m_clientMaterials.end())
                s.materialsFrac = materials->second / glm::max(m_player.materialsMax(), 1e-3f);
        records.push_back(s);
    }

    const int count = glm::min((int)records.size(), 110); // 9B each under the 1024B event cap
    uint8 buffer[1000];
    NetWriter writer(buffer);
    writer.write<uint16>((uint16)count);
    for (int i = 0; i < count; ++i)
    {
        const ShieldNetState& s = records[i];
        writer.write<uint32>(s.netId);
        writer.write<uint8>(packFrac8(s.healthFrac));
        writer.write<uint8>(packFrac8(s.energyFrac));
        writer.write<uint8>(packFrac8(s.output / c_shieldOutputRange));
        writer.write<uint8>(packFrac8(s.materialsFrac));
        writer.write<uint8>((uint8)((s.collapsed ? 1u : 0u) | (uint32(s.kind) << 1)
            | (uint32(glm::min((int)s.team, 7)) << 4)));
    }
    Globals::networkManager.fireNetworkEvent("GSh", writer.data());
}

void GameMatch::sendShieldReport()
{
    // Owner -> server: our locally computed shield result. The server applies it to our twin's
    // emitter (its view and unit interactions match) and re-emits it to other clients via GSh.
    Entity* own = m_player.entity();
    if (!own)
        return;
    float output = 0.01f;
    if (const ForceComponent* fc = getComponent<ForceComponent>(own))
        output = fc->emitter.getOutput();
    uint8 buffer[8];
    NetWriter writer(buffer);
    writer.write<uint8>(packFrac8(m_player.health() / glm::max(m_player.healthMax(), 1e-3f)));
    writer.write<uint8>(packFrac8(m_player.shieldFrac()));
    writer.write<uint8>(packFrac8(output / c_shieldOutputRange));
    writer.write<uint8>((uint8)((m_player.shieldCollapsed() ? 1u : 0u) | (2u << 1)));
    Globals::networkManager.fireNetworkEvent("GqE", writer.data());
}

// Server-side melee for a CLIENT's swing: npc damage + the physics shove cone, at their twin.
void GameMatch::serverMeleeFrom(const glm::vec3& pos, const glm::vec3& dirPlanar, uint8 team)
{
    m_npcs.applyPlayerMelee(pos, dirPlanar, m_player.meleeRange(), team);
    for (const EntityPtr& root : Globals::world.rootEntities())
    {
        PhysicsComponent* pc = getComponent<PhysicsComponent>(root.get());
        if (!pc || pc->bodyType != EPhysicsBodyType::Dynamic || !pc->body.isValid())
            continue;
        const glm::vec3 d = pc->body.getPosition() - pos;
        const glm::vec2 planar(d.x, d.z);
        const float dist = glm::length(planar);
        if (dist > m_player.meleeRange() || dist < 1e-3f
            || glm::dot(planar / dist, glm::vec2(dirPlanar.x, dirPlanar.z)) < 0.5f)
            continue;
        pc->body.applyImpulse((glm::normalize(glm::vec3(d.x, 0.0f, d.z)) + glm::vec3(0.0f, 0.4f, 0.0f)) * 600.0f);
    }
}

void GameMatch::handleNetEvent(std::string_view name)
{
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
        else if (name == "GCb")
        {
            const uint32 idA = reader.read<uint32>();
            const uint32 idB = reader.read<uint32>();
            const uint8 type = reader.read<uint8>();
            const uint8 removed = reader.read<uint8>();
            if (!reader.overflowed() && type < (uint8)ECableType::Count)
                m_structures.mirrorCable(idA, idB, (ECableType)type, removed != 0);
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
        else if (name == "GSh")
        {
            // Shield snapshot: apply each record's output to the entity's LOCAL emitter (remote
            // bubbles now render true collapse/drain state) and keep the values for overhead
            // bars. Wholesale replace; our own player is skipped — the owner simulates locally.
            uint32 ownNetId = 0;
            if (Entity* own = m_player.entity())
                if (const NetworkComponent* net = getComponent<NetworkComponent>(own))
                    ownNetId = net->netId;
            m_remoteShields.clear();
            const uint16 count = reader.read<uint16>();
            for (uint16 i = 0; i < count && !reader.overflowed(); ++i)
            {
                const uint32 netId = reader.read<uint32>();
                const uint8 health = reader.read<uint8>(), energy = reader.read<uint8>();
                const uint8 output = reader.read<uint8>(), materials = reader.read<uint8>();
                const uint8 flags = reader.read<uint8>();
                if (reader.overflowed() || netId == 0)
                    continue;
                if (netId == ownNetId)
                {
                    // Our shield/health are locally simulated, but MATERIALS are server-authoritative.
                    m_player.setMaterials(materials / 255.0f * m_player.materialsMax());
                    continue;
                }
                RemoteShield rs;
                rs.healthFrac = health / 255.0f;
                rs.energyFrac = energy / 255.0f;
                rs.output = output / 255.0f * c_shieldOutputRange;
                rs.collapsed = (flags & 1u) != 0;
                rs.kind = (uint8)((flags >> 1) & 7u);
                rs.team = (uint8)((flags >> 4) & 7u);
                if (rs.kind < 3) // camp structures/cores are health-only — their bubble is authored
                    if (Entity* entity = Globals::networkManager.findEntity(netId))
                        if (ForceComponent* fc = getComponent<ForceComponent>(entity))
                            fc->emitter.setOutput(glm::max(rs.output, 0.01f));
                m_remoteShields[netId] = rs;
            }
        }
        return;
    }
    if (!m_isServer || name[1] != 'q')
        return;
    // Client requests, validated here + through the same authority seams local input uses.
    const uint32 sender = Globals::networkManager.currentEventSender();
    if (name == "GqP")
    {
        const uint8 type = reader.read<uint8>();
        const float x = reader.read<float>(), z = reader.read<float>();
        const int16 nodeIndex = reader.read<int16>();
        const float fx = reader.read<float>(), fz = reader.read<float>();
        if (!reader.overflowed() && type < NumPlaceableStructures)
            m_structures.queuePlaceRequest((EStructureType)type, glm::vec3(x, 0.0f, z), nodeIndex,
                glm::vec3(fx, 0.0f, fz), requestTeam(sender));
    }
    else if (name == "GqC")
    {
        const uint32 idA = reader.read<uint32>();
        const uint32 idB = reader.read<uint32>();
        const uint8 type = reader.read<uint8>();
        if (!reader.overflowed() && type < (uint8)ECableType::Count)
            m_structures.queueCableRequest(idA, idB, (ECableType)type, requestTeam(sender));
    }
    else if (name == "GqD")
    {
        const uint32 id = reader.read<uint32>();
        if (!reader.overflowed())
            m_structures.queueDemolishRequest(id, requestTeam(sender));
    }
    else if (name == "GqE")
    {
        // The owner's self-reported shield state: apply to their twin's emitter (server-side
        // bubble + unit interactions match the owner's sim) and keep it for the GSh re-emit.
        const auto it = m_clientPlayers.find(sender);
        if (it == m_clientPlayers.end() || !it->second)
            return;
        const uint8 health = reader.read<uint8>(), energy = reader.read<uint8>();
        const uint8 output = reader.read<uint8>(), flags = reader.read<uint8>();
        const NetworkComponent* net = getComponent<NetworkComponent>(it->second.get());
        if (reader.overflowed() || !net || net->netId == 0)
            return;
        RemoteShield rs;
        rs.healthFrac = health / 255.0f;
        rs.energyFrac = energy / 255.0f;
        rs.output = output / 255.0f * c_shieldOutputRange;
        rs.collapsed = (flags & 1u) != 0;
        rs.kind = 2;
        rs.team = requestTeam(sender);
        if (ForceComponent* fc = getComponent<ForceComponent>(it->second.get()))
            fc->emitter.setOutput(glm::max(rs.output, 0.01f));
        m_remoteShields[net->netId] = rs;
    }
    else if (name == "GqS" || name == "GqM")
    {
        const auto it = m_clientPlayers.find(sender);
        PhysicsComponent* pc = it != m_clientPlayers.end() && it->second
            ? getComponent<PhysicsComponent>(it->second.get()) : nullptr;
        if (!pc || !pc->body.isValid())
            return;
        const glm::vec3 from = pc->body.getPosition();
        const float dx = reader.read<float>(), dy = reader.read<float>(), dz = reader.read<float>();
        const glm::vec3 dir(dx, dy, dz);
        if (reader.overflowed() || glm::dot(dir, dir) < 1e-6f || !std::isfinite(dx + dy + dz))
            return;
        if (name == "GqS")
        {
            float& cooldown = m_clientFireCooldown[sender];
            if (cooldown > 0.0f)
                return;
            cooldown = m_player.fireInterval();
            const glm::vec3 shotDir = glm::normalize(dir);
            if (EntityPtr p = Globals::world.spawnAssetFile("Entities/Game/projectile.pre",
                Transform(from + glm::vec3(0.0f, 0.5f, 0.0f) + shotDir * 1.2f), true))
            {
                p->setName("Projectile");
                if (PhysicsComponent* ppc = getComponent<PhysicsComponent>(p.get()))
                    ppc->body.setLinearVelocity(shotDir * 30.0f);
                if (ForceComponent* fc = getComponent<ForceComponent>(p.get()))
                    fc->emitter.setTeam(requestTeam(sender)); // the shooter's team field
                Globals::world.addRootEntity(p);
                m_player.trackProjectile(std::move(p)); // rides the aging + field-push loop
            }
        }
        else
        {
            float& cooldown = m_clientMeleeCooldown[sender];
            if (cooldown > 0.0f)
                return;
            cooldown = m_player.meleeInterval();
            serverMeleeFrom(from, glm::normalize(glm::vec3(dir.x, 0.0f, dir.z)), requestTeam(sender));
        }
    }
}

void GameMatch::update(float deltaSec)
{
    if (!m_enabled)
        return;

    // The ambient enemy field: zero within "Safe radius" of the Base, growing with distance.
    // Pushed every tick on EVERY instance (clients evaluate fields locally) — tweaks stay live.
    // PvP: slope 0 = no ambient field at all (the map belongs to the players).
    Globals::forceSystem.setAmbientField(1u, glm::vec2(m_basePos.x, m_basePos.z), m_safeRadius,
        m_pvp ? 0.0f : m_fieldSlope, m_fieldMaxStrength);

    if (m_isClient)
    {
        // PvP: our team comes from the Welcome's clientId — latch it the frame it appears, BEFORE
        // adoption, so the adopted capsule's field re-teams immediately.
        if (m_pvp && m_team == 0)
            if (const uint32 clientId = Globals::networkManager.localClientId(); clientId != 0)
            {
                m_team = teamOfClient(clientId);
                m_player.setTeam(m_team);
                Log::info("PvP: we are team " + std::to_string(m_team));
            }
        // CLIENT: adopt + drive our own capsule (the owner simulates; claims stream the state);
        // shield/health run on LOCAL readbacks against the mirrored fields. World sim is remote.
        m_player.clientAdopt(m_pvp ? m_pvpEnemyBasePos + glm::vec3(0.0f, 1.0f, -6.0f) : m_playerStart);
        m_player.tickMovement(m_camera.forwardPlanar(), deltaSec);
        m_player.tickShieldAndHealth(deltaSec);
        m_structures.tickMirror(deltaSec);
        m_reportTimer -= deltaSec;
        if (m_player.takeShieldEdge() || m_reportTimer <= 0.0f)
        {
            sendShieldReport(); // collapse/reboot edges flush immediately; else ~10 Hz
            m_reportTimer = 0.1f;
        }
        return;
    }

    const glm::vec3 playerPos = m_player.bodyPos();
    m_structures.tickAuthority(playerPos, deltaSec);
    m_player.tickMovement(m_camera.forwardPlanar(), deltaSec);
    m_player.tickShieldAndHealth(deltaSec);
    m_player.tickCombat(m_camera.forwardPlanar(), deltaSec);

    // EVERY player body + team (ours + each client capsule): unit fallback targeting and camp
    // turrets see everyone, not just the server's own player.
    std::vector<PlayerInfo> players;
    players.reserve(1 + m_clientPlayers.size());
    players.push_back({ playerPos, (uint8)m_team });
    for (const auto& [id, p] : m_clientPlayers)
        if (p)
            if (const PhysicsComponent* pc = getComponent<PhysicsComponent>(p.get()); pc && pc->body.isValid())
                players.push_back({ pc->body.getPosition(), requestTeam(id) });

    // MATERIALS loop (server-authoritative for every player): refill the carried inventory from
    // nearby own-team Silos/Base, invest it into nearby blueprints.
    {
        const auto tickPlayerMaterials = [&](const glm::vec3& pos, uint8 team, float& materials)
        {
            materials += m_structures.takeStoredMinerals(pos, m_refillRadius, team,
                glm::min(m_refillRate * deltaSec, m_player.materialsMax() - materials));
            materials -= m_structures.fundNearbyBlueprint(pos, m_buildRadius, team,
                glm::min(m_playerBuildRate * deltaSec, materials));
        };
        float serverMaterials = m_player.materials();
        tickPlayerMaterials(playerPos, (uint8)m_team, serverMaterials);
        m_player.setMaterials(serverMaterials);
        for (const auto& [id, p] : m_clientPlayers)
            if (p)
                if (const PhysicsComponent* pc = getComponent<PhysicsComponent>(p.get()); pc && pc->body.isValid())
                    tickPlayerMaterials(pc->body.getPosition(), requestTeam(id), m_clientMaterials[id]);
    }

    if (!m_pvp)
    {
        // Waves spawn on the frontier ring (where the ambient crosses iso), all around the safe zone.
        const float iso = Globals::forceSystem.getParams().isoThreshold;
        const float frontierIso = m_safeRadius + iso / glm::max(m_fieldSlope, 1e-4f);
        m_npcs.tickAuthority(m_basePos, frontierIso, players, m_structures, deltaSec);
        m_npcs.tickFriendlies(m_structures, deltaSec);
        m_npcs.tickTurrets(m_structures, deltaSec);
        m_npcs.tickEnemyStructures(m_structures, players, deltaSec);
        if (m_player.meleeJustSwung()) // the shove happened in tickCombat; the damage lands here
            m_npcs.applyPlayerMelee(m_player.bodyPos(), m_player.meleeDir(), m_player.meleeRange());
    }
    else
    {
        // PvP: no waves/friendlies/camps — barracks produce per-team ATTACK units that ride the
        // same unit sim and march on enemy structures/players.
        m_npcs.tickPvpBarracks(m_structures, deltaSec);
        m_npcs.tickAuthority(m_basePos, 0.0f, players, m_structures, deltaSec);
        if (m_player.meleeJustSwung())
            m_npcs.applyPlayerMelee(m_player.bodyPos(), m_player.meleeDir(), m_player.meleeRange(), (uint8)m_team);
    }

    if (m_isServer)
    {
        for (auto& [id, cooldown] : m_clientFireCooldown)
            cooldown = glm::max(0.0f, cooldown - deltaSec);
        for (auto& [id, cooldown] : m_clientMeleeCooldown)
            cooldown = glm::max(0.0f, cooldown - deltaSec);
        m_statTimer -= deltaSec;
        if (m_statTimer <= 0.0f)
        {
            sendStats();
            m_statTimer = 0.2f; // ~5 Hz volatile-state mirror
        }
        const bool npcEdge = m_npcs.takeShieldCollapseEdge();    // both polled every tick so the
        const bool playerEdge = m_player.takeShieldEdge();       // flags never latch stale
        m_shieldTimer -= deltaSec;
        if (npcEdge || playerEdge || m_shieldTimer <= 0.0f)
        {
            sendShieldStates(); // collapse edges flush the same tick; else the ~10 Hz mirror
            m_shieldTimer = 0.1f;
        }
    }
}

// Build/delete/cable intents: local queue on the authority, Gq* request events from a client —
// the SAME validation runs server-side either way (the seams the plan called for).
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

void GameMatch::requestCable(uint32 idA, uint32 idB, ECableType type)
{
    if (!m_isClient)
    {
        m_structures.queueCableRequest(idA, idB, type, (uint8)m_team);
        return;
    }
    uint8 buffer[16];
    NetWriter writer(buffer);
    writer.write<uint32>(idA);
    writer.write<uint32>(idB);
    writer.write<uint8>((uint8)type);
    Globals::networkManager.fireNetworkEvent("GqC", writer.data());
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

GameMatch::Aim GameMatch::computeAim(const Camera& camera, EStructureType type) const
{
    Aim aim;
    aim.type = type;

    glm::vec3 pos;
    if (!aimGroundPoint(camera, pos))
        return aim;

    // Clamp to build range around the player, flatten to the whitebox ground plane.
    const glm::vec3 playerPos = m_player.bodyPos();
    glm::vec2 offset = glm::vec2(pos.x, pos.z) - glm::vec2(playerPos.x, playerPos.z);
    const float dist = glm::length(offset);
    if (dist > m_structures.placeRange())
        offset *= m_structures.placeRange() / dist;
    aim.pos = glm::vec3(playerPos.x + offset.x, 0.0f, playerPos.z + offset.y);

    if (aim.type == EStructureType::Extractor)
    {
        // Extractors only build ON a free resource node: snap the ghost to the nearest one.
        aim.nodeIndex = m_structures.findFreeNodeNear(aim.pos, m_structures.extractorSnapRadius());
        if (aim.nodeIndex < 0)
            return aim; // no free node under the cursor — invalid, no ghost
        aim.pos = m_structures.nodeGroundPos(aim.nodeIndex);
    }
    aim.pos = StructureSystem::snapToGrid(aim.type, aim.pos); // grid-aligned (extractors too)

    aim.valid = true;
    // Placing a blueprint is free — "affordable" now means the footprint's cells are unoccupied
    // (drives the red ghost AND gates the confirm click).
    aim.affordable = m_structures.cellsFree(aim.type, aim.pos);
    return aim;
}

bool GameMatch::aimGroundPoint(const Camera& camera, glm::vec3& outPos) const
{
    const Ray ray = camera.screenToRay(Globals::ui.getViewportRect(), m_mousePos);
    const PhysicsComponent* ppc = m_player.entity() ? getComponent<PhysicsComponent>(m_player.entity()) : nullptr;
    const PhysicsWorld::RayHit hit = Globals::physics.castRayClosest(ray.origin, ray.dir * 500.0f,
        PhysicsLayers::All, ppc ? &ppc->body : nullptr);
    if (hit.hit)
        outPos = hit.point;
    else if (ray.dir.y < -1e-4f) // no collider under the cursor: fall back to the y=0 plane
        outPos = ray.origin + ray.dir * (-ray.origin.y / ray.dir.y);
    else
        return false;
    return true;
}

int GameMatch::hoveredStructure(const Camera& camera) const
{
    glm::vec3 aimPos;
    if (!aimGroundPoint(camera, aimPos))
        return -1;
    return m_structures.findConnectableNear(aimPos, 4.0f);
}

// Slot labels/counts for the current grid level: categories at the top, the picked category's
// items (+ "Back" on key 0) inside one. Called every Build-mode frame — counts stay live.
void GameMatch::refreshBuildHotbar()
{
    GameHud& hud = Globals::gameHud;
    const std::span<const BuildItem> items = buildCategoryItems(m_buildCategory);
    for (int i = 0; i < (int)items.size(); ++i)
        hud.setSlot(i, items[i].label, items[i].isCable
            ? m_structures.cableCount(items[i].cable)
            : m_structures.affordableCount(items[i].structure, (uint8)m_team));
    for (int i = (int)items.size(); i < GameHud::NumSlots; ++i)
        hud.clearSlot(i);
    if (m_buildSelection >= 0)
        hud.selectSlot(m_buildSelection);
}

void GameMatch::setMode(EPlayerMode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_cablePendingId = 0;
    m_selectedId = 0;
    m_lanceAiming = false;
    m_buildCategory = glm::max(m_buildCategory, 0); // default to Emitters; the category keys set it
    m_buildSelection = -1;
    Globals::gameHud.setHotbarVisible(mode == EPlayerMode::Build); // the hotbar IS the Build indicator
    if (mode == EPlayerMode::Build)
        refreshBuildHotbar();
    switch (mode)
    {
    case EPlayerMode::Build:  break; // the category switch logs its own line (updateModeSwitching)
    case EPlayerMode::Delete: Log::info("Delete mode (X): click a structure to demolish — X to exit"); break;
    case EPlayerMode::Select: Log::info("Select mode (Tab): click a structure to inspect — Tab to exit"); break;
    case EPlayerMode::None:   Log::info("Combat mode: LMB shoot, F melee — B/V/C build, X delete, Tab select"); break;
    }
}

void GameMatch::updateModeSwitching()
{
    Input& input = Globals::input;
    const bool focused = input.isWindowHasFocus() && Globals::ui.isViewportFocused();
    // B/V/C jump straight into Build with that category (fast chains: b->1->LMB, v->3->LMB);
    // the ACTIVE category's key exits to neutral, another one switches category in place.
    const SDL_Scancode catKeys[3] = { SDL_Scancode::SDL_SCANCODE_B, SDL_Scancode::SDL_SCANCODE_V, SDL_Scancode::SDL_SCANCODE_C };
    for (int i = 0; i < 3; ++i)
    {
        const bool down = focused && input.isKeyDown(catKeys[i]);
        if (down && !m_modeKeyWasDown[i])
        {
            if (m_mode == EPlayerMode::Build && m_buildCategory == i)
                setMode(EPlayerMode::None);
            else
            {
                if (m_mode != EPlayerMode::Build)
                    setMode(EPlayerMode::Build);
                m_buildCategory = i;
                m_buildSelection = -1;
                m_cablePendingId = 0;
                m_lanceAiming = false;
                refreshBuildHotbar();
                Log::info(std::string("Build: ") + c_buildCategories[i]
                    + " — 1-9 arms, 0 disarms, LMB/F places, same key exits");
            }
        }
        m_modeKeyWasDown[i] = down;
    }
    const SDL_Scancode modeKeys[2] = { SDL_Scancode::SDL_SCANCODE_X, SDL_Scancode::SDL_SCANCODE_TAB };
    const EPlayerMode modes[2] = { EPlayerMode::Delete, EPlayerMode::Select };
    for (int i = 0; i < 2; ++i)
    {
        const bool down = focused && input.isKeyDown(modeKeys[i]);
        if (down && !m_modeKeyWasDown[3 + i])
            setMode(m_mode == modes[i] ? EPlayerMode::None : modes[i]); // same key toggles back out
        m_modeKeyWasDown[3 + i] = down;
    }
}

// The Cable tool (hotbar slot 5): first click selects an endpoint, second click on another
// structure toggles the cable between them (create when valid, remove when it already exists —
// removal works at any distance). Clicking empty ground or the selected structure clears the
// selection. Selection persists by stable id, so a structure dying mid-selection just clears it.
void GameMatch::updateCableTool(const Camera& camera, bool confirmEdge, ECableType type)
{
    const int hover = hoveredStructure(camera);

    int pendingIdx = -1;
    if (m_cablePendingId != 0)
    {
        pendingIdx = m_structures.structureIndexById(m_cablePendingId);
        if (pendingIdx < 0)
            m_cablePendingId = 0; // the selected endpoint died
    }

    const glm::vec3 up(0.0f, 0.3f, 0.0f);
    if (hover >= 0)
        drawCircle(m_structures.structurePos(hover) * glm::vec3(1, 0, 1) + up, 1.6f,
            packColor(glm::vec3(0.9f, 0.9f, 0.9f)), 20);
    if (pendingIdx >= 0)
        drawCircle(m_structures.structurePos(pendingIdx) * glm::vec3(1, 0, 1) + up, 1.9f,
            packColor(glm::vec3(0.3f, 1.0f, 0.4f)), 20);
    if (pendingIdx >= 0 && hover >= 0 && hover != pendingIdx)
    {
        // Preview: cable-type hue = will connect (yellow basic / cyan heavy / orange pipe / blue
        // conveyor), white = will remove/retype the existing link, red = refused (range, no
        // Connector endpoint, or a Connector already carrying another medium).
        const glm::vec3 typeHue = type == ECableType::Pipe ? glm::vec3(1.0f, 0.55f, 0.15f)
            : type == ECableType::Conveyor ? glm::vec3(0.35f, 0.5f, 1.0f)
            : type == ECableType::Heavy ? glm::vec3(0.3f, 0.9f, 1.0f) : glm::vec3(0.9f, 0.9f, 0.3f);
        const bool exists = m_structures.cableExists(m_cablePendingId, m_structures.structureId(hover));
        const glm::vec3 previewColor = exists ? glm::vec3(0.9f, 0.9f, 0.9f)
            : m_structures.cableAllowed(pendingIdx, hover, type) ? typeHue : glm::vec3(1.0f, 0.3f, 0.2f);
        Globals::rendererVK.addDebugLine(m_structures.structurePos(pendingIdx),
            m_structures.structurePos(hover), packColor(previewColor));
    }

    if (!confirmEdge)
        return;
    if (hover < 0)
    {
        m_cablePendingId = 0; // clicked empty ground
        return;
    }
    const uint32 hoverId = m_structures.structureId(hover);
    if (m_cablePendingId == 0)
        m_cablePendingId = hoverId;
    else if (m_cablePendingId == hoverId)
        m_cablePendingId = 0;
    else
    {
        requestCable(m_cablePendingId, hoverId, type); // validated in the authority tick (server)
        m_cablePendingId = 0;
    }
}

void GameMatch::updateBuildMode(const Camera& camera, bool confirmEdge)
{
    // Grid navigation: polled 1..9,0 edges (SDL scancodes are contiguous; InputControls routes the
    // same keys to the HUD's selectSlot for the highlight — harmless duplication).
    Input& input = Globals::input;
    const bool focus = input.isWindowHasFocus() && Globals::ui.isViewportFocused();
    int pressed = -1;
    for (int k = 0; k < 10; ++k)
    {
        const bool down = focus && input.isKeyDown((SDL_Scancode)((int)SDL_Scancode::SDL_SCANCODE_1 + k));
        if (down && !m_numKeyWasDown[k])
            pressed = k;
        m_numKeyWasDown[k] = down;
    }
    if (pressed >= 0)
    {
        m_cablePendingId = 0; // switching tools drops a half-made connection (and half-done aims)
        m_lanceAiming = false;
        m_wallPlacing = false;
        if (pressed == 9) // key 0 disarms the ghost
            m_buildSelection = -1;
        else if (pressed < (int)buildCategoryItems(m_buildCategory).size())
            m_buildSelection = pressed;
    }
    refreshBuildHotbar();

    if (m_buildSelection < 0)
        return; // nothing armed — browsing the category
    const BuildItem& item = buildCategoryItems(m_buildCategory)[m_buildSelection];
    if (item.isCable)
    {
        updateCableTool(camera, confirmEdge, item.cable);
        return;
    }
    m_cablePendingId = 0;

    // Lance second click: the position is anchored — the cursor now aims the cone's facing
    // (relative to the anchor); confirm places, too-close clicks just keep waiting.
    if (m_lanceAiming)
    {
        const glm::vec3 up(0.0f, 0.3f, 0.0f);
        const uint32 color = packColor(glm::vec3(0.3f, 1.0f, 0.4f));
        drawCircle(m_lancePendingPos + up, 1.0f, color, 20);
        glm::vec3 target;
        glm::vec3 facing(0.0f);
        if (aimGroundPoint(camera, target))
        {
            const glm::vec2 d(target.x - m_lancePendingPos.x, target.z - m_lancePendingPos.z);
            if (glm::dot(d, d) > 0.25f)
            {
                const glm::vec2 dir = glm::normalize(d);
                facing = glm::vec3(dir.x, 0.0f, dir.y);
                // Preview: the aim line plus the lobe extent along the chosen facing.
                Globals::rendererVK.addDebugLine(m_lancePendingPos + up, target + up, color);
                Globals::rendererVK.addDebugLine(m_lancePendingPos + up,
                    m_lancePendingPos + facing * m_structures.emitterReachOf(EStructureType::Lance) + up,
                    packColor(glm::vec3(0.3f, 0.8f, 1.0f)));
            }
        }
        if (confirmEdge && glm::dot(facing, facing) > 0.5f)
        {
            requestPlace(EStructureType::Lance, m_lancePendingPos, -1, facing);
            m_lanceAiming = false;
        }
        return;
    }

    // Wall second click: segments preview along the anchored line; confirm queues one placement
    // per segment (each pays its own cost — a short stock places a partial wall).
    if (m_wallPlacing)
    {
        const Aim end = computeAim(camera, EStructureType::Wall);
        if (end.valid)
        {
            const glm::vec2 span(end.pos.x - m_wallStart.x, end.pos.z - m_wallStart.z);
            const float len = glm::length(span);
            const int segments = glm::clamp((int)(len / c_wallSegmentSpacing) + 1, 1, c_wallMaxSegments);
            const glm::vec2 dir = len > 1e-3f ? span / len : glm::vec2(0.0f);
            const int affordableSegs = c_wallMaxSegments; // blueprints are free to place
            for (int s = 0; s < segments; ++s)
            {
                const glm::vec3 p = m_wallStart + glm::vec3(dir.x, 0.0f, dir.y) * (c_wallSegmentSpacing * s);
                drawCircle(p + glm::vec3(0.0f, 0.3f, 0.0f), 0.9f,
                    packColor(s < affordableSegs ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f)), 12);
            }
            if (confirmEdge)
            {
                for (int s = 0; s < segments; ++s)
                    requestPlace(EStructureType::Wall,
                        m_wallStart + glm::vec3(dir.x, 0.0f, dir.y) * (c_wallSegmentSpacing * s),
                        -1, glm::vec3(0.0f));
                m_wallPlacing = false;
            }
        }
        return;
    }

    const Aim aim = computeAim(camera, item.structure);
    if (!aim.valid)
        return;
    const uint32 color = packColor(aim.affordable ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f));
    drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), 1.0f, color, 20);
    if (isEmitterType(aim.type)) // show the field footprint the powered variant would get
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.emitterReachOf(aim.type) * 0.5f, color, 32);
    drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.cableRange(),
        packColor(glm::vec3(0.4f, 0.4f, 0.45f)), 40); // cable reach from this spot
    if (confirmEdge && aim.affordable)
    {
        if (aim.type == EStructureType::Lance)
        {
            m_lanceAiming = true; // first click anchors; the next click aims the cone
            m_lancePendingPos = aim.pos;
        }
        else if (aim.type == EStructureType::Wall)
        {
            m_wallPlacing = true; // first click anchors the line start; the next click ends it
            m_wallStart = aim.pos;
        }
        else
            requestPlace(aim.type, aim.pos, aim.nodeIndex, glm::vec3(0.0f));
    }
}

void GameMatch::updateDeleteMode(const Camera& camera, bool confirmEdge)
{
    const int hover = hoveredStructure(camera);
    if (hover < 0)
        return;
    const bool deletable = m_structures.structureType(hover) != EStructureType::Base;
    drawCircle(m_structures.structurePos(hover) * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.3f, 0.0f), 1.6f,
        packColor(deletable ? glm::vec3(1.0f, 0.25f, 0.2f) : glm::vec3(0.5f, 0.5f, 0.5f)), 20);
    if (confirmEdge && deletable)
        requestDemolish(m_structures.structureId(hover)); // validated in the authority tick (server)
}

void GameMatch::updateSelectMode(const Camera& camera, bool confirmEdge)
{
    const int hover = hoveredStructure(camera);
    if (hover >= 0)
        drawCircle(m_structures.structurePos(hover) * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.3f, 0.0f), 1.6f,
            packColor(glm::vec3(0.9f, 0.9f, 0.9f)), 20);
    if (confirmEdge)
        m_selectedId = hover >= 0 ? m_structures.structureId(hover) : 0; // empty ground deselects

    int selected = -1;
    if (m_selectedId != 0)
    {
        selected = m_structures.structureIndexById(m_selectedId);
        if (selected < 0)
            m_selectedId = 0; // it died
    }
    if (selected >= 0) // highlight ring; the info block rides the world label (buildWorldLabels)
        drawCircle(m_structures.structurePos(selected) * glm::vec3(1, 0, 1) + glm::vec3(0.0f, 0.3f, 0.0f), 2.0f,
            packColor(glm::vec3(0.3f, 1.0f, 0.4f)), 24);
}

// World-anchored UI: a health bar above every damageable structure, plus name/HP/power info on the
// selected one. Projected here with THIS frame's final camera (worldToScreen), replaced wholesale
// each frame; the overlay just paints at the given viewport pixels.
void GameMatch::buildWorldLabels(const Camera& camera)
{
    std::vector<HudWorldLabel> labels;
    labels.reserve(m_structures.structureCount());
    const Rect& viewport = Globals::ui.getViewportRect();
    const int selected = m_selectedId != 0 ? m_structures.structureIndexById(m_selectedId) : -1;
    for (int i = 0; i < m_structures.structureCount(); ++i)
    {
        HudWorldLabel label;
        if (!camera.worldToScreen(viewport, m_structures.structureLabelAnchor(i), label.screenPos))
            continue;
        const EStructureType type = m_structures.structureType(i);
        const bool consumer = isEmitterType(type) || type == EStructureType::Extractor
            || type == EStructureType::Fabricator;
        if (type != EStructureType::Base) // the Base is invulnerable — no health bar
        {
            label.barValue = m_structures.structureHealth(i);
            label.barMax = m_structures.structureHealthMax();
            const float frac = label.barValue / label.barMax;
            // Blueprint: health IS the construction progress — the bar reads blue while building.
            label.barColor = m_structures.structureBlueprint(i) ? glm::vec3(0.5f, 0.7f, 1.0f)
                : glm::mix(glm::vec3(1.0f, 0.25f, 0.2f), glm::vec3(0.3f, 1.0f, 0.4f), frac);
        }
        const float energyCap = m_structures.structureCapacity(i);
        const float fuelCap = m_structures.structureFuelCapacity(i);
        const float mineralCap = m_structures.structureMineralCapacity(i);
        // Second bar: fuel (orange) on the fuel holders, MINERALS (blue) on the mineral stores,
        // energy (yellow) elsewhere.
        const bool fuelBar = type == EStructureType::Generator || type == EStructureType::FuelTank;
        const bool mineralBar = type == EStructureType::MineralSilo;
        if (m_structures.structureBlueprint(i))
        {
        } // blueprint: no second bar — the (blue) health bar IS the build progress
        else if (type == EStructureType::Connector)
        {
            // THROUGHPUT gauge, not the relay buffer: how hard the busiest attached link runs,
            // colored by the medium the connector carries. No links = no bar.
            if (const int medium = m_structures.connectorMedium(i); medium >= 0)
            {
                label.bar2Value = m_structures.connectorUtilization(i);
                label.bar2Max = 1.0f;
                label.bar2Color = medium == 1 ? glm::vec3(1.0f, 0.6f, 0.2f)
                    : medium == 2 ? glm::vec3(0.35f, 0.5f, 1.0f) : glm::vec3(1.0f, 0.9f, 0.3f);
            }
        }
        else if (fuelBar)
        {
            label.bar2Value = m_structures.structureFuel(i);
            label.bar2Max = fuelCap;
            label.bar2Color = glm::vec3(1.0f, 0.6f, 0.2f);
        }
        else if (mineralBar)
        {
            label.bar2Value = m_structures.structureMinerals(i);
            label.bar2Max = mineralCap;
            label.bar2Color = glm::vec3(0.35f, 0.5f, 1.0f);
        }
        else if (energyCap > 0.0f)
        {
            label.bar2Value = m_structures.structureCharge(i);
            label.bar2Max = energyCap;
            label.bar2Color = glm::vec3(1.0f, 0.9f, 0.3f);
        }
        if (i == selected)
        {
            label.emphasized = true;
            label.title = structureTypeName(type);
            char info[192];
            int len = type == EStructureType::Base
                ? snprintf(info, sizeof(info), "Invulnerable")
                : snprintf(info, sizeof(info), "HP %.0f / %.0f", label.barValue, label.barMax);
            if (energyCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nEnergy %.0f / %.0f",
                    m_structures.structureCharge(i), energyCap);
            if (fuelCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nFuel %.0f / %.0f",
                    m_structures.structureFuel(i), fuelCap);
            if (mineralCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nMinerals %.0f / %.0f",
                    m_structures.structureMinerals(i), mineralCap);
            if (consumer && len > 0 && len < (int)sizeof(info))
                snprintf(info + len, sizeof(info) - len, "\n%s",
                    m_structures.structurePowered(i) ? "Powered" : "No power");
            label.info = info;
        }
        labels.push_back(std::move(label));
    }
    // Enemy units: red health bar overhead; friendlies: green.
    for (int i = 0; i < m_npcs.unitCount(); ++i)
    {
        HudWorldLabel label;
        if (!camera.worldToScreen(viewport, m_npcs.unitPos(i) + glm::vec3(0.0f, 1.6f, 0.0f), label.screenPos))
            continue;
        label.barValue = m_npcs.unitHealth(i);
        label.barMax = m_npcs.unitHealthMax(i); // per-type: Brutes have triple bars, Runners half
        label.barColor = m_npcs.unitTeam(i) == (uint8)m_team // PvP: own barracks units read friendly
            ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.25f, 0.2f);
        label.bar2Value = m_npcs.unitEnergy(i); // shield battery (no regen) under the health bar
        label.bar2Max = m_npcs.unitEnergyMax(i);
        label.bar2Color = glm::vec3(1.0f, 0.9f, 0.3f);
        labels.push_back(std::move(label));
    }
    // Enemy camp structures: red health bar + name on the core (the kill priority).
    for (int i = 0; i < m_npcs.enemyStructureCount(); ++i)
    {
        HudWorldLabel label;
        if (!camera.worldToScreen(viewport, m_npcs.enemyStructurePos(i) + glm::vec3(0.0f, 2.4f, 0.0f), label.screenPos))
            continue;
        label.barValue = m_npcs.enemyStructureHealth(i);
        label.barMax = m_npcs.enemyStructureHealthMax(i);
        label.barColor = glm::vec3(1.0f, 0.25f, 0.2f);
        labels.push_back(std::move(label));
    }
    for (int i = 0; i < m_npcs.friendlyCount(); ++i)
    {
        HudWorldLabel label;
        if (!camera.worldToScreen(viewport, m_npcs.friendlyPos(i) + glm::vec3(0.0f, 1.4f, 0.0f), label.screenPos))
            continue;
        label.barValue = m_npcs.friendlyHealth(i);
        label.barMax = m_npcs.friendlyHealthMax();
        label.barColor = glm::vec3(0.3f, 1.0f, 0.4f);
        label.bar2Value = m_npcs.friendlyEnergy(i);
        label.bar2Max = m_npcs.friendlyEnergyMax();
        label.bar2Color = glm::vec3(1.0f, 0.9f, 0.3f);
        labels.push_back(std::move(label));
    }
    // Shield mirror: bars over remote actors this instance does not simulate — on a client every
    // unit and other player; on the server the client players (self-reported through GqE). The
    // own player is never in the map (its HUD bars cover it).
    for (const auto& [netId, rs] : m_remoteShields)
    {
        const Entity* entity = Globals::networkManager.findEntity(netId);
        if (!entity)
            continue;
        HudWorldLabel label;
        const float height = rs.kind >= 3 ? 2.4f : rs.kind == 2 ? 2.0f : 1.6f;
        if (!camera.worldToScreen(viewport, entity->pos + glm::vec3(0.0f, height, 0.0f), label.screenPos))
            continue;
        label.barValue = rs.healthFrac;
        label.barMax = 1.0f;
        // OWN team reads green, everything else red (camps are team 1 — red for everyone in PvE).
        label.barColor = rs.kind >= 3 || rs.team != (uint8)m_team
            ? glm::vec3(1.0f, 0.25f, 0.2f) : glm::vec3(0.3f, 1.0f, 0.4f);
        if (rs.kind < 3) // camp structures: health bar only (no battery)
        {
            label.bar2Value = rs.energyFrac;
            label.bar2Max = 1.0f;
            label.bar2Color = glm::vec3(1.0f, 0.9f, 0.3f);
        }
        labels.push_back(std::move(label));
    }
    Globals::gameHud.setWorldLabels(std::move(labels));
}

void GameMatch::drawWorldFieldBoundary() const
{
    // The frontier: where the ambient enemy field crosses iso around the BASE — inside the purple
    // ring is safe ground (the ambient itself is never rendered). A second faint ring marks the
    // zero line (the safe radius the emitters are pushing).
    const float iso = Globals::forceSystem.getParams().isoThreshold;
    const float frontier = m_safeRadius + iso / glm::max(m_fieldSlope, 1e-4f);
    drawCircle(glm::vec3(m_basePos.x, 0.5f, m_basePos.z), frontier,
        packColor(glm::vec3(0.8f, 0.3f, 1.0f)), 64);
    drawCircle(glm::vec3(m_basePos.x, 0.5f, m_basePos.z), m_safeRadius,
        packColor(glm::vec3(0.4f, 0.2f, 0.5f)), 64);
}

void GameMatch::updateHud()
{
    GameHud& hud = Globals::gameHud;
    hud.setBar("Health", m_player.health(), m_player.healthMax(), glm::vec3(0.9f, 0.25f, 0.2f));
    hud.setBar("Energy", m_player.energy(), m_player.energyMax(), glm::vec3(0.3f, 0.8f, 1.0f));
    hud.setBar("Materials", m_player.materials(), m_player.materialsMax(), glm::vec3(1.0f, 0.8f, 0.4f)); // carried inventory
    hud.setCounter("Pressure", m_player.pressure(), 2, glm::vec3(0.8f, 0.4f, 1.0f));
    hud.setCounter("Density", m_player.density(), 2, glm::vec3(0.6f, 0.9f, 0.6f));
    hud.setCounter("Shield radius", m_player.shieldRadius(), 2, glm::vec3(0.3f, 0.8f, 1.0f));
    hud.setCounter("Minerals", m_structures.minerals((uint8)m_team), 0, glm::vec3(0.6f, 0.8f, 1.0f));
    hud.setCounter("Fuel", m_structures.fuel((uint8)m_team), 0, glm::vec3(1.0f, 0.6f, 0.2f));
    hud.setBar("Grid energy", m_structures.gridEnergy(), glm::max(m_structures.gridEnergyCapacity(), 1.0f),
        glm::vec3(1.0f, 0.9f, 0.3f));
    hud.setCounter("Energy gen/s", m_structures.energyGenPerSec(), 1, glm::vec3(1.0f, 0.9f, 0.3f));
    hud.setCounter("Energy use/s", m_structures.energyUsePerSec(), 1, glm::vec3(1.0f, 0.9f, 0.3f));
}

void GameMatch::updateWindowed(Camera& camera, float deltaSec)
{
    if (!m_enabled)
        return;

    Input& input = Globals::input;
    const float qeAxis = (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_E) ? 1.0f : 0.0f)
                       - (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_Q) ? 1.0f : 0.0f);
    m_camera.apply(camera, m_player.interpolatedPos(), deltaSec, qeAxis, m_dragDeltaX, m_wheelAccum);
    m_dragDeltaX = 0.0f;
    m_wheelAccum = 0.0f;

    // Mode keys first, then the active mode consumes the inputs. In the tool modes LMB and F both
    // confirm (F is the fallback the UI can never eat); in NEUTRAL mode they are the combat keys —
    // LMB shoots a projectile at the cursor, F swings melee. Requests queue here and are
    // validated/applied in the authority tick.
    updateModeSwitching();
    const bool fDown = input.isKeyDown(SDL_Scancode::SDL_SCANCODE_F)
        && input.isWindowHasFocus() && Globals::ui.isViewportFocused();
    const bool lmbEdge = m_placeClicked;
    const bool fEdge = fDown && !m_placeKeyWasDown;
    m_placeKeyWasDown = fDown;
    m_placeClicked = false;
    const bool confirmEdge = lmbEdge || fEdge;

    switch (m_mode)
    {
    case EPlayerMode::Build:  updateBuildMode(camera, confirmEdge); break;
    case EPlayerMode::Delete: updateDeleteMode(camera, confirmEdge); break;
    case EPlayerMode::Select: updateSelectMode(camera, confirmEdge); break;
    case EPlayerMode::None:
    {
        if (lmbEdge)
        {
            glm::vec3 target;
            if (aimGroundPoint(camera, target))
            {
                const glm::vec3 muzzle = m_player.bodyPos() + glm::vec3(0.0f, 0.5f, 0.0f);
                const glm::vec3 dir = target - muzzle;
                if (glm::dot(dir, dir) > 1e-4f)
                {
                    if (!m_isClient)
                        m_player.queueShot(glm::normalize(dir));
                    else // co-op client: the server spawns the (replicated) projectile at our twin
                    {
                        uint8 buffer[16];
                        NetWriter writer(buffer);
                        const glm::vec3 d = glm::normalize(dir);
                        writer.write<float>(d.x);
                        writer.write<float>(d.y);
                        writer.write<float>(d.z);
                        Globals::networkManager.fireNetworkEvent("GqS", writer.data());
                    }
                }
            }
        }
        if (fEdge)
        {
            // Swing toward the cursor; a failed aim (ray up into the sky) falls back to camera
            // forward inside tickCombat (zero direction queued).
            glm::vec3 target, dir(0.0f);
            if (aimGroundPoint(camera, target))
                dir = target - m_player.bodyPos();
            if (!m_isClient)
                m_player.queueMelee(dir);
            else
            {
                const glm::vec3 d = glm::dot(dir, dir) > 1e-4f
                    ? glm::normalize(glm::vec3(dir.x, 0.0f, dir.z)) : m_camera.forwardPlanar();
                uint8 buffer[16];
                NetWriter writer(buffer);
                writer.write<float>(d.x);
                writer.write<float>(d.y);
                writer.write<float>(d.z);
                Globals::networkManager.fireNetworkEvent("GqM", writer.data());
            }
        }
        break;
    }
    }

    // Melee swing visual: a brief ring along the swing direction while the flash runs.
    if (m_player.meleeFlash() > 0.0f)
        drawCircle(m_player.interpolatedPos() + m_player.meleeDir() * m_player.meleeRange() * 0.6f,
            m_player.meleeRange() * 0.5f, packColor(glm::vec3(1.0f, 1.0f, 0.9f)), 20);

    // Faint ring at the estimated equilibrium shield radius — compare it against the drawn bubble.
    const float shieldR = m_player.shieldRadius();
    if (shieldR > 0.05f)
        drawCircle(m_player.interpolatedPos(), shieldR, packColor(glm::vec3(0.3f, 0.8f, 1.0f)), 32);

    m_structures.drawDebug();
    if (!m_pvp)
        drawWorldFieldBoundary(); // safe-radius/frontier rings mean nothing without the ambient

    // Powered camp cores: the core's bubble is REAL (it pressures shields and deforms bubbles),
    // but a shell surface cannot draw inside the same-team ambient field — the total team-1 field
    // never falls to iso out there, so there is no crossing to rasterize. A ring at the bubble's
    // span marks the active stronghold instead; it dies with the core.
    const uint32 campRingColor = packColor(glm::vec3(1.0f, 0.25f, 0.55f));
    if (!m_isClient)
    {
        for (int i = 0; i < m_npcs.enemyStructureCount(); ++i)
            if (m_npcs.enemyStructureIsCore(i))
            {
                const glm::vec3 pos = m_npcs.enemyStructurePos(i);
                drawCircle(glm::vec3(pos.x, 0.5f, pos.z), 10.0f, campRingColor, 40);
            }
    }
    else
    {
        for (const auto& [netId, rs] : m_remoteShields)
            if (rs.kind == 4)
                if (const Entity* entity = Globals::networkManager.findEntity(netId))
                    drawCircle(glm::vec3(entity->pos.x, 0.5f, entity->pos.z), 10.0f, campRingColor, 40);
    }

    buildWorldLabels(camera);
    updateHud();
}
