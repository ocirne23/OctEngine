module Game;

import Core;
import Core.glm;
import Core.SDL;
import Core.Log;
import Core.Time; // the hover cards' real-clock cadence
import Core.Camera;
import Core.Rect;
import Core.GameHud;
import Input;
import UI;
import Entity;
import RendererVK;
import Nav;
import Physics; // the sandbox world's terrain aim ray
import :Match;
import :Player;
import :Structures;
import :Npc;

// INTERACTION: the grid hotbar, the modes (Build / Delete / Select), placement (ghosts, the Lance
// two-click, the Wall drag, the cable paint stroke), click-to-select, the unit box selection and
// the RMB orders. Everything here runs in updateWindowed (main thread, windowed instances).

// GRID HOTKEYS (RTS style): the 12 hotbar slots map onto QWER / ASDF / ZXCV, row-major. The ROOT
// page holds the categories (Q Combat, W Production), the three cables on A/S/D and Delete on X;
// a category page holds its items in order with Cancel on C (straight back to Select). The same
// slot index is reached by its key or by clicking the drawn slot.
static constexpr int c_gridSlots = 12;
static constexpr SDL_Scancode c_gridKeys[c_gridSlots] = {
    SDL_Scancode::SDL_SCANCODE_Q, SDL_Scancode::SDL_SCANCODE_W, SDL_Scancode::SDL_SCANCODE_E, SDL_Scancode::SDL_SCANCODE_R,
    SDL_Scancode::SDL_SCANCODE_A, SDL_Scancode::SDL_SCANCODE_S, SDL_Scancode::SDL_SCANCODE_D, SDL_Scancode::SDL_SCANCODE_F,
    SDL_Scancode::SDL_SCANCODE_Z, SDL_Scancode::SDL_SCANCODE_X, SDL_Scancode::SDL_SCANCODE_C, SDL_Scancode::SDL_SCANCODE_V,
};
static_assert(oc::size(c_gridKeyLabels) == c_gridSlots);
// The sandbox world: the same slots on the number row (WASD walk the player there).
static constexpr SDL_Scancode c_gridNumberKeys[c_gridSlots] = {
    SDL_Scancode::SDL_SCANCODE_1, SDL_Scancode::SDL_SCANCODE_2, SDL_Scancode::SDL_SCANCODE_3, SDL_Scancode::SDL_SCANCODE_4,
    SDL_Scancode::SDL_SCANCODE_5, SDL_Scancode::SDL_SCANCODE_6, SDL_Scancode::SDL_SCANCODE_7, SDL_Scancode::SDL_SCANCODE_8,
    SDL_Scancode::SDL_SCANCODE_9, SDL_Scancode::SDL_SCANCODE_0, SDL_Scancode::SDL_SCANCODE_MINUS, SDL_Scancode::SDL_SCANCODE_EQUALS,
};
static_assert(oc::size(c_gridNumberKeyLabels) == c_gridSlots);
// Root page slots: the three category pages (Q/W/E) and Delete.
static constexpr int c_rootDeleteSlot = 9;     // X
static constexpr int c_cancelSlot = 10;        // C: straight back to Select (Esc/Tab step back one level instead)
static constexpr int c_numCategories = 2;
static constexpr const char* c_buildCategories[c_numCategories] = { "DFNC", "WORK" };      // slot captions
static constexpr const char* c_buildCategoryNames[c_numCategories] = { "Defence", "Works" }; // log prose
static constexpr const char* c_buildCategoryCards[c_numCategories] = { // the slots' hover cards
    "Defence\nLantern fields, walls, light towers and the barracks.",
    "Works\nLight, wood and stone: collecting, working and storing." };
// The three cables live on the ROOT page (A/S/D) - no page of their own. Arming one enters Build
// with this HIDDEN category, which draws and behaves as the root page (see isRootPage).
static constexpr int c_cableCategory = 2;
static constexpr int c_rootCableSlot = 4;      // A: c_cableItems[0], S, D follow
// A category page is just its list of placeable types - the shorthand table (c_structureShortNames,
// Match.ixx) IS each slot's caption.
static constexpr EStructureType c_combatItems[] = {
    EStructureType::Emitter,
    EStructureType::Bastion,
    EStructureType::Lance,
    EStructureType::Wall,
    EStructureType::Gate,
    EStructureType::Turret,
    EStructureType::Barracks,
};
static constexpr float c_wallSegmentSpacing = 2.0f; // one segment per box width along the line
static constexpr int c_wallMaxSegments = 16;
static constexpr int c_cableMaxSegments = 32; // one paint-fill placement burst cap
// Everything that is not a weapon: generation, extraction and the distribution buildings.
static constexpr EStructureType c_productionItems[] = {
    EStructureType::Generator,
    EStructureType::Solar,
    EStructureType::Lens,
    EStructureType::Lumberyard,
    EStructureType::Quarry,
    EStructureType::Fabricator,
    EStructureType::Constructor,
    EStructureType::MedicStation,
};
// PHYSICAL cables: one segment type per medium, on the root page's A/S/D. Placement PAINTS
// (press + drag, see updateCablePlacement); connections derive from cell adjacency. The per-medium
// crossings are NOT on the hotbar: the paint stroke places them itself (placeCableLine).
static constexpr EStructureType c_cableItems[] = {
    // LOGISTICS (the sandbox world's third page; the flat modes' root page shows the same items): the ROAD - the
    // carriers walk it with wood and stone - the House (the workers) and the two storage buildings. The power cable
    // and the pipeline are off the hotbar: light moves on beams (Light.cpp).
    EStructureType::CableConveyor,
    EStructureType::House,
    EStructureType::MineralSilo, // the Storehouse: stone AND wood (the Fuel tank is off the hotbar)
    EStructureType::Battery,
    EStructureType::Bridge, // a road piece on a deck: it stands on water (the Road's stroke places it there by itself)
};
static oc::span<const EStructureType> buildCategoryItems(int category)
{
    switch (category)
    {
    case 0: return c_combatItems;
    case 1: return c_productionItems;
    case 2: return c_cableItems;
    default: return {};
    }
}

// ---- the structure ghost (declared in Match.ixx; packColor / drawCircle are Structures.ixx's) --

static void drawStructureGhostExtent(EStructureType type, const glm::vec3& groundPos, uint32 color,
    const glm::ivec2& ext)
{
    const glm::vec2 half(ext.x * StructureSystem::GridCellSize * 0.5f,
                         ext.y * StructureSystem::GridCellSize * 0.5f);
    const float height = StructureSystem::spawnHeightOf(type) * 2.0f;
    const glm::vec3 c = groundPos; // y = the height it will stand on (0 on the flat maps)
    const glm::vec3 corner[4] = {
        c + glm::vec3(-half.x, 0.0f, -half.y), c + glm::vec3(half.x, 0.0f, -half.y),
        c + glm::vec3(half.x, 0.0f, half.y),   c + glm::vec3(-half.x, 0.0f, half.y) };
    const glm::vec3 up(0.0f, height, 0.0f);
    for (int i = 0; i < 4; ++i)
    {
        const glm::vec3& a = corner[i];
        const glm::vec3& b = corner[(i + 1) % 4];
        Globals::rendererVK.addDebugLine(a, b, color);                 // base
        Globals::rendererVK.addDebugLine(a + up, b + up, color);       // top
        Globals::rendererVK.addDebugLine(a, a + up, color);            // riser
    }
    for (int i = 1; i < ext.x; ++i) // interior grid: which cells are taken
    {
        const float o = -half.x + i * StructureSystem::GridCellSize;
        Globals::rendererVK.addDebugLine(c + glm::vec3(o, 0.0f, -half.y), c + glm::vec3(o, 0.0f, half.y), color);
    }
    for (int i = 1; i < ext.y; ++i)
    {
        const float o = -half.y + i * StructureSystem::GridCellSize;
        Globals::rendererVK.addDebugLine(c + glm::vec3(-half.x, 0.0f, o), c + glm::vec3(half.x, 0.0f, o), color);
    }
}

