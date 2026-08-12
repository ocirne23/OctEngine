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
};
static constexpr BuildItem c_distributionItems[] = {
    { "Transmitter", false, EStructureType::Transmitter, ECableType::Basic },
    { "Battery", false, EStructureType::Battery, ECableType::Basic },
    { "Fuel tank", false, EStructureType::FuelTank, ECableType::Basic },
    { "Cable", true, EStructureType::Emitter, ECableType::Basic },
    { "H-Cable", true, EStructureType::Emitter, ECableType::Heavy },
    { "Pipe", true, EStructureType::Emitter, ECableType::Pipe },
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
    m_npcs.spawnEnemyCamps(m_basePos); // fortified camps ring the deep field
    m_player.spawn(m_playerStart);

    Globals::gameHud.setHotbarVisible(false); // hidden until Build mode (B); setMode populates the
    Globals::gameHud.selectSlot(0);           // grid hotbar (categories -> items) when entered

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
    m_npcs.tickFriendlies(m_structures, deltaSec);
    m_npcs.tickTurrets(m_structures, deltaSec);
    m_npcs.tickEnemyStructures(m_structures, playerPos, deltaSec);
    if (m_player.meleeJustSwung()) // the shove happened in tickCombat; the damage lands here
        m_npcs.applyPlayerMelee(m_player.bodyPos(), m_player.meleeDir(), m_player.meleeRange());
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

// Slot labels/counts for the current grid level: categories at the top, the picked category's
// items (+ "Back" on key 0) inside one. Called every Build-mode frame — counts stay live.
void GameMatch::refreshBuildHotbar()
{
    GameHud& hud = Globals::gameHud;
    const std::span<const BuildItem> items = buildCategoryItems(m_buildCategory);
    for (int i = 0; i < (int)items.size(); ++i)
        hud.setSlot(i, items[i].label, items[i].isCable
            ? m_structures.cableCount(items[i].cable)
            : m_structures.affordableCount(items[i].structure));
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
        // Preview: cable-type hue = will connect (yellow basic / cyan heavy / orange pipe),
        // white = will remove/retype the existing link, red = refused (out of cable range).
        const glm::vec3 typeHue = type == ECableType::Pipe ? glm::vec3(1.0f, 0.55f, 0.15f)
            : type == ECableType::Heavy ? glm::vec3(0.3f, 0.9f, 1.0f) : glm::vec3(0.9f, 0.9f, 0.3f);
        const bool exists = m_structures.cableExists(m_cablePendingId, m_structures.structureId(hover));
        const glm::vec3 previewColor = exists ? glm::vec3(0.9f, 0.9f, 0.9f)
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
            m_structures.queuePlaceRequest(EStructureType::Lance, m_lancePendingPos, -1, facing);
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
            const int affordableSegs = (int)(m_structures.minerals() / glm::max(m_structures.mineralCost(EStructureType::Wall), 0.01f));
            for (int s = 0; s < segments; ++s)
            {
                const glm::vec3 p = m_wallStart + glm::vec3(dir.x, 0.0f, dir.y) * (c_wallSegmentSpacing * s);
                drawCircle(p + glm::vec3(0.0f, 0.3f, 0.0f), 0.9f,
                    packColor(s < affordableSegs ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f)), 12);
            }
            if (confirmEdge)
            {
                for (int s = 0; s < segments; ++s)
                    m_structures.queuePlaceRequest(EStructureType::Wall,
                        m_wallStart + glm::vec3(dir.x, 0.0f, dir.y) * (c_wallSegmentSpacing * s));
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
            m_structures.queuePlaceRequest(aim.type, aim.pos, aim.nodeIndex);
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
        const bool consumer = isEmitterType(type) || type == EStructureType::Extractor
            || type == EStructureType::Fabricator;
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
    // Enemy units: red health bar overhead; friendlies: green.
    for (int i = 0; i < m_npcs.unitCount(); ++i)
    {
        HudWorldLabel label;
        if (!camera.worldToScreen(viewport, m_npcs.unitPos(i) + glm::vec3(0.0f, 1.6f, 0.0f), label.screenPos))
            continue;
        label.barValue = m_npcs.unitHealth(i);
        label.barMax = m_npcs.unitHealthMax(i); // per-type: Brutes have triple bars, Runners half
        label.barColor = glm::vec3(1.0f, 0.25f, 0.2f);
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
