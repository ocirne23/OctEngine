module Game;

import Core;
import Core.glm;
import Core.Camera;
import Core.Rect;
import Core.GameHud;
import Entity;
import Threading;
import :Match;
import :Player;
import :Structures;
import :Npc;

// World-anchored UI: a health bar above every damageable structure, plus name/HP/power info on the
// selected one. Projected with THIS frame's final camera (worldToScreen - captured in
// updateWindowed), replaced wholesale each frame; the overlay just paints at the given viewport
// pixels. A JOB: submitted at the END of the game tick (structures settled for the frame; only the
// entity pass runs alongside, and it changes field values, never the rosters or the structure
// list - torn float reads are fine for a bar) and joined by main right before the widget pass is
// queued (joinWorldLabels), which is what consumes the labels. GameHud's writes are mutexed.
void GameMatch::submitWorldLabels(float deltaSec)
{
    if (!m_labelsCameraValid)
        return; // headless / no windowed tick this frame
    m_labelsDelta = deltaSec; // the warning timers age by this inside the job
    Globals::jobSystem.submit([this] { buildWorldLabels(); }, { "Game world labels", EProfileCategory::Game },
        EJobPriority::Normal, &m_labelsCounter);
}

void GameMatch::joinWorldLabels()
{
    Globals::jobSystem.wait(m_labelsCounter);
}

// The most pressing PROBLEM over an own-team structure, as a short badge (nullptr = nothing wrong).
// These are the states the bars alone do not explain: a consumer with no power, an input that never
// arrives, an output with nowhere to go, a full store, a capped barracks. Ordered by severity -
// one badge per structure, the first hit wins. Blueprints are inert BY DESIGN and never warn.
const char* GameMatch::structureWarning(int index, glm::vec3& color) const
{
    const EStructureType type = m_structures.structureType(index);
    if (isCableOrCrossing(type) || m_structures.structureBlueprint(index)
        || m_structures.structureTeam(index) != (uint8)m_team)
        return nullptr;
    const GameStructureComponent& s = *m_structures.structures()[index].state;
    const auto linked = [&](uint8 medium) { return (s.attachedMask & (1u << medium)) != 0; }; // a run touches it
    constexpr glm::vec3 c_red(1.0f, 0.35f, 0.3f), c_orange(1.0f, 0.62f, 0.25f), c_amber(1.0f, 0.85f, 0.35f);

    // 1) A MEDIUM WITH NO CABLE OF ITS OWN. Whichever media a structure MOVES - one it eats, one it
    //    makes, one it banks - a missing line of that medium is dead weight: the input can never
    //    arrive, or the output has nowhere to go. Structural, so it does NOT gate on the current
    //    stock: a cabled-but-starved or cabled-but-backed-up machine is not a badge (its bars say
    //    that, and it resolves itself), while an uncabled one never resolves.
    //    The BASE is exempt: it is the hub, self-generates, and starts every match bare.
    const int node = m_structures.structureNodeIndex(index);
    const bool fuelNode = node >= 0 && node < m_structures.nodeCount()
        && m_structures.nodeType(node) == ENodeType::Fuel; // an extractor makes ONE of the two
    // ENERGY: burnt by the emitters/machines/turrets, banked by batteries, made by gen + solar.
    const bool energy = hasShieldEmitter(type) || isBarracksType(type)
        || type == EStructureType::Extractor || type == EStructureType::Fabricator
        || type == EStructureType::Constructor || type == EStructureType::MedicStation
        || type == EStructureType::Turret || type == EStructureType::Generator
        || type == EStructureType::Solar || type == EStructureType::Battery;
    // FUEL: burnt by generators and fabricators, banked by tanks, made by a fuel-node extractor.
    const bool fuel = type == EStructureType::Generator || type == EStructureType::Fabricator
        || type == EStructureType::FuelTank || (type == EStructureType::Extractor && fuelNode);
    // MINERALS: spent by constructors, banked by silos, made by fabricators + mineral extractors.
    const bool minerals = type == EStructureType::Constructor || type == EStructureType::MineralSilo
        || type == EStructureType::Fabricator || (type == EStructureType::Extractor && !fuelNode);
    if (type != EStructureType::Base)
    {
        if (energy && !linked(0))
        {
            color = c_red;
            return "No power cable";
        }
        if (fuel && !linked(1))
        {
            color = c_orange;
            return "No pipeline";
        }
        if (minerals && !linked(2))
        {
            color = c_amber;
            return "No conveyor";
        }
    }
    // 2) POPULATION CAPPED: the build bar fills but the unit can never be born (build houses).
    if (isBarracksType(type)
        && s.barracks.population + (int)s.barracks.spawnPop > s.barracks.popCap)
    {
        color = c_amber;
        return "Pop full";
    }
    return nullptr;
}