void drawStructureGhost(EStructureType type, const glm::vec3& groundPos, uint32 color)
{
    drawStructureGhostExtent(type, groundPos, color,
        glm::ivec2(StructureSystem::footprintCellsOf(type)));
}

// ---- aiming ----

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

    aim.pos = StructureSystem::snapToGrid(aim.type, aim.pos); // grid-aligned
    aim.type = roadPieceAt(aim.type, aim.pos);                // a road over water: the bridge piece

    aim.valid = true;
    // The sandbox world's ground (flat enough, dry, no tree or rock) - and the height to stand on.
    aim.ground = m_structures.groundFit(aim.type, aim.pos, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), aim.pos.y);
    if (aim.ground == StructureSystem::EGroundFit::NoGround)
    {
        aim.valid = false; // past the streamed collider ring: nothing to show a ghost on
        return aim;
    }
    // Placing a blueprint is free - "affordable" now means the footprint is CLEAR: no structure
    // (or reserved node) on those cells, and nobody standing in them (drives the red ghost AND
    // gates the confirm click; placeStructure re-checks both, the MP seam).
    aim.affordable = aim.ground == StructureSystem::EGroundFit::Ok && m_structures.cellsFree(aim.type, aim.pos)
        && !StructureSystem::actorInFootprint(aim.type, aim.pos);
    if (isHarvestType(aim.type))
    {
        // A Lumberyard / Quarry places with its YIELD, and that is the survey's answer for this very cell (one frame
        // after the ghost came here - updateBuildMode asks, surveyHarvest answers).
        const HarvestProbe& probe = m_harvestProbe;
        const bool known = probe.known && probe.type == aim.type && probe.pos.x == aim.pos.x && probe.pos.z == aim.pos.z;
        aim.yield = known ? probe.yield : -1.0f;
        aim.affordable = aim.affordable && known;
    }
    return aim;
}

EStructureType GameMatch::roadPieceAt(EStructureType armed, const glm::vec3& snapped) const
{
    if (armed != EStructureType::CableConveyor)
        return armed;
    float y = 0.0f;
    return m_structures.groundFit(armed, snapped, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), y) == StructureSystem::EGroundFit::Water
        ? EStructureType::Bridge : armed;
}

bool GameMatch::fitGround(EStructureType type, glm::vec3& p, const glm::quat& rot) const
{
    float y = 0.0f;
    const bool ok = m_structures.groundFit(type, p, rot, y) == StructureSystem::EGroundFit::Ok;
    p.y = y;
    return ok;
}

bool GameMatch::aimGroundPoint(const Camera& camera, glm::vec3& outPos) const
{
    const Ray ray = camera.screenToRay(Globals::ui.getViewportRect(), m_mousePos);
    if (m_sandboxWorld)
    {
        // The terrain collider alone (trees, structures and the player's own capsule never stop the aim).
        constexpr float c_aimReach = 250.0f;
        const PhysicsWorld::RayHit hit = Globals::physics.castRayClosest(ray.origin, ray.dir * c_aimReach,
            PhysicsLayers::bit("Terrain"));
        if (!hit.hit)
            return false;
        outPos = hit.point;
        return true;
    }
    // Analytic ray vs the y=0 ground plane - the world floor IS flat, and a physics raycast kept
    // hitting the tall border-wall colliders (and other bodies) instead of the ground behind them,
    // which made near-wall placement jumpy.
    if (ray.dir.y > -1e-4f)
        return false; // looking at/above the horizon - no ground under the cursor
    outPos = ray.origin + ray.dir * (-ray.origin.y / ray.dir.y);
    return true;
}

int GameMatch::hoveredStructure(const Camera& camera) const
{
    glm::vec3 aimPos;
    if (!aimGroundPoint(camera, aimPos))
        return -1;
    return m_structures.findConnectableNear(aimPos, 4.0f);
}

// ---- the hotbar and the modes ----

// The hotbar page for the current state: ROOT (Select/Delete: categories + Delete on X) or the
// picked category's items (+ Back on V). Called every windowed frame - counts stay live and the
// highlight always mirrors the real state (the engine's number-key routing may poke selectSlot).
bool GameMatch::isRootPage() const
{
    return m_mode != EPlayerMode::Build || m_buildCategory < 0 || m_buildCategory == c_cableCategory;
}

void GameMatch::refreshBuildHotbar()
{
    GameHud& hud = Globals::gameHud;
    // HOVER CARDS: the full type name + StructureSystem's description (one sentence, then the
    // exact per-second flows). Formatted from the LIVE tweaks, so they are rebuilt on a slow
    // cadence instead of per frame - this runs every frame for the counts, and 26 formatted
    // strings a frame is pure waste for text that only moves when someone drags a tweak. The
    // cadence is REAL seconds, not a frame count: the rate must not follow the frame rate, and a
    // paused game still updates its cards. The setters below then early-out on an unchanged
    // string, so a steady frame allocates nothing.
    const double now = Globals::time.getElapsedSec(); // real clock: the cards refresh while paused too
    if (now - m_typeCardTime >= 1.0)
    {
        m_typeCardTime = now;
        for (int t = 0; t < (int)EStructureType::Count; ++t)
            m_typeCards[t] = oc::string(structureTypeName((EStructureType)t)) + "\n"
                + m_structures.describeType((EStructureType)t);
    }
    if (m_sandboxWorld)
    {
        refreshBuildBar();
        return;
    }
    uint32 used = 0; // slots this page filled; everything else is cleared at the end
    const auto itemSlot = [&](int slot, EStructureType type)
    {
        hud.setSlot(slot, c_structureShortNames[(int)type],
            m_structures.affordableCount(type, (uint8)m_team));
        hud.setSlotTooltip(slot, m_typeCards[(int)type]);
        used |= 1u << slot;
    };
    const auto plainSlot = [&](int slot, const char* label, const char* card)
    {
        hud.setSlot(slot, label, 0);
        hud.setSlotTooltip(slot, card);
        used |= 1u << slot;
    };
    if (isRootPage())
    {
        for (int i = 0; i < c_numCategories; ++i)
            plainSlot(i, c_buildCategories[i], c_buildCategoryCards[i]);
        for (int i = 0; i < (int)oc::size(c_cableItems); ++i)
            itemSlot(c_rootCableSlot + i, c_cableItems[i]);
        const bool cableArmed = m_mode == EPlayerMode::Build && m_buildSelection >= 0;
        hud.selectSlot(m_mode == EPlayerMode::Delete ? c_rootDeleteSlot
                     : cableArmed ? c_rootCableSlot + m_buildSelection : -1);
    }
    else
    {
        const oc::span<const EStructureType> items = buildCategoryItems(m_buildCategory);
        for (int i = 0; i < (int)items.size() && i < c_rootDeleteSlot; ++i)
            itemSlot(i, items[i]);
        hud.selectSlot(m_buildSelection);
    }
    plainSlot(c_rootDeleteSlot, "DEL", "Delete\nClick a structure to demolish it."); // X on EVERY page
    plainSlot(c_cancelSlot, "CNCL", "Cancel\nBack to Select mode.");                 // C on EVERY page
    for (int i = 0; i < GameHud::NumSlots; ++i)
        if (!(used & (1u << i)))
            hud.clearSlot(i);
}

