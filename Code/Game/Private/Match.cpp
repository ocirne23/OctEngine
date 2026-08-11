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
import :Match;
import :GameCamera;
import :Player;
import :Structures;
import :Npc;

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

GameMatch::GameMatch(bool enabled) : m_enabled(enabled)
{
    if (!m_enabled)
        return;

    Tweak::floatVar("Game/World", "Safe radius", &m_safeRadius, 2.0f, 500.0f, 1.0f);
    Tweak::floatVar("Game/World", "Field gradient", &m_fieldSlope, 0.001f, 0.5f, 0.001f);
    Tweak::floatVar("Game/World", "Field max strength", &m_fieldMaxStrength, 0.2f, 10.0f, 0.05f);
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
        Globals::world.removeRootEntity(m_ground.get());
}

void GameMatch::spawnWorld()
{
    if (!m_enabled)
        return;

    m_ground = Globals::world.spawnAssetFile("Entities/Game/ground.pre",
        Transform(glm::vec3(0.0f, -0.5f, 0.0f)), true); // box top = walkable y 0
    if (m_ground)
    {
        m_ground->setName("Ground");
        Globals::world.addRootEntity(m_ground);
    }
    m_structures.spawnNodes();
    m_structures.spawnBase(m_basePos); // before the player: the spawn point sits in its bubble
    m_player.spawn(m_playerStart);

    Globals::gameHud.setSlot(0, "Emitter", 0);     // slot keys 1..5; activating the hotbar also
    Globals::gameHud.setSlot(1, "Generator", 0);   // reroutes those keys away from the testbed spawns
    Globals::gameHud.setSlot(2, "Transmitter", 0);
    Globals::gameHud.setSlot(3, "Extractor", 0);
    Globals::gameHud.setSlot(4, "Battery", 0);
    Globals::gameHud.setSlot(5, "Fuel tank", 0);
    Globals::gameHud.setSlot(6, "Cable", 0);       // connect tools, not structures
    Globals::gameHud.setSlot(7, "H-Cable", 0);
    Globals::gameHud.setHotbarVisible(false);      // hidden until Build mode (B) — see setMode
    Globals::gameHud.selectSlot(0);

    Log::info("Game mode: B = build (hotbar 1-7, LMB/F confirms), X = delete, V = select. The enemy "
              "field surrounds you — build powered Emitters on the frontier to expand the safe zone");
}

void GameMatch::update(float deltaSec)
{
    if (!m_enabled)
        return;

    const glm::vec3 playerPos = m_player.bodyPos();
    m_structures.tickAuthority(playerPos, deltaSec);
    m_player.tickMovement(m_camera.forwardPlanar(), deltaSec);
    m_player.tickShieldAndHealth(deltaSec);
    m_player.tickCombat(m_camera.forwardPlanar(), deltaSec);

    // The ambient enemy field: zero within "Safe radius" of the Base, growing with distance.
    // Pushed every tick so all three tweaks stay live.
    Globals::forceSystem.setAmbientField(1u, glm::vec2(m_basePos.x, m_basePos.z), m_safeRadius,
        m_fieldSlope, m_fieldMaxStrength);

    // Waves spawn on the frontier ring (where the ambient crosses iso), all around the safe zone.
    const float iso = Globals::forceSystem.getParams().isoThreshold;
    const float frontierIso = m_safeRadius + iso / glm::max(m_fieldSlope, 1e-4f);
    m_npcs.tickAuthority(m_basePos, frontierIso, playerPos, m_structures, deltaSec);
    if (m_player.meleeJustSwung()) // the shove happened in tickCombat; the damage lands here
        m_npcs.applyPlayerMelee(m_player.bodyPos(), m_player.meleeDir(), m_player.meleeRange());
}