void GameMatch::buildWorldLabels()
{
    ProfileScope scope("Game world labels", EProfileCategory::Game);
    const Camera& camera = m_labelsCamera;
    // Both are the job's kept scratch, SWAPPED into GameHud at the end (last frame's list comes
    // back with its capacity): no list allocation per frame, only the label strings that outgrow
    // the SSO buffer (the selected structure's info block).
    oc::vector<HudWorldLabel>& labels = m_labelsScratch;
    labels.clear();
    labels.reserve(m_structures.structureCount());
    HudPopup& popup = m_labelsPopup; // the selected own barracks' unit-type picker (inactive = none)
    popup.active = false;
    popup.title.clear();
    popup.buttons.clear();
    const Rect& viewport = m_labelsViewport;
    // CULLING. worldToScreen only rejects what is BEHIND the camera, so without these every
    // structure on the map and every unit in the frustum built a label, was copied into GameHud
    // and walked by the widget pass, which then clipped most of them. A label past "Label max
    // distance" is unreadable and one outside the viewport (plus the overlay's own margin) is
    // never drawn; neither is worth building. The selected structure keeps its label regardless.
    // The distance is from the PLAYER, not the camera: the top-down camera sits well above and
    // behind the capsule, and what matters is what is near the player (the camera is the
    // fallback when there is no player entity - the editor, a spectating client).
    const Entity* cullEntity = m_player.entity();
    const glm::vec3 cullCenter = cullEntity ? cullEntity->pos : camera.position;
    const float maxDist2 = m_labelMaxDistance * m_labelMaxDistance;
    const auto inRange = [&](const glm::vec3& p)
    {
        const glm::vec3 d = p - cullCenter;
        return glm::dot(d, d) <= maxDist2;
    };
    const glm::vec2 vpMin = glm::vec2(viewport.min) - 100.0f;
    const glm::vec2 vpMax = glm::vec2(viewport.max) + 100.0f;
    const auto onScreen = [&](const glm::vec2& p)
    {
        return p.x >= vpMin.x && p.x <= vpMax.x && p.y >= vpMin.y && p.y <= vpMax.y;
    };
    // PROBLEM BADGES, on a per-structure JITTERED ~1 s timer: the check scans a structure's links,
    // and every state it reports changes on the timescale of a player's actions, so re-running it
    // per structure per frame is waste. The jitter is a STABLE per-id phase (a hash of the
    // structure id), so a batch placed or loaded together spreads over the interval instead of
    // re-checking in lockstep forever. The result rides the roster entry (Ref::warning).
    {
        constexpr float c_warningInterval = 1.0f;
        for (int i = 0; i < m_structures.structureCount(); ++i)
        {
            float& timer = m_structures.structureWarningTimer(i);
            timer -= m_labelsDelta;
            if (timer > 0.0f)
                continue;
            const uint32 hash = m_structures.structureId(i) * 2654435761u;
            const float phase = 0.75f + 0.5f * (float)(hash >> 8) / (float)(1u << 24); // 0.75 .. 1.25
            timer = c_warningInterval * phase;
            glm::vec3 color(1.0f);
            const char* warning = structureWarning(i, color);
            m_structures.setStructureWarning(i, warning, color);
        }
    }
    const int selected = m_selectedId != 0 ? m_structures.structureIndexById(m_selectedId) : -1;
    for (int i = 0; i < m_structures.structureCount(); ++i)
    {
        const EStructureType type = m_structures.structureType(i);
        // Cable segments: no label unless there is something to show - a blueprint's build
        // progress or damage, AUTHORITY only (cable health is not mirrored, so a client's copy
        // would read full). Hundreds of full-health segments would drown the HUD.
        const float healthMax = m_structures.structures()[i].state->healthMax;
        if (isCableOrCrossing(type) && i != selected
            && (m_isClient || (!m_structures.structureBlueprint(i)
                && m_structures.structureHealth(i) >= healthMax - 1e-3f)))
            continue;
        HudWorldLabel label;
        const glm::vec3 anchor = m_structures.structureLabelAnchor(i);
        if (i != selected && !inRange(anchor))
            continue;
        if (!camera.worldToScreen(viewport, anchor, label.screenPos) || !onScreen(label.screenPos))
            continue;
        label.title = c_structureShortNames[(int)type]; // the selected one overrides w/ full name
        if (const char* warning = m_structures.structureWarning(i)) // the cached problem bubble
        {
            label.warning = warning;
            label.warningColor = m_structures.structureWarningColor(i);
        }
        const bool consumer = hasShieldEmitter(type) || type == EStructureType::Extractor
            || type == EStructureType::Fabricator || type == EStructureType::MedicStation;
        label.barValue = m_structures.structureHealth(i);
        label.barMax = healthMax; // per-type: cables are softer than buildings
        {
            const float frac = label.barValue / label.barMax;
            // Blueprint: health IS the construction progress - the bar reads blue while building.
            label.barColor = m_structures.structureBlueprint(i) ? glm::vec3(0.5f, 0.7f, 1.0f)
                : glm::mix(glm::vec3(1.0f, 0.25f, 0.2f), glm::vec3(0.3f, 1.0f, 0.4f), frac);
        }
        // A FULL health bar stays hidden - only damage (or blueprint progress, or selection, or
        // "AlwaysDisplayHealth true" in the .pre) draws one.
        if (!m_structures.structureBlueprint(i) && i != selected
            && !m_structures.structures()[i].state->alwaysDisplayHealth
            && label.barValue >= label.barMax - 1e-3f)
            label.barMax = 0.0f; // <= 0 = no bar (the selected-info HP string is selection-only)
        const float energyCap = m_structures.structureCapacity(i);
        const float fuelCap = m_structures.structureFuelCapacity(i);
        const float mineralCap = m_structures.structureMineralCapacity(i);
        // Second bar: fuel (orange) on the fuel holders, MINERALS (blue) on the mineral stores and
        // on anything that runs on minerals alone, energy (yellow) elsewhere (barracks included).
        const bool fuelBar = type == EStructureType::Generator || type == EStructureType::FuelTank;
        const bool mineralBar = type == EStructureType::MineralSilo
            || (mineralCap > 0.0f && energyCap <= 0.0f);
        // The STORE bars are opt-in when unselected: only the prefabs that author
        // `AlwaysShowResources true` (storage, emitters, barracks - the stores a player watches at
        // a glance) carry them around, everything else shows them while SELECTED. The health bar
        // is unaffected: damage always shows one.
        const bool showResources = i == selected
            || m_structures.structures()[i].state->alwaysShowResources;
        if (m_structures.structureBlueprint(i) || !showResources)
        {
        } // blueprint: no second bar - the (blue) health bar IS the build progress
        else if (isCableOrCrossing(type))
        {
            // A conduit's second bar is its THROUGHPUT: the ~2 s average of cells leaving the
            // segment against its out-rate, hued by medium.
            StructureSystem::CableInfo ci;
            if (m_structures.cableInfo(i, ci))
            {
                label.bar2Value = ci.movedPerSec;
                label.bar2Max = ci.ratePerSec;
                label.bar2Color = ci.medium == 1 ? glm::vec3(1.0f, 0.6f, 0.2f)
                    : ci.medium == 2 ? glm::vec3(0.35f, 0.5f, 1.0f) : glm::vec3(1.0f, 0.9f, 0.3f);
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
            // A barracks' store IS its build bar (capacity = the unit's cost) and a turret's IS its
            // reload (capacity = one shot): green progress, not energy-yellow.
            label.bar2Color = isBarracksType(type) || type == EStructureType::Turret
                ? glm::vec3(0.4f, 0.95f, 0.5f) : glm::vec3(1.0f, 0.9f, 0.3f);
            if (mineralCap > 0.0f) // the Base: energy AND its spendable mineral bank
            {
                label.bar3Value = m_structures.structureMinerals(i);
                label.bar3Max = mineralCap;
                label.bar3Color = glm::vec3(0.35f, 0.5f, 1.0f);
            }
        }
        if (i == selected)
        {
            label.emphasized = true;
            label.title = structureTypeName(type);
            char info[192];
            int len = snprintf(info, sizeof(info), "HP %.0f / %.0f", label.barValue, label.barMax);
            if (energyCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len,
                    isBarracksType(type) ? "\nBuild %.0f / %.0f" : "\nEnergy %.0f / %.0f",
                    m_structures.structureCharge(i), energyCap);
            if (fuelCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nFuel %.0f / %.0f",
                    m_structures.structureFuel(i), fuelCap);
            if (mineralCap > 0.0f && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nMinerals %.0f / %.0f",
                    m_structures.structureMinerals(i), mineralCap);
            if (isBarracksType(type) && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\nPopulation %d / %d (%d houses)",
                    m_structures.structurePopulation(i), m_structures.structurePopCap(i),
                    m_structures.structureHouses(i));
            if (type == EStructureType::House && len > 0 && len < (int)sizeof(info))
                len += snprintf(info + len, sizeof(info) - len, "\n%s",
                    m_structures.structureLinkedId(i) != 0 ? "Linked to a barracks" : "No barracks in range");
            if (isCableOrCrossing(type) && len > 0 && len < (int)sizeof(info))
            {
                // The segment's transport readout: cells held, cells that left it last tick
                // against its out-rate, and its run's total (see StructureSystem::cableInfo).
                static constexpr const char* c_medium[3] = { "Energy", "Fuel", "Minerals" };
                StructureSystem::CableInfo ci;
                if (m_structures.cableInfo(i, ci))
                    len += snprintf(info + len, sizeof(info) - len, "\n%s %d / %d cells, %.1f / %.1f per s\nRun %d / %d cells over %d segments",
                        c_medium[glm::clamp(ci.medium, 0, 2)], ci.fill, ci.capacity, ci.movedPerSec, ci.ratePerSec,
                        ci.runFill, ci.runCapacity, ci.runSegments);
                else
                    len += snprintf(info + len, sizeof(info) - len, "\nNot conducting");
            }
            if (consumer && len > 0 && len < (int)sizeof(info))
                snprintf(info + len, sizeof(info) - len, "\n%s",
                    m_structures.structurePowered(i) ? "Powered" : "No power");
            label.info = info;
            // The unit-type picker floats above an OWN barracks' label: one button per type, the
            // produced one highlighted. Clicks resolve in updateWindowed (popupButtonAtScreenPos).
            if (isBarracksType(type) && m_structures.structureTeam(i) == (uint8)m_team)
            {
                popup.active = true;
                popup.screenPos = label.screenPos;
                popup.title = "Produce";
                popup.buttons.reserve(oc::size(c_barracksMenu));
                const uint8 produced = m_structures.structureUnitType(i);
                for (const uint8 t : c_barracksMenu)
                {
                    HudPopupButton& b = popup.buttons.emplace_back();
                    b.label = c_unitTypeNames[t];
                    char sub[32];
                    snprintf(sub, sizeof(sub), "%d pop  %.0f E", m_structures.unitPopulation(t),
                        m_structures.unitSpawnEnergy(t));
                    b.sub = sub;
                    b.selected = t == (int)produced;
                }
            }
        }
        labels.push_back(oc::move(label));
    }
    // Units + players: own team green, enemy teams red. Only VISIBLE ones are fetched - an
    // off-screen one would just fail worldToScreen below. Works identically on server AND client:
    // remote instances' GameUnitComponents are populated by the snapshot game blob. Puppets are
    // player capsules; the own player is skipped (its HUD bars cover it).
    Entity* ownPlayer = m_player.entity();
    oc::vector<Entity*>& units = m_labelUnits; // the labels job's own scratch (one job at a time)
    // The query measures from the CAMERA; a unit within maxDist of the player is within
    // maxDist + |camera - player| of the camera, so that bound keeps the traversal tight and the
    // exact player-distance test below does the rest.
    NpcSystem::queryVisibleUnits(camera, m_labelMaxDistance + glm::distance(cullCenter, camera.position), units);
    for (Entity* unitEntity : units)
    {
        const GameUnitComponent* u = getComponent<GameUnitComponent>(unitEntity);
        if (!u || unitEntity == ownPlayer || !inRange(unitEntity->pos))
            continue;
        // (Swarm bodies included: full bars are hidden, so only the DAMAGED slice of a thousand-
        // body horde pushes a label - the drown-the-HUD concern the old shieldOutput skip covered.)
        HudWorldLabel label;
        const float height = u->puppet ? 2.0f : 1.6f;
        if (!camera.worldToScreen(viewport, unitEntity->pos + glm::vec3(0.0f, height, 0.0f), label.screenPos)
            || !onScreen(label.screenPos)) // the frustum query is conservative (entry bounds)
            continue;
        label.title = u->getShortName(); // the prefab's `ShortName` tag (same on every instance - no wire type needed)
        // FULL bars stay hidden ("AlwaysDisplayHealth true" in the .pre opts a prefab back in):
        // only damage draws attention. An undamaged non-player unit skips its label entirely -
        // no floating name over a healthy crowd; players always keep their name tag.
        const bool always = u->alwaysDisplayHealth;
        if (!u->puppet && !u->collapsed && u->energy > 0.0f)
        { // shield-less bodies (swarm) spawn with a ZERO battery, so they land in the health branch
            // UNIT with a live shield: ONE bar - the shield IS the unit's front line, so the bar
            // shows it (shield color) until it collapses; only then does the health bar take over.
            if (always || u->energy < u->energyMax - 1e-3f)
            {
                label.barValue = u->energy;
                label.barMax = glm::max(u->energyMax, 1e-3f);
                label.barColor = glm::vec3(1.0f, 0.9f, 0.3f);
            }
        }
        else
        {
            if (always || u->health < u->healthMax - 1e-3f)
            {
                label.barValue = u->health;
                label.barMax = glm::max(u->healthMax, 1e-3f); // per-type: Brutes triple, Runners half
                label.barColor = u->team == (uint32)m_team
                    ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(1.0f, 0.25f, 0.2f);
            }
            if (u->puppet && (always || u->energy < u->energyMax - 1e-3f))
            {
                label.bar2Value = u->energy; // players keep both bars: shield under health
                label.bar2Max = glm::max(u->energyMax, 1e-3f);
                label.bar2Color = glm::vec3(1.0f, 0.9f, 0.3f);
            }
        }
        if (!u->puppet && label.barMax <= 0.0f)
            continue;
        labels.push_back(oc::move(label));
    }
    Globals::gameHud.swapWorldLabels(labels);
    Globals::gameHud.swapPopup(popup);
}