void GameMatch::setMode(EPlayerMode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_selectedId = 0;
    m_lanceAiming = false;
    m_wallPlacing = false;
    m_cablePainting = false;
    m_buildSelection = -1;
    if (mode != EPlayerMode::Build)
        m_buildCategory = -1; // back to the root page
    refreshBuildHotbar();
    switch (mode)
    {
    case EPlayerMode::Build:  break; // the category entry logs its own line (activateSlot)
    case EPlayerMode::Delete: Log::info("Delete mode (X): click a structure to demolish (one, then back to Select) - X cancels"); break;
    case EPlayerMode::Select: Log::info("Select mode: click inspects, RMB routes / moves - Q/W build, A/S/D cables, X delete"); break;
    }
}

// One level back: a half-finished two-click step drops first, then the armed item disarms, then
// the category page (or Delete mode) returns to Select. Esc/Tab (the C "Cancel" slot goes straight
// back to Select).
void GameMatch::cancelOneLevel()
{
    if (m_mode == EPlayerMode::Build && m_buildSelection >= 0)
    {
        if (m_lanceAiming || m_wallPlacing || m_cablePainting)
        {
            m_lanceAiming = false;
            m_wallPlacing = false;
            m_cablePainting = false;
        }
        else
            disarmBuild();
    }
    else
        setMode(EPlayerMode::Select);
}

// ONE entry point for a hotbar slot, whether its key was pressed or the drawn slot was clicked.
void GameMatch::activateSlot(int slot)
{
    if (slot < 0 || slot >= c_gridSlots)
        return;
    if (m_sandboxWorld) // the build bar: a page tab shows its page (nothing armed), an item slot arms its item
    {
        m_buildUi = true;
        if (slot < c_buildBarTabs)
        {
            setMode(EPlayerMode::Build);
            m_buildCategory = slot;
            m_buildSelection = -1;
            m_lanceAiming = false;
            m_wallPlacing = false;
            m_cablePainting = false;
            m_cablePendingValid = false;
            m_lastPage = slot;
            refreshBuildHotbar();
        }
        else if (slot - c_buildBarTabs < (int)buildCategoryItems(buildBarPage()).size())
            armPageItem(buildBarPage(), slot - c_buildBarTabs);
        return;
    }
    if (isRootPage())
    {
        // ROOT page
        if (slot < c_numCategories)
        {
            setMode(EPlayerMode::Build);
            m_buildCategory = slot;
            m_buildSelection = -1;
            refreshBuildHotbar();
            Log::info(oc::string("Build: ") + c_buildCategoryNames[slot]
                + " - grid keys arm an item, LMB places, RMB cancels, C/Esc back");
        }
        else if (slot >= c_rootCableSlot && slot < c_rootCableSlot + (int)oc::size(c_cableItems))
        {
            // A cable arms straight from the root page (the hidden cable category).
            setMode(EPlayerMode::Build);
            m_buildCategory = c_cableCategory;
            m_lanceAiming = false;
            m_wallPlacing = false;
            m_cablePainting = false;
            m_cablePendingValid = false;
            m_buildSelection = slot - c_rootCableSlot;
            refreshBuildHotbar();
        }
        else if (slot == c_rootDeleteSlot)
            setMode(m_mode == EPlayerMode::Delete ? EPlayerMode::Select : EPlayerMode::Delete);
        else if (slot == c_cancelSlot)
            setMode(EPlayerMode::Select); // cancels Delete mode / an armed cable; a no-op in Select
        return;
    }
    // CATEGORY page
    if (slot == c_cancelSlot)
    {
        setMode(EPlayerMode::Select); // straight back, whatever was armed
        return;
    }
    if (slot == c_rootDeleteSlot)
    {
        setMode(EPlayerMode::Delete); // X works on every page, not just the root
        return;
    }
    if (slot >= (int)buildCategoryItems(m_buildCategory).size() || slot >= c_rootDeleteSlot)
        return; // empty slot
    m_lanceAiming = false; // switching items drops half-done aims/flows
    m_wallPlacing = false;
    m_cablePainting = false;
    m_cablePendingValid = false;
    m_buildSelection = slot;
    refreshBuildHotbar();
}

void GameMatch::armPageItem(int category, int selection)
{
    const int count = (int)buildCategoryItems(category).size();
    if (count <= 0)
        return;
    setMode(EPlayerMode::Build);
    m_buildCategory = category;
    m_lanceAiming = false; // switching items drops half-done aims/flows
    m_wallPlacing = false;
    m_cablePainting = false;
    m_cablePendingValid = false;
    m_buildSelection = glm::clamp(selection, 0, count - 1);
    m_pageSelection[category] = m_buildSelection;
    m_lastPage = category;
    refreshBuildHotbar();
}

void GameMatch::toggleBuildUi()
{
    if (m_buildUi) // open already: Tab is the NEXT PAGE (it wraps), armed on that page's last item
    {
        const int next = (buildBarPage() + 1) % c_buildBarTabs;
        armPageItem(next, m_pageSelection[next]);
        return;
    }
    m_buildUi = true;
    armPageItem(m_lastPage, m_pageSelection[m_lastPage]); // open on what was armed last: Tab + a click builds again
}

int GameMatch::buildBarPage() const
{
    return m_mode == EPlayerMode::Build && m_buildCategory >= 0 ? m_buildCategory : m_lastPage;
}