GameMatch::Aim GameMatch::computeAim(const Camera& camera) const
{
    Aim aim;
    const int slot = Globals::gameHud.getSelectedSlot();
    if (slot < 0 || slot >= NumPlaceableStructures) // the Base is never placeable
        return aim;
    aim.type = (EStructureType)slot;

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

    aim.valid = true;
    aim.affordable = m_structures.minerals() >= m_structures.mineralCost(aim.type);
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

void GameMatch::setMode(EPlayerMode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_cablePendingId = 0;
    m_selectedId = 0;
    Globals::gameHud.setHotbarVisible(mode == EPlayerMode::Build); // the hotbar IS the Build indicator
    switch (mode)
    {
    case EPlayerMode::Build:  Log::info("Build mode (B): hotbar 1-8 picks, LMB/F places — B to exit"); break;
    case EPlayerMode::Delete: Log::info("Delete mode (X): click a structure to demolish — X to exit"); break;
    case EPlayerMode::Select: Log::info("Select mode (V): click a structure to inspect — V to exit"); break;
    case EPlayerMode::None:   Log::info("Combat mode: LMB shoot, F melee — B build, X delete, V select"); break;
    }
}

void GameMatch::updateModeSwitching()
{
    Input& input = Globals::input;
    const bool focused = input.isWindowHasFocus() && Globals::ui.isViewportFocused();
    const SDL_Scancode keys[3] = { SDL_Scancode::SDL_SCANCODE_B, SDL_Scancode::SDL_SCANCODE_X, SDL_Scancode::SDL_SCANCODE_V };
    const EPlayerMode modes[3] = { EPlayerMode::Build, EPlayerMode::Delete, EPlayerMode::Select };
    for (int i = 0; i < 3; ++i)
    {
        const bool down = focused && input.isKeyDown(keys[i]);
        if (down && !m_modeKeyWasDown[i])
            setMode(m_mode == modes[i] ? EPlayerMode::None : modes[i]); // same key toggles back out
        m_modeKeyWasDown[i] = down;
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
        // Preview: cable-type hue = will connect (yellow basic / cyan heavy), orange = will
        // remove/retype the existing cable, red = refused (out of cable range).
        const glm::vec3 typeHue = type == ECableType::Heavy
            ? glm::vec3(0.3f, 0.9f, 1.0f) : glm::vec3(0.9f, 0.9f, 0.3f);
        const bool exists = m_structures.cableExists(m_cablePendingId, m_structures.structureId(hover));
        const glm::vec3 previewColor = exists ? glm::vec3(1.0f, 0.6f, 0.2f)
            : m_structures.cableAllowed(pendingIdx, hover) ? typeHue : glm::vec3(1.0f, 0.3f, 0.2f);
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
        m_structures.queueCableRequest(m_cablePendingId, hoverId, type); // validated in the authority tick
        m_cablePendingId = 0;
    }
}

void GameMatch::updateBuildMode(const Camera& camera, bool confirmEdge)
{
    const int slot = Globals::gameHud.getSelectedSlot();
    const int cableType = slot - NumPlaceableStructures; // slots after the placeables = cable tools
    if (cableType >= 0 && cableType < (int)ECableType::Count)
    {
        updateCableTool(camera, confirmEdge, (ECableType)cableType);
        return;
    }
    m_cablePendingId = 0; // leaving the cable tool drops a half-made connection
    const Aim aim = computeAim(camera);
    if (!aim.valid)
        return;
    const uint32 color = packColor(aim.affordable ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f));
    drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), 1.0f, color, 20);
    if (aim.type == EStructureType::Emitter) // show the field footprint a powered pylon would get
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.emitterFieldReach() * 0.5f, color, 32);
    drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.cableRange(),
        packColor(glm::vec3(0.4f, 0.4f, 0.45f)), 40); // cable reach from this spot
    if (confirmEdge && aim.affordable)
        m_structures.queuePlaceRequest(aim.type, aim.pos, aim.nodeIndex);
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
        m_structures.queueDemolishRequest(m_structures.structureId(hover)); // validated in the authority tick
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
        const bool consumer = type == EStructureType::Emitter || type == EStructureType::Extractor;
        if (type != EStructureType::Base) // the Base is invulnerable — no health bar
        {
            label.barValue = m_structures.structureHealth(i);
            label.barMax = m_structures.structureHealthMax();
            const float frac = label.barValue / label.barMax;
            label.barColor = glm::mix(glm::vec3(1.0f, 0.25f, 0.2f), glm::vec3(0.3f, 1.0f, 0.4f), frac);
        }
        const float energyCap = m_structures.structureCapacity(i);
        const float fuelCap = m_structures.structureFuelCapacity(i);
        // Every structure stores energy now; the second bar shows fuel on the fuel holders
        // (Generator/Fuel tank — their buffers live in the selected info) and energy elsewhere.
        const bool fuelBar = type == EStructureType::Generator || type == EStructureType::FuelTank;
        if (fuelBar)
        {
            label.bar2Value = m_structures.structureFuel(i);
            label.bar2Max = fuelCap;
            label.bar2Color = glm::vec3(1.0f, 0.6f, 0.2f);
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
            if (consumer && len > 0 && len < (int)sizeof(info))
                snprintf(info + len, sizeof(info) - len, "\n%s",
                    m_structures.structurePowered(i) ? "Powered" : "No power");
            label.info = info;
        }
        labels.push_back(std::move(label));
    }
    // Enemy units: red health bar overhead.
    for (int i = 0; i < m_npcs.unitCount(); ++i)
    {
        HudWorldLabel label;
        if (!camera.worldToScreen(viewport, m_npcs.unitPos(i) + glm::vec3(0.0f, 1.6f, 0.0f), label.screenPos))
            continue;
        label.barValue = m_npcs.unitHealth(i);
        label.barMax = m_npcs.unitHealthMax();
        label.barColor = glm::vec3(1.0f, 0.25f, 0.2f);
        label.bar2Value = m_npcs.unitEnergy(i); // shield battery (no regen) under the health bar
        label.bar2Max = m_npcs.unitEnergyMax();
        label.bar2Color = glm::vec3(1.0f, 0.9f, 0.3f);
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
    hud.setCounter("Pressure", m_player.pressure(), 2, glm::vec3(0.8f, 0.4f, 1.0f));
    hud.setCounter("Density", m_player.density(), 2, glm::vec3(0.6f, 0.9f, 0.6f));
    hud.setCounter("Shield radius", m_player.shieldRadius(), 2, glm::vec3(0.3f, 0.8f, 1.0f));
    hud.setCounter("Minerals", m_structures.minerals(), 0, glm::vec3(0.6f, 0.8f, 1.0f));
    hud.setCounter("Fuel", m_structures.fuel(), 0, glm::vec3(1.0f, 0.6f, 0.2f));
    hud.setBar("Grid energy", m_structures.gridEnergy(), glm::max(m_structures.gridEnergyCapacity(), 1.0f),
        glm::vec3(1.0f, 0.9f, 0.3f));
    hud.setCounter("Energy gen/s", m_structures.energyGenPerSec(), 1, glm::vec3(1.0f, 0.9f, 0.3f));
    hud.setCounter("Energy use/s", m_structures.energyUsePerSec(), 1, glm::vec3(1.0f, 0.9f, 0.3f));
    hud.setSlotCount(0, m_structures.affordableCount(EStructureType::Emitter));
    hud.setSlotCount(1, m_structures.affordableCount(EStructureType::Generator));
    hud.setSlotCount(2, m_structures.affordableCount(EStructureType::Transmitter));
    hud.setSlotCount(3, m_structures.affordableCount(EStructureType::Extractor));
    hud.setSlotCount(4, m_structures.affordableCount(EStructureType::Battery));
    hud.setSlotCount(5, m_structures.affordableCount(EStructureType::FuelTank));
    hud.setSlotCount(6, m_structures.cableCount(ECableType::Basic)); // cables are free — counts show existing
    hud.setSlotCount(7, m_structures.cableCount(ECableType::Heavy));
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
                    m_player.queueShot(glm::normalize(dir));
            }
        }
        if (fEdge)
        {
            // Swing toward the cursor; a failed aim (ray up into the sky) falls back to camera
            // forward inside tickCombat (zero direction queued).
            glm::vec3 target, dir(0.0f);
            if (aimGroundPoint(camera, target))
                dir = target - m_player.bodyPos();
            m_player.queueMelee(dir);
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
    drawWorldFieldBoundary();
    buildWorldLabels(camera);
    updateHud();
}