// The sandbox world's hotbar: nothing in PLAY, the build bar in the BUILD UI - the three page tabs, then the items
// of the shown page. The cursor is free there, so every slot is a button with its hover card.
void GameMatch::refreshBuildBar()
{
    GameHud& hud = Globals::gameHud;
    static constexpr const char* c_pageNames[c_buildBarTabs] = { "Defence", "Works", "Logistics" };
    static constexpr const char* c_tabLabels[c_buildBarTabs] = { "DEFENCE", "WORKS", "LOGISTICS" };
    static constexpr const char* c_tabOpenLabels[c_buildBarTabs] = { "[DEFENCE]", "[WORKS]", "[LOGISTICS]" };
    static constexpr const char* c_tabCards[c_buildBarTabs] = {
        "Defence\nLantern fields, walls, light towers and the barracks.",
        "Works\nMaking light, wood and stone, and the builder's and the healer's hut.",
        "Logistics\nRoads for the carriers (hold and drag to paint), houses for the workers, the storehouse for goods and the crystal store for light." };
    if (!m_buildUi)
    {
        for (int i = 0; i < GameHud::NumSlots; ++i)
            hud.clearSlot(i); // no assigned slot = no bar
        hud.setCounterText("Build", "Tab: build   X: delete", glm::vec3(0.7f, 0.7f, 0.7f));
        return;
    }
    const int page = buildBarPage();
    const bool building = m_mode == EPlayerMode::Build;
    for (int tab = 0; tab < c_buildBarTabs; ++tab)
    {
        hud.setSlot(tab, building && tab == page ? c_tabOpenLabels[tab] : c_tabLabels[tab], 0);
        hud.setSlotTooltip(tab, c_tabCards[tab]);
    }
    const oc::span<const EStructureType> items = buildCategoryItems(page);
    for (int i = 0; i < GameHud::NumSlots - c_buildBarTabs; ++i)
    {
        const int slot = c_buildBarTabs + i;
        if (i >= (int)items.size())
        {
            hud.clearSlot(slot);
            continue;
        }
        hud.setSlot(slot, c_structureShortNames[(int)items[i]], m_structures.affordableCount(items[i], (uint8)m_team));
        hud.setSlotTooltip(slot, m_typeCards[(int)items[i]]);
    }
    const bool armed = building && m_buildSelection >= 0 && m_buildSelection < (int)items.size();
    hud.selectSlot(armed ? c_buildBarTabs + m_buildSelection : -1);
    char text[128];
    if (m_mode == EPlayerMode::Delete)
        snprintf(text, sizeof(text), "Delete: click a structure   X: back   RMB: close");
    else if (armed)
        snprintf(text, sizeof(text), "%s: %s (%.0f stone)   LMB place, Q/E item, Tab page, RMB close, MMB look",
            c_pageNames[page], structureTypeName(items[m_buildSelection]), m_structures.mineralCost(items[m_buildSelection]));
    else
        snprintf(text, sizeof(text), "%s: Q/E or click an item   Tab page, X delete, RMB close", c_pageNames[page]);
    hud.setCounterText("Build", text, m_mode == EPlayerMode::Delete ? glm::vec3(1.0f, 0.4f, 0.3f) : glm::vec3(1.0f, 0.85f, 0.45f));
}

void GameMatch::stepSelection(int direction)
{
    m_buildUi = true;
    if (m_mode != EPlayerMode::Build || m_buildCategory < 0)
    {
        armPageItem(m_lastPage, m_pageSelection[m_lastPage]); // from Select: back to what was armed last
        return;
    }
    // ONE list over all pages: past a page's last item comes the next page's first (and the other way), wrapping.
    int page = m_buildCategory;
    int selection = m_buildSelection < 0 ? (direction > 0 ? 0 : (int)buildCategoryItems(page).size() - 1) : m_buildSelection + direction;
    if (selection >= (int)buildCategoryItems(page).size())
    {
        page = (page + 1) % c_buildBarTabs;
        selection = 0;
    }
    else if (selection < 0)
    {
        page = (page + c_buildBarTabs - 1) % c_buildBarTabs;
        selection = (int)buildCategoryItems(page).size() - 1;
    }
    armPageItem(page, selection);
}

void GameMatch::updateModeSwitching()
{
    Input& input = Globals::input;
    const bool focused = input.isWindowHasFocus() && Globals::ui.isViewportFocused();
    if (m_sandboxWorld)
    {
        static constexpr SDL_Scancode c_buildKeys[4] = { SDL_Scancode::SDL_SCANCODE_TAB, SDL_Scancode::SDL_SCANCODE_Q,
            SDL_Scancode::SDL_SCANCODE_E, SDL_Scancode::SDL_SCANCODE_X };
        for (int k = 0; k < 4; ++k)
        {
            const bool down = focused && input.isKeyDown(c_buildKeys[k]) && (SDL_GetModState() & SDL_KMOD_CTRL) == 0;
            if (down && !m_buildKeyWasDown[k])
                switch (k)
                {
                case 0: toggleBuildUi(); break;
                case 1: stepSelection(-1); break;
                case 2: stepSelection(1); break;
                default: // X: Delete mode, and back to the page from it
                    if (m_mode == EPlayerMode::Delete)
                    {
                        setMode(EPlayerMode::Build);
                        m_buildCategory = m_lastPage;
                    }
                    else
                    {
                        m_buildUi = true;
                        setMode(EPlayerMode::Delete);
                    }
                    refreshBuildHotbar();
                    break;
                }
            m_buildKeyWasDown[k] = down;
        }
    }
    // Grid hotkeys: polled edges on the 12 keys, each mapping straight onto its slot.
    const SDL_Scancode* gridKeys = m_sandboxWorld ? c_gridNumberKeys : c_gridKeys;
    for (int k = 0; k < c_gridSlots; ++k)
    {
        const bool down = focused && input.isKeyDown(gridKeys[k]) && (SDL_GetModState() & SDL_KMOD_CTRL) == 0;
        if (down && !m_gridKeyWasDown[k])
        {
            if (!m_sandboxWorld)
                activateSlot(k);
            else if (k < GameHud::NumSlots - c_buildBarTabs) // the build bar: 1..9 are its item slots
                activateSlot(c_buildBarTabs + k);
        }
        m_gridKeyWasDown[k] = down;
    }
    // Escape/Tab: one level back.
    const bool backDown = focused && (input.isKeyDown(SDL_Scancode::SDL_SCANCODE_ESCAPE)
        || (!m_sandboxWorld && input.isKeyDown(SDL_Scancode::SDL_SCANCODE_TAB))); // the sandbox world: Tab = the pages
    if (backDown && !m_modeKeyWasDown[0])
        cancelOneLevel();
    m_modeKeyWasDown[0] = backDown;
    // The build UI closes when the mode is back at Select (Esc at the end of its chain, Tab, a finished demolish).
    if (m_buildUi && m_mode == EPlayerMode::Select)
    {
        m_buildUi = false;
        refreshBuildHotbar();
    }
    // F9 save / F10 load (authority only - a client has no sim to save).
    const bool saveDown = focused && input.isKeyDown(SDL_Scancode::SDL_SCANCODE_F9);
    if (saveDown && !m_saveKeyWasDown)
        saveGame();
    m_saveKeyWasDown = saveDown;
    const bool loadDown = focused && input.isKeyDown(SDL_Scancode::SDL_SCANCODE_F10);
    if (loadDown && !m_loadKeyWasDown)
        loadGame();
    m_loadKeyWasDown = loadDown;
}

void GameMatch::showGroundReason(StructureSystem::EGroundFit fit)
{
    if (fit == m_shownGroundFit)
        return; // the HUD only hears the edges
    m_shownGroundFit = fit;
    if (fit == StructureSystem::EGroundFit::Ok)
        Globals::gameHud.removeCounter("Can't build");
    else
        Globals::gameHud.setCounterText("Can't build", StructureSystem::groundFitReason(fit), glm::vec3(1.0f, 0.4f, 0.3f));
}

void GameMatch::showYield(int percent)
{
    if (percent == m_shownYield)
        return; // the HUD only hears the changes
    m_shownYield = percent;
    if (percent == -2)
        Globals::gameHud.removeCounter("Yield");
    else if (percent < 0)
        Globals::gameHud.setCounterText("Yield", "surveying...", glm::vec3(0.7f, 0.7f, 0.7f));
    else
    {
        char text[16];
        snprintf(text, sizeof(text), "%d %%", percent);
        Globals::gameHud.setCounterText("Yield", text, glm::mix(glm::vec3(1.0f, 0.4f, 0.3f), glm::vec3(0.4f, 1.0f, 0.5f), (float)percent * 0.01f));
    }
}

void GameMatch::disarmBuild()
{
    m_buildSelection = -1;
    m_lanceAiming = false;
    m_wallPlacing = false;
    m_cablePainting = false;
    m_cablePendingValid = false;
    refreshBuildHotbar(); // the slot highlight follows in the same frame
}

// ---- placement ----

// The Crossing's axis rotation for a ±X/±Z facing - the SAME formula placeStructure applies to
// the request's facing, so a client-side cellsFree probes exactly the cells it will cover.
static glm::quat crossingRotation(const glm::vec2& dir)
{
    return glm::angleAxis(std::atan2(-dir.x, -dir.y), glm::vec3(0.0f, 1.0f, 0.0f));
}

// Fill the auto-bent L between two snapped 1-cell positions - the dominant leg first, then the
// perpendicular one - requesting a placement per FREE cell (occupied cells are skipped, so a line
// across an existing run just fills the gaps). The stroke HOLDS its newest cell back until the
// cell after it is known (m_cablePending): when that next cell holds a plain cable of ANOTHER
// medium, the stroke's OWN medium's Crossing goes over it with its long axis along the stroke -
// the held cell and the cell beyond are its two END cells and are never painted - so a stroke
// across a foreign run bridges it instead of leaving a gap. A crossing the cells refuse falls
// back to the plain skip.
void GameMatch::placeCableLine(EStructureType armed, const glm::vec3& from, const glm::vec3& to)
{
    constexpr float step = StructureSystem::GridCellSize;
    glm::vec3 points[c_cableMaxSegments];
    int count = 0;
    glm::vec3 p = from;
    const auto push = [&] { if (count < c_cableMaxSegments) points[count++] = p; };
    push();
    const glm::vec2 d(to.x - from.x, to.z - from.z);
    const bool xFirst = glm::abs(d.x) >= glm::abs(d.y);
    for (int leg = 0; leg < 2; ++leg)
    {
        const bool alongX = xFirst == (leg == 0);
        const float target = alongX ? to.x : to.z;
        float& axis = alongX ? p.x : p.z;
        while (glm::abs(target - axis) > step * 0.5f && count < c_cableMaxSegments)
        {
            axis += target > axis ? step : -step;
            push();
        }
    }
    const auto sameCell = [](const glm::vec3& a, const glm::vec3& b) {
        return glm::abs(a.x - b.x) < step * 0.5f && glm::abs(a.z - b.z) < step * 0.5f; };
    // points[0] is `from`: the held-back cell when one is pending, else already dealt with.
    for (int i = m_cablePendingValid ? 0 : 1; i < count; ++i)
    {
        if (i == count - 1)
        {
            m_cablePending = points[i]; // held until the next sample or the release
            m_cablePendingValid = true;
            m_cablePaintLast = points[i];
            return;
        }
        const glm::vec3& next = points[i + 1];
        const int foreign = m_structures.bridgeableAt(next);
        if (foreign >= 0 && conduitMediumOf(m_structures.structureType(foreign)) != cableMediumOf(armed))
        {
            const glm::vec2 dir(glm::sign(next.x - points[i].x), glm::sign(next.z - points[i].z));
            const EStructureType crossing = crossingForMedium(cableMediumOf(armed));
            // planCrossing, not cellsFree: an END cell holding our OWN medium is replaced, so a
            // stroke crosses a foreign line even where its own run already stands.
            glm::vec3 at = next;
            if (m_structures.planCrossing(crossing, next, crossingRotation(dir), (uint8)m_team).valid
                && !StructureSystem::actorInFootprint(crossing, next) && fitGround(crossing, at, crossingRotation(dir)))
            {
                requestPlace(crossing, at, -1, glm::vec3(dir.x, 0.0f, dir.y));
                // points[i] and the cell beyond `next` are the crossing's END cells: never cables.
                const glm::vec3 farEnd = next + glm::vec3(dir.x, 0.0f, dir.y) * step;
                i += i + 2 < count && sameCell(points[i + 2], farEnd) ? 2 : 1;
                continue;
            }
        }
        glm::vec3 at = points[i];
        const EStructureType piece = roadPieceAt(armed, at);
        if (m_structures.cellsFree(piece, at) && fitGround(piece, at))
            requestPlace(piece, at, 1.0f, glm::vec3(0.0f));
    }
    m_cablePendingValid = false;
    m_cablePaintLast = points[count - 1];
}

void GameMatch::finishCableStroke(EStructureType armed)
{
    glm::vec3 at = m_cablePending;
    const EStructureType piece = roadPieceAt(armed, at);
    if (m_cablePendingValid && m_structures.cellsFree(piece, at) && fitGround(piece, at))
        requestPlace(piece, at, 1.0f, glm::vec3(0.0f));
    m_cablePendingValid = false;
    m_cablePainting = false;
}

// Cable segments place by PAINTING (see Match.ixx): a press starts the stroke at its cell and,
// while held, the stroke places the cells the cursor crosses (L-filled between samples so the run
// never breaks). Release ends the stroke; a plain click is a one-cell stroke.
void GameMatch::updateCablePlacement(const Camera& camera, EStructureType armed, bool confirmEdge)
{
    const Aim aim = computeAim(camera, armed);
    if (m_cablePainting)
    {
        if (!m_lmbDown)
            finishCableStroke(armed); // release ends the stroke (and lands the held cell)
        else if (aim.valid && glm::distance(glm::vec2(aim.pos.x, aim.pos.z),
            glm::vec2(m_cablePaintLast.x, m_cablePaintLast.z)) > 0.1f)
            placeCableLine(armed, m_cablePaintLast, aim.pos);
        if (aim.valid)
            drawStructureGhost(aim.type, aim.pos, packColor(glm::vec3(0.3f, 1.0f, 0.4f)));
        return;
    }
    if (!aim.valid)
    {
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
        return;
    }
    m_frameGroundFit = aim.ground;
    const uint32 color = packColor(aim.affordable ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f));
    drawStructureGhost(aim.type, aim.pos, color);
    if (confirmEdge && aim.affordable)
    {
        m_cablePainting = true; // hold + drag paints from here
        m_cablePending = aim.pos; // the press's cell is held back one step (see placeCableLine)
        m_cablePendingValid = true;
        m_cablePaintLast = aim.pos;
    }
    updateSelectionClick(camera, confirmEdge, /*allowPick*/ !aim.affordable);
}

void GameMatch::updateBuildMode(const Camera& camera, bool confirmEdge, bool cancelEdge)
{
    // (Grid keys / slot clicks arm items through activateSlot - see updateModeSwitching and the
    // hotbar click in updateWindowed.)
    if (m_buildCategory < 0 || m_buildSelection < 0)
    {
        // Nothing armed - browsing the category: clicks inspect, LMB drag box-selects units and
        // RMB sets barracks routes / orders, exactly as Select mode. Only an ARMED item takes the
        // clicks away.
        updateRightClickActions(camera, cancelEdge);
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
        updateUnitSelection(camera);
        return;
    }
    // RMB CANCELS, one step at a time: a half-finished flow (Lance aim, Wall line, cable paint
    // stroke, Crossing aim) drops first, and the next RMB disarms the item itself. Only once nothing
    // is armed does RMB go back to its Select-mode meaning (barracks route / move order) above.
    const EStructureType armed = buildCategoryItems(m_buildCategory)[m_buildSelection];

    // Cable segments have their own paint input (press + drag).
    if (isCableType(armed))
    {
        if (cancelEdge)
        {
            if (m_cablePainting)
                finishCableStroke(armed); // the held cell was already shown painted - it lands
            else
                disarmBuild();
            return; // NOT consumed: the same press also walks the player
        }
        updateCablePlacement(camera, armed, confirmEdge);
        return;
    }

    // (Crossings are never armed: the paint stroke places them - placeCableLine.)

    // Lance second click: the position is anchored - the cursor now aims the cone's facing
    // (relative to the anchor); confirm places, too-close clicks just keep waiting. RIGHT-click
    // cancels the anchor before the confirm.
    if (m_lanceAiming)
    {
        if (cancelEdge)
        {
            m_lanceAiming = false;
            return; // NOT consumed: the same press also walks the player (see the RMB chain)
        }
        const glm::vec3 up(0.0f, 0.3f, 0.0f);
        const uint32 color = packColor(glm::vec3(0.3f, 1.0f, 0.4f));
        drawStructureGhost(EStructureType::Lance, m_lancePendingPos, color);
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

    // LINE DRAG (the Wall, the Gate - isLineType): the press anchored the line start; while held,
    // segments preview along the line to the cursor, and the RELEASE queues one placement per segment.
    // RIGHT-click cancels the stroke before the release.
    if (m_wallPlacing)
    {
        if (cancelEdge)
        {
            m_wallPlacing = false;
            return; // NOT consumed: the same press also walks the player
        }
        const bool release = !m_lmbDown;
        const Aim end = computeAim(camera, armed);
        if (end.valid)
        {
            const glm::vec2 span(end.pos.x - m_wallStart.x, end.pos.z - m_wallStart.z);
            const float len = glm::length(span);
            const int segments = glm::clamp((int)(len / c_wallSegmentSpacing) + 1, 1, c_wallMaxSegments);
            const glm::vec2 dir = len > 1e-3f ? span / len : glm::vec2(0.0f);
            // Snap each sample to the SAME grid placeStructure uses - the preview circles must sit
            // exactly where the segments will land. Diagonal lines can snap two samples into the
            // same cell; dedup so it draws (and places) once.
            glm::vec3 points[c_wallMaxSegments];
            int count = 0;
            for (int s = 0; s < segments; ++s)
            {
                const glm::vec3 snapped = StructureSystem::snapToGrid(armed,
                    m_wallStart + glm::vec3(dir.x, 0.0f, dir.y) * (c_wallSegmentSpacing * s));
                if (count > 0 && glm::distance(glm::vec2(points[count - 1].x, points[count - 1].z),
                    glm::vec2(snapped.x, snapped.z)) < 0.1f)
                    continue;
                points[count++] = snapped;
            }
            for (int s = 0; s < count; ++s)
            {
                const bool fits = fitGround(armed, points[s]); // + its stand height (the water surface for a Gate)
                const bool free = fits && m_structures.cellsFree(armed, points[s])
                    && !StructureSystem::actorInFootprint(armed, points[s]);
                drawStructureGhost(armed, points[s],
                    packColor(free ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f)));
            }
            if (release)
                for (int s = 0; s < count; ++s)
                    requestPlace(armed, points[s], 1.0f, glm::vec3(0.0f));
        }
        if (release)
            m_wallPlacing = false; // a release off the ground (no valid end) just drops the stroke
        return;
    }

    if (cancelEdge) // nothing half-placed (the two-click flows returned above): drop the ghost
    {
        disarmBuild();
        return; // NOT consumed: cancelling and moving are one press (see the RMB chain)
    }

    const Aim aim = computeAim(camera, armed);
    if (!aim.valid)
    {
        // No ghost here (off-map). Clicks still inspect.
        updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
        return;
    }
    m_frameGroundFit = aim.ground; // the HUD names a refused ground
    const uint32 color = packColor(aim.affordable ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.3f, 0.2f));
    drawStructureGhost(aim.type, aim.pos, color); // the exact box that will be built
                                                  // (crossings are auto-placed by the paint stroke)
    if (isEmitterType(aim.type)) // show the field footprint the powered variant would get
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.emitterReachOf(aim.type) * 0.5f, color, 32);
    if (aim.type == EStructureType::Constructor) // show the build/repair reach it would cover
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.constructorRange(), color, 40);
    if (m_structures.lightBeams())
    {
        // Light on beams: a hub shows the reach it would feed, a Mirror how far its beam reaches a hub.
        if (aim.type == EStructureType::Battery)
            drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.lightHubReach(), packColor(glm::vec3(1.0f, 0.85f, 0.4f)), 48);
        if (aim.type == EStructureType::Solar)
            drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.mirrorBeamRange(), packColor(glm::vec3(1.0f, 0.85f, 0.4f)), 64);
        if (aim.type == EStructureType::Lens) // how far it reaches the next lens / a store, and feeds what uses light
            drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.lensRange(), packColor(glm::vec3(1.0f, 0.85f, 0.4f)), 64);
    }
    if (aim.type == EStructureType::House) // show how far it links to a barracks
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.houseLinkRadius(), color, 48);
    if (aim.type == EStructureType::MedicStation) // show the heal reach
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.medicHealRadius(), color, 48);
    if (isHarvestType(aim.type))
    {
        // The range it works, hued by the yield it would have here (red = little, green = full; grey = no answer
        // yet). The probe for this cell: surveyHarvest answers it later this frame.
        HarvestProbe& probe = m_harvestProbe;
        if (probe.type != aim.type || probe.pos.x != aim.pos.x || probe.pos.z != aim.pos.z)
        {
            probe.type = aim.type;
            probe.pos = aim.pos;
            probe.known = false;
        }
        probe.wanted = true;
        m_frameYield = aim.yield < 0.0f ? -1 : (int)glm::round(aim.yield * 100.0f);
        const glm::vec3 hue = aim.yield < 0.0f ? glm::vec3(0.6f) : glm::mix(glm::vec3(1.0f, 0.4f, 0.3f), glm::vec3(0.4f, 1.0f, 0.5f), aim.yield);
        drawCircle(aim.pos + glm::vec3(0.0f, 0.3f, 0.0f), m_structures.harvestRadius(), packColor(hue), 64);
    }
    if (confirmEdge && aim.affordable)
    {
        if (aim.type == EStructureType::Lance)
        {
            m_lanceAiming = true; // first click anchors; the next click aims the cone
            m_lancePendingPos = aim.pos;
        }
        else if (isLineType(aim.type))
        {
            m_wallPlacing = true; // the press anchors the line start; the release ends it
            m_wallStart = aim.pos;
        }
        else
            requestPlace(aim.type, aim.pos, aim.yield, glm::vec3(0.0f));
    }
    // A click the placement REFUSES (occupied cells - i.e. on a building) inspects it instead of
    // doing nothing; a click that can place always places.
    updateSelectionClick(camera, confirmEdge, /*allowPick*/ !aim.affordable);
}

void GameMatch::updateDeleteMode(const Camera& camera, bool confirmEdge)
{
    const int hover = hoveredStructure(camera);
    if (hover < 0)
        return;
    const bool deletable = m_structures.structureType(hover) != EStructureType::Base;
    drawCircle(m_structures.structureGroundPos(hover) + glm::vec3(0.0f, 0.3f, 0.0f), 1.6f,
        packColor(deletable ? glm::vec3(1.0f, 0.25f, 0.2f) : glm::vec3(0.5f, 0.5f, 0.5f)), 20);
    if (confirmEdge && deletable)
    {
        requestDemolish(m_structures.structureId(hover)); // validated in the authority tick (server)
        setMode(EPlayerMode::Select); // one demolish per arm: the Delete button releases itself
    }
}

void GameMatch::updateSelectMode(const Camera& camera, bool confirmEdge, bool rmbEdge)
{
    updateRightClickActions(camera, rmbEdge);
    updateSelectionClick(camera, confirmEdge, /*allowPick*/ true);
    updateUnitSelection(camera);
}

// ---- selection and orders ----

// BOX SELECTION of own-team units (Select mode): LMB drag draws the box on the ground; on
// release every visible own unit whose screen position falls inside it is selected (SHIFT adds).
// A plain click (no drag) clears the unit selection. Selected units carry a ring; RMB orders send
// them along with the player (see the move-order block in updateWindowed).
void GameMatch::updateUnitSelection(const Camera& camera)
{
    static constexpr float c_dragPx = 8.0f;
    const Rect viewport = Globals::ui.getViewportRect();
    const auto groundOf = [&](const glm::vec2& screen, glm::vec3& out)
    {
        const Ray ray = camera.screenToRay(viewport, screen);
        if (ray.dir.y > -1e-4f)
            return false;
        out = ray.origin + ray.dir * (-ray.origin.y / ray.dir.y);
        return true;
    };
    if (m_lmbDown && glm::distance(m_lmbDownPos, m_mousePos) > c_dragPx)
    {
        // Live box: the four screen corners projected onto the ground.
        const glm::vec2 a = m_lmbDownPos, b = m_mousePos;
        const glm::vec2 corners[4] = { a, glm::vec2(b.x, a.y), b, glm::vec2(a.x, b.y) };
        glm::vec3 g[4];
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i)
            ok = groundOf(corners[i], g[i]);
        if (ok)
        {
            const uint32 col = packColor(glm::vec3(0.4f, 1.0f, 0.5f));
            for (int i = 0; i < 4; ++i)
                Globals::rendererVK.addDebugLine(g[i] + glm::vec3(0.0f, 0.2f, 0.0f), g[(i + 1) % 4] + glm::vec3(0.0f, 0.2f, 0.0f), col);
        }
    }
    if (m_lmbReleased)
    {
        m_lmbReleased = false;
        const bool drag = glm::distance(m_lmbDownPos, m_mousePos) > c_dragPx;
        if (!Globals::input.isKeyDown(SDL_Scancode::SDL_SCANCODE_LSHIFT))
            m_selectedUnits.clear();
        if (drag && !m_isClient)
        {
            const glm::vec2 lo = glm::min(m_lmbDownPos, m_mousePos), hi = glm::max(m_lmbDownPos, m_mousePos);
            oc::vector<Entity*> units;
            NpcSystem::queryVisibleUnits(camera, FLT_MAX, units); // a box select reaches every unit on screen, however far
            for (Entity* e : units)
            {
                const GameUnitComponent* u = getComponent<GameUnitComponent>(e);
                if (!u || u->puppet || !u->alive() || u->team != (uint32)m_team)
                    continue;
                glm::vec2 sp;
                if (!camera.worldToScreen(viewport, e->pos, sp))
                    continue;
                if (sp.x < lo.x || sp.x > hi.x || sp.y < lo.y || sp.y > hi.y)
                    continue;
                bool already = false;
                for (const EntityPtr& p : m_selectedUnits)
                    already |= p.get() == e;
                if (!already)
                    m_selectedUnits.push_back(EntityPtr(e));
            }
        }
    }
    pruneSelectedUnits();
    for (const EntityPtr& p : m_selectedUnits)
        drawCircle(glm::vec3(p->pos.x, groundY(p->pos.x, p->pos.z, p->pos.y - 0.85f) + 0.15f, p->pos.z), 0.9f,
            packColor(glm::vec3(0.4f, 1.0f, 0.5f)), 16);
}

void GameMatch::pruneSelectedUnits()
{
    for (size_t i = 0; i < m_selectedUnits.size();)
    {
        const GameUnitComponent* u = getComponent<GameUnitComponent>(m_selectedUnits[i].get());
        if (!u || !u->alive())
            m_selectedUnits.erase(m_selectedUnits.begin() + i);
        else
            ++i;
    }
}

// A barracks route becomes a LANE in the team's flow field: one A* per leg (barracks -> wp1 ->
// ...), written as flow the marching units simply follow - no unit plans anything. The lane decays
// like any other ("Nav/Flow decay"), and the units walking it keep it alive.
void GameMatch::seedRouteLane(uint32 structureId)
{
    if (m_isClient)
        return;
    const int index = m_structures.structureIndexById(structureId);
    if (index < 0 || !isBarracksType(m_structures.structureType(index)))
        return;
    const oc::span<const glm::vec3> route = m_structures.structureRoute(index);
    const uint8 team = m_structures.structureTeam(index);
    glm::vec3 from = m_structures.structurePos(index);
    for (const glm::vec3& wp : route)
    {
        Globals::navSystem.seedPath(team, from, wp, laneSeedSpeed(), laneSeedWidth());
        from = wp;
    }
}

// Centroid of the LARGEST CLUSTER of the selected units (single-linkage flood fill with
// `linkRadius`): the lane a move order seeds must start where the BULK of the group is, and a plain
// average would be dragged off by one straggler across the map. Selections are small, so O(n^2) is
// the right amount of machinery here.
static bool largestClusterCentroid(oc::span<const EntityPtr> units, float linkRadius, glm::vec3& out)
{
    const size_t n = units.size();
    if (n == 0)
        return false;
    oc::small_vector<int, 64> cluster;   // -1 = unassigned
    oc::small_vector<int, 64> stack;
    for (size_t i = 0; i < n; ++i)
        cluster.push_back(-1);
    const float r2 = linkRadius * linkRadius;
    int best = -1, bestCount = 0;
    glm::vec3 bestSum(0.0f);
    int clusters = 0;
    for (size_t seed = 0; seed < n; ++seed)
    {
        if (cluster[seed] >= 0)
            continue;
        const int id = clusters++;
        int count = 0;
        glm::vec3 sum(0.0f);
        cluster[seed] = id;
        stack.push_back(int(seed));
        while (!stack.empty())
        {
            const int i = stack.back();
            stack.pop_back();
            sum += units[i]->pos;
            ++count;
            for (size_t j = 0; j < n; ++j)
            {
                if (cluster[j] >= 0)
                    continue;
                const glm::vec2 d(units[j]->pos.x - units[i]->pos.x, units[j]->pos.z - units[i]->pos.z);
                if (glm::dot(d, d) <= r2)
                {
                    cluster[j] = id;
                    stack.push_back(int(j));
                }
            }
        }
        if (count > bestCount)
        {
            bestCount = count;
            bestSum = sum;
            best = id;
        }
    }
    if (best < 0)
        return false;
    out = bestSum / float(bestCount);
    return true;
}

// THE right-click move order, shared by the RMB handler and the profiling scenario: the player and
// the selected units walk to a world position (a fresh order: the lane is seeded from the group).
bool GameMatch::moveOrderAt(const glm::vec3& worldPos, bool includePlayer)
{
    // A click that lands inside co-op rock clamps to the nearest open cell - a target on blocked
    // cells fails the lane A* and every unit's own plan request (the pointOutsideFootprint rule,
    // applied to terrain).
    const glm::vec3 dest = clampToOpenGround(glm::vec3(worldPos.x, 0.0f, worldPos.z));
    if (includePlayer)
        m_player.setMoveTarget(dest);
    return orderSelectedUnits(dest, true);
}

// A clicked ground point on a structure, pushed just OUTSIDE its footprint along the side it fell
// on: the capsule ends up at that face instead of grinding into the wall, and the Nav A* has a
// reachable goal (a point inside the footprint is blocked cells - no lane, and every unit's own
// plan request to it fails too).
glm::vec3 GameMatch::pointOutsideFootprint(const glm::vec3& clicked, int structure) const
{
    const glm::vec3 center = m_structures.structurePos(structure);
    const float half = StructureSystem::footprintCellsOf(m_structures.structureType(structure))
        * StructureSystem::GridCellSize * 0.5f + 1.2f; // + capsule and a gap
    glm::vec2 d(clicked.x - center.x, clicked.z - center.z);
    const float deepest = glm::max(glm::abs(d.x), glm::abs(d.y));
    if (deepest < half) // inside the inflated footprint: push out to the nearest face
    {
        if (deepest < 1e-3f) // dead center: come from the player's side
            d = glm::vec2(m_player.bodyPos().x - center.x, m_player.bodyPos().z - center.z);
        const float scale = glm::max(glm::abs(d.x), glm::abs(d.y));
        d = scale > 1e-3f ? d * (half / scale) : glm::vec2(half, 0.0f);
    }
    return glm::vec3(center.x + d.x, 0.0f, center.z + d.y);
}

bool GameMatch::orderSelectedUnits(const glm::vec3& target, bool freshOrder)
{
    for (const EntityPtr& p : m_selectedUnits)
        if (GameUnitComponent* u = getComponent<GameUnitComponent>(p.get()))
            u->orderMove(glm::vec3(target.x, 0.0f, target.z), freshOrder);
    bool laneSeeded = false;
    // A FRESH order seeds a planned LANE from the group to the destination: one A* (a job; Nav
    // writes the lane on a later update), into the team flow, and the units follow it as crowd flow - the group routes around
    // buildings without any of them planning. The start is the largest cluster's centre, so a lone
    // straggler cannot pull the lane's origin away from the bulk of the group.
    glm::vec3 groupPos;
    if (freshOrder && largestClusterCentroid(m_selectedUnits, m_settings.selectionClusterRadius, groupPos))
        laneSeeded = Globals::navSystem.seedPath(uint32(m_team), groupPos, target, laneSeedSpeed(), laneSeedWidth());
    // (No group re-seed timer: while they walk, the units themselves ask for a lane on their own
    // timers and Nav's proximity dedup turns the whole group's requests into one plan - see
    // GameUnitComponent's plan request and NavSystem::requestSeedPath.)
    return laneSeeded;
}

// RIGHT-CLICK actions with a selection, shared by Select AND Build mode: on a structure = SMART
// CONNECT (below); on GROUND with an own-team BARRACKS selected = set its unit ROUTE waypoint
// (SHIFT appends, a plain click restarts the route).
void GameMatch::updateRightClickActions(const Camera& camera, bool rmbEdge)
{
    if (!rmbEdge || m_selectedId == 0)
        return;
    if (const int hover = hoveredStructure(camera);
        hover >= 0 && !isWalkThrough(m_structures.structureType(hover)))
        return; // on a building: the caller turns it into a MOVE order (walk-through pieces are ground)
    const int sel = m_structures.structureIndexById(m_selectedId);
    glm::vec3 ground;
    if (sel < 0 || !isBarracksType(m_structures.structureType(sel))
        || m_structures.structureTeam(sel) != (uint8)m_team || !aimGroundPoint(camera, ground))
        return; // not a route click either: the caller turns it into a MOVE order
    ground.y = 0.0f;
    ground = clampToOpenGround(ground); // a waypoint inside co-op rock would stall every marcher
    m_rmbConsumed = true;
    oc::vector<glm::vec3> route;
    if (Globals::input.isKeyDown(SDL_Scancode::SDL_SCANCODE_LSHIFT))
    {
        const oc::span<const glm::vec3> current = m_structures.structureRoute(sel);
        route.assign(current.begin(), current.end()); // hold shift: extend the route
    }
    if ((int)route.size() < StructureSystem::MaxRouteWaypoints)
        route.push_back(ground);
    requestSetRoute(m_selectedId, route);
}

// Click-to-select, shared by Select AND Build mode so inspecting a building never needs a mode
// switch: hover ring, LMB picks (empty ground deselects), and the selection keeps its highlight.
// allowPick false = this frame's click belongs to something else (a valid placement), so only the
// rings draw - the selection still shows while building.
void GameMatch::updateSelectionClick(const Camera& camera, bool confirmEdge, bool allowPick)
{
    const int hover = hoveredStructure(camera);
    if (hover >= 0 && allowPick)
        drawCircle(m_structures.structureGroundPos(hover) + glm::vec3(0.0f, 0.3f, 0.0f), 1.6f,
            packColor(glm::vec3(0.9f, 0.9f, 0.9f)), 20);
    if (confirmEdge && allowPick)
        m_selectedId = hover >= 0 ? m_structures.structureId(hover) : 0;

    int selected = -1;
    if (m_selectedId != 0)
    {
        selected = m_structures.structureIndexById(m_selectedId);
        if (selected < 0)
            m_selectedId = 0; // it died
    }
    if (selected >= 0) // highlight ring; the info block rides the world label (buildWorldLabels)
        drawCircle(m_structures.structureGroundPos(selected) + glm::vec3(0.0f, 0.3f, 0.0f), 2.0f,
            packColor(glm::vec3(0.3f, 1.0f, 0.4f)), 24);
}
