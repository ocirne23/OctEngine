module Game;

import Core;
import Core.glm;
import Core.Log;
import Entity;
import Force;
import Physics;
import RendererVK;
import Spatial;
import :Structures;
import :Sound;

// THE ECONOMY TICKS + materials (see Structures.ixx): production (income, fuel burn, consumer
// drain, the emitter ramps - over the roster, the transport moves the cells between them), the
// death sweep, the constructors, the shared materials model (HEALTH IS THE PROGRESS: the same
// price builds a blueprint and repairs a damaged structure), the tint, and the debug draw.

static constexpr glm::vec3 c_blueprintColor(0.45f, 0.55f, 0.7f); // ghost tint until built
static constexpr glm::vec3 c_rubbleColor(0.1f, 0.085f, 0.075f);  // rubble: burnt dark until it is built again

// ---------------------------------------------------------------- combat/economy helpers

uint32 StructureSystem::randomTargetStructureId(const glm::vec3& nearPos, uint8 attackerTeam) const
{
    // (The units target through their own spatial queries now; this remains for tooling.)
    constexpr int MaxCandidates = 4;
    struct Candidate { float distSq; uint32 id; };
    Candidate best[MaxCandidates];
    int count = 0;
    for (const Ref& s : m_frame)
    {
        if (s.type == EStructureType::Base || s.state->team == attackerTeam)
            continue;
        const glm::vec2 d = glm::vec2(s.entity->pos.x, s.entity->pos.z) - glm::vec2(nearPos.x, nearPos.z);
        const Candidate c{ glm::dot(d, d), s.state->structureId };
        if (count < MaxCandidates)
            best[count++] = c;
        else
        {
            int worst = 0;
            for (int i = 1; i < MaxCandidates; ++i)
                if (best[i].distSq > best[worst].distSq)
                    worst = i;
            if (c.distSq < best[worst].distSq)
                best[worst] = c;
        }
    }
    if (count == 0)
        return 0;
    return best[glm::clamp((int)glm::linearRand(0.0f, (float)count), 0, count - 1)].id;
}

void StructureSystem::damageStructure(uint32 id, float amount)
{
    const int index = structureIndexById(id);
    if (index >= 0)
        m_frame[index].state->damage(amount); // atomic; invulnerable (Base) refuses inside
}

void StructureSystem::spendMinerals(uint8 team, float amount)
{
    // Silos drain first; the Base last - its trickle refills, and keeping silo stock rotating
    // makes conveyor supply lines visibly matter.
    for (const EStructureType pass : { EStructureType::MineralSilo, EStructureType::Base })
        for (const Ref& s : m_frame)
        {
            if (amount <= 0.0f)
                return;
            if (s.type != pass || s.state->team != team)
                continue;
            const float take = glm::min(amount, s.state->store[2]);
            s.state->store[2] -= take;
            amount -= take;
        }
}

// The medium hues the cable prefabs/flow rings use: power yellow, fuel orange, minerals blue.
static glm::vec3 mediumHue(int medium)
{
    return medium == 1 ? glm::vec3(1.0f, 0.55f, 0.15f)
         : medium == 2 ? glm::vec3(0.35f, 0.5f, 1.0f) : glm::vec3(0.9f, 0.9f, 0.3f);
}

void StructureSystem::applyStructureTint(const Ref& s)
{
    RenderComponent* rc = getComponent<RenderComponent>(s.entity);
    if (!rc || !rc->node.isValid())
        return;
    glm::vec3 color = s.state->rubble ? c_rubbleColor : c_blueprintColor;
    if (!s.state->blueprint)
    {
        color = glm::vec3(1.0f); // fallback: authored tint missing
        if (const RenderComponent::SpawnInfo* info = getRenderSpawnInfo(s.entity); info && info->color.x >= 0.0f)
            color = info->color;
        // The three crossings share one authored-gray prefab: the type's medium hues it.
        if (isCrossingType(s.type))
            color = mediumHue(crossingMediumOf(s.type));
    }
    const auto material = RenderMaterial::getOrCreateSolidColor(color);
    rc->node.setMaterialOverride(material);
    // Cables/crossings are COMPOSITES (arm/end/ramp child entities): tint the child pieces along -
    // this is also what turns them blueprint-gray and back. A BUILDING's child pieces (its crystal, roof, posts)
    // are gray as a blueprint and take their OWN authored colour once built.
    if (const SceneComponent* sc = getComponent<SceneComponent>(s.entity))
        for (const EntityPtr& child : sc->children)
            if (RenderComponent* crc = getComponent<RenderComponent>(child.get()); crc && crc->node.isValid())
            {
                const RenderComponent::SpawnInfo* info = getRenderSpawnInfo(child.get());
                const bool own = !isCableOrCrossing(s.type) && !s.state->blueprint && info && info->color.x >= 0.0f;
                crc->node.setMaterialOverride(own ? RenderMaterial::getOrCreateSolidColor(info->color, info->emissive) : material);
            }
}

// RUBBLE IS NOT BUILT UP UNDER ATTACK: a living enemy unit within "Rubble safe radius (m)" of it (flat, on the map)
// blocks every way to build it - the Builder's hut, the workers and the players all pay through investMaterials,
// and the searches for work skip it (blueprintNeed, fundNearbyBlueprint). A PLAYER is the exception (byPlayer): it
// builds rubble up with enemies near - it stands there and takes the risk; only a body on the pile stops it.
bool StructureSystem::rubbleBlocked(const Ref& s, bool byPlayer) const
{
    if (!s.state->rubble)
        return false;
    // A BODY ON THE PILE blocks it too (a player or a unit of any team): rubble is walked over, and the building
    // that comes back would close around whoever stands there - nothing moves a body out of a building.
    if (!byPlayer && m_settings.rubbleSafeRadius > 0.0f && s.rubbleUnsafe) // (the cheap test first)
        return true;
    return actorInFootprint(s.type, s.entity->pos - glm::vec3(0.0f, spawnHeightOf(s.type), 0.0f));
}

void StructureSystem::refreshRubbleSafety(float deltaSec)
{
    constexpr float c_rubbleCheckPeriod = 0.25f; // s
    m_rubbleCheckTimer -= deltaSec;
    if (m_rubbleCheckTimer > 0.0f)
        return;
    m_rubbleCheckTimer = c_rubbleCheckPeriod;
    ProfileScope scope("Structures rubble safety", EProfileCategory::Game);
    const float radius = m_settings.rubbleSafeRadius;
    for (const Ref& s : m_frame)
    {
        if (!s.state->rubble || radius <= 0.0f)
        {
            s.rubbleUnsafe = false;
            continue;
        }
        const glm::vec3 pos = s.entity->pos;
        const uint32 team = s.state->team;
        bool enemy = false;
        Globals::spatialIndex.forEachInSphere(glm::dvec3(pos), radius, SpatialLayer_Render, [&](uint64 user)
        {
            if (enemy)
                return;
            Entity* other = reinterpret_cast<Entity*>(user);
            const GameUnitComponent* unit = getComponent<GameUnitComponent>(other);
            if (!unit || unit->puppet || unit->team == team || !unit->alive())
                return;
            const glm::vec2 d(other->pos.x - pos.x, other->pos.z - pos.z);
            enemy = glm::dot(d, d) <= radius * radius;
        });
        s.rubbleUnsafe = enemy;
    }
}

float StructureSystem::investMaterials(const Ref& s, float amount, float repairHealCap, bool byPlayer)
{
    if (rubbleBlocked(s, byPlayer))
        return 0.0f;
    // HEALTH IS THE PROGRESS: materials heal at cost/healthMax per hp - the same price builds a
    // blueprint and repairs a damaged structure. A full-health blueprint flips to BUILT. Heals
    // against the COMPONENT's healthMax (cables are softer than buildings).
    if (amount <= 0.0f || !isRepairableType(s.type))
        return 0.0f; // the Base and the ship's rail are never placed, but they repair at their m_settings.costs entry like the rest
    const float healthMax = glm::max(s.state->healthMax, 1e-3f);
    const float materialsPerHp = glm::max(m_settings.costs[(int)s.type], 0.01f) / healthMax;
    // A REPAIR has a health rate of its own (repairHealCap): by the stone price alone, a cheap structure with much
    // health (a Wall: 5 stone, 500 health) would be whole again in a moment.
    const float heal = glm::min(glm::min(amount / materialsPerHp, healthMax - s.state->health),
        s.state->blueprint ? FLT_MAX : repairHealCap);
    if (heal <= 0.0f)
        return 0.0f;
    s.state->health += heal;
    if (s.state->blueprint && s.state->health >= healthMax - 1e-3f)
    {
        s.state->blueprint = false;
        if (s.state->rubble) // built again: solid, and a target again
            setRubble(s, false);
        applyStructureTint(s); // back to the authored color
        m_linksDirty = true;   // a finished cable segment starts conducting
        Log::info(oc::string(structureTypeName(s.type)) + " constructed");
        GameSound::post(EGameSound::StructureBuilt, s.entity->pos, isCableOrCrossing(s.type) ? 0.4f : 1.0f);
        if (onStructureBuilt) // server: re-fire GPl - cables are outside GSt, so this IS the
            onStructureBuilt(s.state->structureId); // client's only built notification
    }
    return heal * materialsPerHp;
}

float StructureSystem::takeStoredMinerals(const glm::vec3& pos, float radius, uint8 team, float amount)
{
    // Silos, the Base, AND the quarries: a player standing at a fresh dig site can courier
    // its buffer by hand before any conveyor exists.
    const auto holdsMinerals = [](EStructureType t) {
        return t == EStructureType::MineralSilo || t == EStructureType::Base
            || t == EStructureType::Quarry; };
    float taken = 0.0f;
    for (const Ref& s : m_frame)
    {
        if (amount - taken <= 0.0f)
            break;
        if (s.state->blueprint || s.state->team != team || !holdsMinerals(s.type))
            continue;
        if (glm::distance(glm::vec2(s.entity->pos.x, s.entity->pos.z), glm::vec2(pos.x, pos.z)) > radius)
            continue;
        const float take = glm::min(amount - taken, s.state->store[2]);
        s.state->store[2] -= take;
        taken += take;
    }
    return taken;
}

float StructureSystem::fundNearbyBlueprint(const glm::vec3& pos, float radius, uint8 team, float amount,
    bool includeRepairs, float repairHealCap, bool byPlayer)
{
    if (amount <= 0.0f)
        return 0.0f;
    // The candidates in reach, nearest first; the rubble test (a query for a body on the pile) runs only down that
    // list until one takes the stone - rubble with an enemy near or a body on it takes nothing: the next one gets it.
    oc::small_vector<oc::pair<float, int>, 16> candidates;
    const float radiusSq = radius * radius;
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        const Ref& s = m_frame[i];
        // A type stone does not build would take nothing: as the nearest it would stall the hut for good.
        if (s.state->team != team || !isRepairableType(s.type))
            continue;
        // The Base is never a blueprint, but it IS repairable now that it takes damage.
        const bool wantsMaterials = s.state->blueprint
            || (includeRepairs && s.state->health < s.state->healthMax - 1e-3f);
        if (!wantsMaterials)
            continue;
        const glm::vec2 d(s.entity->pos.x - pos.x, s.entity->pos.z - pos.z);
        if (const float distSq = glm::dot(d, d); distSq < radiusSq)
            candidates.push_back({ distSq, i });
    }
    oc::sort(candidates.begin(), candidates.end());
    for (const auto& [distSq, i] : candidates)
        if (!rubbleBlocked(m_frame[i], byPlayer))
            return investMaterials(m_frame[i], amount, repairHealCap, byPlayer);
    return 0.0f;
}

// ---------------------------------------------------------------- production + sweeps

void StructureSystem::updateStaffing()
{
    for (int t = 0; t < GameMaxTeams; ++t)
        m_workers[t] = m_jobs[t] = 0;
    for (const Ref& ref : m_frame)
    {
        if (ref.state->blueprint || ref.state->team >= GameMaxTeams)
            continue;
        if (ref.type == EStructureType::Base)
            m_workers[ref.state->team] += glm::max(m_settings.baseWorkers, 0);
        else if (ref.type == EStructureType::House)
            m_workers[ref.state->team] += glm::max(m_settings.houseWorkers, 0);
        m_jobs[ref.state->team] += glm::max(workersNeeded(ref.type), 0);
    }
}

void StructureSystem::tickProduction(float deltaSec)
{
    ProfileScope scope("Structures production", EProfileCategory::Game);
    const float dt = glm::max(deltaSec, 1e-6f);
    updateStaffing();
    // LIGHT runs the fields and the machines of defence; PRODUCTION (the lumberyard, the quarry, the fabricator, the constructor,
    // the generator) draws none: it runs on workers.
    // (The Healer's hut pays per health it heals, in its own component update: it is powered while its store holds light.)
    const auto consumerDraw = [&](EStructureType type) {
        return type == EStructureType::Emitter ? m_settings.emitterEnergyPerSec : 0.0f;
    };

    m_genRateTotal = 0.0f;
    m_useRateTotal = 0.0f;
    float totalDemand = 0.0f;
    for (const Ref& ref : m_frame)
    {
        GameStructureComponent& s = *ref.state;
        if (s.blueprint)
        {
            s.powered = false;
            if (hasShieldEmitter(ref.type))
            {
                s.emitter.outputFrac = 0.0f;
                if (ForceComponent* fc = getComponent<ForceComponent>(ref.entity))
                    fc->emitter.setOutput(0.0f);
            }
            continue;
        }

        // ---- income: the lumberyards and the quarries fill their OWN buffer (full = production stalls) at their
        // yield's share of the full rate, the Base trickles minerals into its bank, fabricators convert (fuel -> minerals).
        const float staff = staffing(s.team < GameMaxTeams ? (uint8)s.team : (uint8)0); // the team's workers / jobs
        if (ref.type == EStructureType::Lumberyard)
            s.store[1] = glm::min(s.store[1] + m_settings.fuelRate * ref.yield * staff * dt, s.capacity[1]);
        else if (ref.type == EStructureType::Quarry)
            s.store[2] = glm::min(s.store[2] + m_settings.mineralRate * ref.yield * staff * dt, s.capacity[2]);
        else if (ref.type == EStructureType::Fabricator && s.powered) // powered = it paid its wood last tick
            s.store[2] = glm::min(s.store[2] + m_settings.fabricatorMineralsPerSec * staff * dt, s.capacity[2]);
        else if (ref.type == EStructureType::Base)
            s.store[2] = glm::min(s.store[2] + m_settings.mineralRate * m_settings.baseIncomeMult * dt, s.capacity[2]);
        else if (ref.type == EStructureType::AmmoWorks)
        {
            // THE AMMO WORKS packs stone (by carrier) and light (its light link) into MUNITIONS in its own buffer, at
            // "Ammo works munitions/s" x its staffing; short of either input it makes what they pay for. Full = it stalls.
            // powered = it made munitions this tick (or it is full).
            const float room = glm::max(s.capacity[GoodMunitions] - s.store[GoodMunitions], 0.0f);
            float make = glm::min(m_settings.ammoRate * staff * dt, room);
            if (m_settings.ammoStonePer > 0.0f)
                make = glm::min(make, s.store[2] / m_settings.ammoStonePer);
            if (m_settings.ammoLightPer > 0.0f)
                make = glm::min(make, s.store[0] / m_settings.ammoLightPer);
            make = glm::max(make, 0.0f);
            s.store[GoodMunitions] += make;
            s.store[2] = glm::max(s.store[2] - make * m_settings.ammoStonePer, 0.0f);
            s.store[0] = glm::max(s.store[0] - make * m_settings.ammoLightPer, 0.0f);
            if (dt > 1e-9f)
                m_useRateTotal += make * m_settings.ammoLightPer / dt;
            s.powered = make > 0.0f || room <= 1e-3f;
        }

        // ---- producers: generators burn their OWN tank into their OWN buffer (full buffer =
        // export-limited = no fuel burn), solar trickles for free. (The Base's own generation is with its field, below.)
        if (ref.type == EStructureType::Solar)
        {
            const float add = glm::min(m_settings.solarEnergyPerSec * m_settings.solarDaylight * dt, glm::max(s.capacity[0] - s.store[0], 0.0f));
            s.store[0] += add;
            if (dt > 1e-9f) // paused (sim dt 0): 0/0 would put a NaN into the HUD's gen rate
                m_genRateTotal += add / dt;
        }
        else if (ref.type == EStructureType::Generator && m_settings.genEnergyPerSec > 0.0f)
        {
            float want = glm::min(m_settings.genEnergyPerSec * staff * dt, glm::max(s.capacity[0] - s.store[0], 0.0f));
            if (want > 0.0f)
            {
                const float fuelNeeded = m_settings.fuelBurnRate * want / m_settings.genEnergyPerSec;
                if (fuelNeeded > 1e-9f)
                {
                    const float fuelTaken = glm::min(fuelNeeded, s.store[1]);
                    want *= fuelTaken / fuelNeeded;
                    s.store[1] -= fuelTaken;
                }
                s.store[0] += want;
                if (dt > 1e-9f)
                    m_genRateTotal += want / dt;
            }
        }

        // ---- consumers drain their internal battery. Emitters (the Base's shield included) pay
        // EXTRA per unit of pressure, LATCH OFF at empty until "Emitter restart charge" and ramp
        // their bubble smoothly.
        if (hasShieldEmitter(ref.type))
        {
            ForceComponent* fc = getComponent<ForceComponent>(ref.entity);
            const float pressure = fc ? fc->emitter.getPressure() : 0.0f;
            // THE BASE GENERATES EXACTLY ITS FIELD'S STANDING DRAW (and pays no pressure surcharge): its idle field costs
            // nothing, and it has no light to give away. The enemy siege load still drains its store, so a pressed Base
            // can go dark; dark, it draws nothing and that same generation refills it to the restart charge.
            const bool isBase = ref.type == EStructureType::Base;
            // A FIELD SLEEPS BY DAY: its standing draw follows the darkness (fieldNight), and is whole again the
            // moment a demon is on it (pressure, or the units' drain). Asleep it draws nothing and its shell is faint.
            const bool engaged = s.emitter.unitLoad > 0.0f || pressure > 0.01f;
            const float awake = engaged ? 1.0f : glm::clamp(m_settings.fieldNight, 0.0f, 1.0f);
            const float standing = emitterDrawOf(ref.type) * awake;
            if (isBase)
            {
                const float add = glm::min(standing * dt, glm::max(s.capacity[0] - s.store[0], 0.0f) + (s.emitter.down ? 0.0f : standing * dt));
                s.store[0] += add;
                if (dt > 1e-9f)
                    m_genRateTotal += add / dt;
            }
            const float draw = standing
                + (isBase ? 0.0f : pressure * (1.0f + m_settings.pressureDrawTension * pressure) * m_settings.emitterPressureDraw)
                + s.emitter.unitLoad; // enemy units/shots leaning on the bubble
            s.emitter.unitLoad = 0.0f;
            totalDemand += draw;
            if (s.emitter.down && s.store[0] >= glm::min(m_settings.emitterRestartCharge, energyCapacityOf(ref.type)))
                s.emitter.down = false;
            bool paid = false;
            if (!s.emitter.down)
            {
                paid = s.store[0] >= draw * dt - 1e-4f;
                if (paid)
                {
                    s.store[0] -= draw * dt;
                    m_useRateTotal += draw;
                }
                else
                    s.emitter.down = true;
            }
            s.powered = paid;
            // A FLYING SHIP's Beacon has NO bubble by day (more light than dark: fieldNight < 0.5); it grows back at dusk
            // or once the ship has landed.
            const bool shipDay = ref.onShip && !m_shipLanded && m_settings.fieldNight < 0.5f;
            const float target = paid && !shipDay ? 1.0f : 0.0f;
            const float rampTime = glm::max(target > s.emitter.outputFrac ? m_settings.emitterGrowTime : m_settings.emitterShrinkTime, 0.01f);
            s.emitter.outputFrac = glm::clamp(s.emitter.outputFrac
                + (target > s.emitter.outputFrac ? dt : -dt) / rampTime, 0.0f, 1.0f);
            if (fc)
            {
                // NO sentinel: a dark emitter's field is fully gone.
                fc->emitter.setOutput(emitterOutputOf(ref.type) * s.emitter.outputFrac);
                fc->emitter.setReach(emitterReachOf(ref.type));
                fc->emitter.setShellAlpha(emitterShellAlpha(s.capacity[0] > 0.0f ? s.store[0] / s.capacity[0] : 0.0f,
                    s.structureId, m_settings.emitterWarnCharge) * glm::mix(m_settings.fieldDayAlpha, 1.0f, awake));
            }
            continue;
        }
        const float draw = consumerDraw(ref.type);
        if (ref.type == EStructureType::MedicStation)
        {
            s.powered = s.store[0] > 0.01f;
            continue;
        }
        if (ref.type == EStructureType::AmmoWorks)
            continue; // powered by the income above (it made munitions)
        if (draw <= 0.0f && ref.type != EStructureType::Fabricator)
        {
            // Nothing to pay: a production building (it runs on workers) counts as working.
            if (workersNeeded(ref.type) > 0)
                s.powered = true;
            continue;
        }
        totalDemand += draw;
        // Fabricators burn fuel alongside energy (piped into their own tank) - both must be there.
        const float fuelDraw = ref.type == EStructureType::Fabricator ? m_settings.fabricatorFuelPerSec * staff : 0.0f;
        s.powered = s.store[0] >= draw * dt - 1e-4f && s.store[1] >= fuelDraw * dt - 1e-4f;
        if (s.powered)
        {
            s.store[0] -= draw * dt;
            s.store[1] -= fuelDraw * dt;
            m_useRateTotal += draw;
        }
    }

    // ---- totals ----
    m_gridEnergyTotal = 0.0f;
    m_gridCapacityTotal = 0.0f;
    float fuelSum = 0.0f;
    for (float& f : m_fuelTotal)
        f = 0.0f;
    for (float& m : m_minerals)
        m = 0.0f;
    for (const Ref& s : m_frame)
    {
        // The HUD's "energy stored" counts DEDICATED storage only (the Crystal store, and the Beacon's own store) -
        // production buffers and consumer trickle buffers are working charge, not reserves.
        if (s.type == EStructureType::Battery || s.type == EStructureType::Base)
        {
            m_gridEnergyTotal += s.state->store[0];
            m_gridCapacityTotal += s.state->capacity[0];
        }
        // SPENDABLE minerals = the team's Silo + Base stores (buffers must be conveyed home).
        if (s.type == EStructureType::MineralSilo || s.type == EStructureType::Base)
            m_minerals[s.state->team] += s.state->store[2];
        m_fuelTotal[s.state->team] += s.state->store[1];
        fuelSum += s.state->store[1];
    }
}

void StructureSystem::tickDamage(float)
{
    ProfileScope scope("Structures death sweep", EProfileCategory::Game);
    // Damage happens ON the entities (territory drain + atomic intake in the component) - this is
    // the DEATH SWEEP plus the strainable mark units aim their siege drain at.
    for (size_t i = 0; i < m_frame.size();)
    {
        const Ref& s = m_frame[i];
        // ACTIVE = powered (paid its draw this tick - not latched down) with a bubble up: a dark
        // emitter shrinking out is not a drain target, the next powered one is.
        s.state->strainable = hasShieldEmitter(s.type) && !s.state->blueprint
            && s.state->powered && s.state->emitter.outputFrac > 0.05f;
        // The CPU bubble-radius stand-in shield-less units test against (see GameStructureComponent.ixx):
        // the visible sphere radius is ~half the reach, scaled by the live output ramp.
        s.state->bubbleRadius = s.state->strainable
            ? emitterReachOf(s.type) * 0.5f * s.state->emitter.outputFrac : 0.0f;
        // The Base takes damage but is NEVER destroyed (no lose condition yet): it survives at
        // 0 hp - dead but standing, until players repair it back up.
        if (s.state->invulnerable || s.type == EStructureType::Base || s.state->alive())
        {
            ++i;
            continue;
        }
        // RUBBLE: a BUILT building the enemy destroys stays as rubble - a blueprint again (inert: no production, no
        // field, no light link, no walkers), at the health "Rubble rebuild cost (x)" leaves to pay, with its stores
        // lost. The builders, the workers and the players build it up like any blueprint. A ROAD PIECE too (a cable
        // type: its rubble flag mirrors in GCb, with the blueprint progress). A blueprint that never stood and a
        // crossing are removed as before.
        if (!s.state->blueprint && !isCrossingType(s.type) && m_settings.rubbleCost > 0.0f)
        {
            Log::info(oc::string(structureTypeName(s.type)) + " destroyed: rubble");
            GameStructureComponent& c = *s.state;
            c.blueprint = true;
            c.health = glm::max(c.healthMax * (1.0f - glm::clamp(m_settings.rubbleCost, 0.0f, 1.0f)), 1.0f);
            if (s.type == EStructureType::AmmoStore && c.store[GoodMunitions] >= 1.0f)
                ammoBlast(s); // its munitions go up
            for (float& store : c.store)
                store = 0.0f;
            c.powered = false;
            setRubble(s, true);
            m_linksDirty = true; // what it joined is no longer joined
            ++i;
            continue;
        }
        Log::info(oc::string(structureTypeName(s.type)) + " destroyed");
        if (s.type == EStructureType::AmmoStore && !s.state->blueprint && s.state->store[GoodMunitions] >= 1.0f)
            ammoBlast(s);
        destroyStructureAt(i);
    }
}

// THE CANNONS (authority). A built Cannon is MANNED while a worker of its crew stands at the gun (Ref::crewAtGun,
// Carriers.cpp): only then it reloads (at "Crew" at the gun the full rate, with fewer the reload is that much slower),
// looks for a target and turns. Every 0.2 s it looks for the nearest enemy unit (a SHADOW too) in its arc - within "Arc (deg)" /
// 2 of its placed facing, between "Min range" and "Range" (planar) - that a LOB CAN REACH (of the 4 nearest, the first
// whose highest lob has a clear arc: planLob), and its gun turns toward it at "Turn rate (deg/s)". It fires when the gun
// points at the target (within 3 degrees), its reload is done and it holds a munition: the LOWEST lob whose whole arc is
// clear, the vertical speed solved for the gravity so the shell comes down on the target's spot. The match spawns it
// (cannonShots). With the carriers off ("Game/Carriers" "Enabled") there are no crews: every cannon is manned.
void StructureSystem::tickCannons(float deltaSec)
{
    ProfileScope scope("Structures cannons", EProfileCategory::Game);
    const float range = glm::max(m_settings.cannonRange, 1.0f);
    const float minRange = glm::clamp(m_settings.cannonMinRange, 0.0f, range * 0.9f);
    const float halfArc = glm::radians(glm::clamp(m_settings.cannonArcDeg, 1.0f, 360.0f) * 0.5f);
    const float cosHalfArc = std::cos(halfArc);
    const float gravity = glm::max(-Globals::settings.physics.gravity.y, 0.1f);
    const int crewFull = glm::max(m_settings.cannonCrew, 1);
    constexpr float c_muzzleHeight = 0.55f, c_muzzleOut = 1.65f; // the barrel's mouth over / ahead of the origin (cannon.pre's Mouth)
    constexpr float c_seekSeconds = 0.2f, c_aimTolerance = 0.05f; // (radians: ~3 degrees)
    // THE LOB to `aim` from `muzzle`. The flight time: the distance / "Shot speed", at least 1 s, and long enough for a
    // launch at least 45 degrees up (vertical >= horizontal: dy + g t^2 / 2 >= dist), so the shell climbs steeply out of
    // the muzzle (a cannon on a walkway deck has its muzzle only ~0.65 m over the wall in front). Each ATTEMPT flies 1.3x
    // longer than the one before: a higher lob that comes down steeper.
    // THE CLEARANCE: the WHOLE arc (c_clearSteps rays along the shell's lowest point: its radius under the path) against
    // the static world - walls, gates, buildings, the terrain - from its start to c_landClear metres of path before it
    // lands. The wall in front of the barrel AND the wall in front of a demon that presses against it: a steep enough lob
    // falls over it. Attempts firstAttempt .. c_attempts - 1, the lowest clear one; false = every one is blocked.
    // THE START: the shell begins c_launchAhead metres ALONG ITS OWN ARC from the mouth (the place and the speed it
    // would have there), so the wall or the ship's rail right in front of the barrel is behind it already. The arc is
    // the same: it still comes down on the target. The muzzle flash shows there (cannonShell.pre MuzzleFlash).
    constexpr int c_attempts = 5, c_clearSteps = 12;
    constexpr float c_shellRadius = 0.35f, c_launchAhead = 1.0f, c_landClear = 1.5f;
    const uint64 clearMask = PhysicsLayers::bit("Default") | PhysicsLayers::bit("Gate") | PhysicsLayers::bit("Terrain");
    struct Lob { glm::vec3 start, velocity; };
    const auto planLob = [&](const glm::vec3& muzzle, const glm::vec3& aim, int firstAttempt, Lob& out) -> bool
    {
        const glm::vec2 flat(aim.x - muzzle.x, aim.z - muzzle.z);
        const float dist = glm::length(flat);
        const float rise = aim.y - muzzle.y;
        float time = glm::max(glm::max(dist / glm::max(m_settings.cannonShotSpeed, 1.0f), 1.0f),
            std::sqrt(glm::max(2.0f * (dist - rise) / gravity, 0.0f)));
        for (int attempt = 0; attempt < firstAttempt; ++attempt)
            time *= 1.3f;
        for (int attempt = firstAttempt; attempt < c_attempts; ++attempt, time *= 1.3f)
        {
            const glm::vec2 horizontal = dist > 1e-3f ? flat / time : glm::vec2(0.0f);
            const float vertical = (rise + 0.5f * gravity * time * time) / time;
            const auto arcAt = [&](float t) { return muzzle + glm::vec3(horizontal.x * t, vertical * t - 0.5f * gravity * t * t, horizontal.y * t); };
            const float startTime = glm::min(c_launchAhead / glm::max(glm::length(glm::vec3(horizontal.x, vertical, horizontal.y)), 0.1f), time * 0.25f);
            const float landSpeed = glm::length(glm::vec3(horizontal.x, vertical - gravity * time, horizontal.y));
            const float endTime = glm::max(time - c_landClear / glm::max(landSpeed, 0.1f), startTime);
            const glm::vec3 low(0.0f, c_shellRadius, 0.0f);
            bool blocked = false;
            glm::vec3 prev = arcAt(startTime) - low;
            for (int k = 1; k <= c_clearSteps && !blocked; ++k)
            {
                const glm::vec3 next = arcAt(startTime + (endTime - startTime) * (float)k / c_clearSteps) - low;
                blocked = Globals::physics.castRayClosest(prev, next - prev, clearMask, nullptr, true).hit;
                prev = next;
            }
            if (!blocked)
            {
                out = { arcAt(startTime), glm::vec3(horizontal.x, vertical - gravity * startTime, horizontal.y) };
                return true;
            }
        }
        return false;
    };
    for (Ref& ref : m_frame)
    {
        if (ref.type != EStructureType::Cannon)
            continue;
        GameStructureComponent& s = *ref.state;
        const int manned = m_settings.carriers ? glm::min((int)ref.crewAtGun, crewFull) : crewFull;
        if (s.blueprint || manned == 0)
        {
            ref.cannonHasTarget = false;
            continue;
        }
        ref.cannonReload = glm::max(ref.cannonReload - deltaSec * (float)manned / (float)crewFull, 0.0f);
        const glm::quat facing = cannonFacingRot(ref);
        const glm::vec3 pos = ref.entity->pos;
        ref.cannonSeek -= deltaSec;
        if (ref.cannonSeek <= 0.0f)
        {
            ref.cannonSeek = c_seekSeconds;
            const glm::vec3 fwd3 = facing * glm::vec3(0.0f, 0.0f, -1.0f); // an entity's forward is -Z
            const glm::vec2 fwd = glm::length(glm::vec2(fwd3.x, fwd3.z)) > 1e-4f ? glm::normalize(glm::vec2(fwd3.x, fwd3.z)) : glm::vec2(0.0f, -1.0f);
            // The c_candidates NEAREST enemy units in the arc (kept sorted while the query streams them), then the nearest
            // of those a lob can reach: its HIGHEST lob clear (planLob's last attempt; the fire step picks the lowest
            // clear one). None: no target - the gun holds rather than fire into its own wall.
            constexpr int c_candidates = 4;
            struct Candidate { float distSq; Entity* entity; };
            Candidate nearest[c_candidates];
            int count = 0;
            Globals::spatialIndex.forEachInSphere(glm::dvec3(pos), range, SpatialLayer_Render, [&](uint64 user)
            {
                Entity* other = reinterpret_cast<Entity*>(user);
                const GameUnitComponent* u = getComponent<GameUnitComponent>(other);
                if (!u || u->puppet || u->team == s.team || !u->alive()) // (a shadow too: the shell hurts it - HitsShadows)
                    return;
                const glm::vec2 d(other->pos.x - pos.x, other->pos.z - pos.z);
                const float distSq = glm::dot(d, d);
                if (distSq >= range * range || distSq < minRange * minRange)
                    return;
                if (glm::dot(d, fwd) < cosHalfArc * std::sqrt(distSq))
                    return; // outside the arc
                if (count == c_candidates && distSq >= nearest[c_candidates - 1].distSq)
                    return;
                int k = glm::min(count, c_candidates - 1);
                count = glm::min(count + 1, c_candidates);
                for (; k > 0 && nearest[k - 1].distSq > distSq; --k)
                    nearest[k] = nearest[k - 1];
                nearest[k] = { distSq, other };
            });
            Entity* target = nullptr;
            for (int k = 0; k < count && !target; ++k)
            {
                const glm::vec3 to = nearest[k].entity->pos - pos;
                const glm::vec2 dir = glm::length(glm::vec2(to.x, to.z)) > 1e-4f ? glm::normalize(glm::vec2(to.x, to.z)) : fwd;
                const glm::vec3 muzzle = pos + glm::vec3(dir.x * c_muzzleOut, c_muzzleHeight, dir.y * c_muzzleOut);
                Lob lob;
                if (planLob(muzzle, nearest[k].entity->pos, c_attempts - 1, lob))
                    target = nearest[k].entity;
            }
            ref.cannonHasTarget = target != nullptr;
            if (target)
            {
                ref.cannonTargetPos = target->pos;
                const glm::vec3 local = glm::inverse(facing) * (target->pos - pos); // the bearing in the facing's frame
                ref.cannonAimGoal = glm::clamp(std::atan2(-local.x, -local.z), -halfArc, halfArc);
            }
        }
        turnCannon(ref, deltaSec);
        if (!ref.cannonHasTarget || ref.cannonReload > 0.0f || s.store[GoodMunitions] < 1.0f
            || glm::abs(ref.cannonAimGoal - ref.cannonAim) > c_aimTolerance)
            continue;
        const glm::vec3 aimed3 = ref.entity->rot * glm::vec3(0.0f, 0.0f, -1.0f); // the gun as it points now
        const glm::vec2 fwd = glm::length(glm::vec2(aimed3.x, aimed3.z)) > 1e-4f ? glm::normalize(glm::vec2(aimed3.x, aimed3.z)) : glm::vec2(0.0f, -1.0f);
        const glm::vec3 muzzle = pos + glm::vec3(fwd.x * c_muzzleOut, c_muzzleHeight, fwd.y * c_muzzleOut);
        // The lowest lob whose whole arc is clear (planLob). None (the target moved behind a wall since the search): hold
        // the shot and look again at once - the search takes the next target a lob can reach.
        Lob lob;
        if (!planLob(muzzle, ref.cannonTargetPos, 0, lob))
        {
            ref.cannonSeek = 0.0f;
            continue;
        }
        m_cannonShots.push_back({ lob.start, lob.velocity, s.team });
        s.store[GoodMunitions] -= 1.0f;
        ref.cannonReload = glm::max(m_settings.cannonFireInterval, 0.1f);
    }
}

// A Cannon's stand in the cell of `snappedGround`: the top of a road piece (the Road, the Bridge) or of a walkway DECK
// there. False = nothing to stand on: the ground.
bool StructureSystem::cannonStandY(const glm::vec3& snappedGround, float& outY) const
{
    const int cx = (int)std::floor(snappedGround.x / GridCellSize), cz = (int)std::floor(snappedGround.z / GridCellSize);
    const auto it = m_cells.find(cellKey(cx, cz));
    const int index = it != m_cells.end() ? structureIndexById(it->second.id) : -1;
    if (index < 0)
        return false;
    const Ref& r = m_frame[index];
    if (r.type == EStructureType::Walkway && r.walkDir < 0) // the deck (or a walkway blueprint: its deck to come)
    {
        outY = r.entity->pos.y - spawnHeightOf(r.type) + WalkwayDeckHeight;
        return true;
    }
    if (r.type == EStructureType::CableConveyor || r.type == EStructureType::Bridge)
    {
        outY = r.entity->pos.y - spawnHeightOf(r.type) + 0.05f;
        return true;
    }
    return false;
}

// THE AMMO STORE'S BLAST: a store destroyed with munitions in it explodes. Radius and damage scale with what it held
// (a full store: "Blast radius (m)" and "Blast damage"; the radius from 40 % up, the damage from 25 % up), and fall off
// linearly to half at the edge. It hurts units of EVERY team (player capsules through their damage inbox) and every
// building in it, its own team's too. Authority only (it runs in the death sweep); the match shows the flash.
void StructureSystem::ammoBlast(const Ref& s)
{
    const float fill = glm::clamp(s.state->store[GoodMunitions] / glm::max(s.state->capacity[GoodMunitions], 1.0f), 0.0f, 1.0f);
    const float radius = m_settings.ammoBlastRadius * (0.4f + 0.6f * std::sqrt(fill));
    const float damage = m_settings.ammoBlastDamage * (0.25f + 0.75f * fill);
    if (radius <= 0.1f || damage <= 0.0f)
        return;
    const glm::vec3 centre = s.entity->pos;
    const auto falloff = [&](const glm::vec3& p)
    {
        const float d = glm::distance(glm::vec2(p.x, p.z), glm::vec2(centre.x, centre.z));
        return d > radius ? 0.0f : 1.0f - 0.5f * d / radius;
    };
    Globals::spatialIndex.forEachInSphere(glm::dvec3(centre), radius, SpatialLayer_Render, [&](uint64 user)
    {
        Entity* other = reinterpret_cast<Entity*>(user);
        if (GameUnitComponent* u = getComponent<GameUnitComponent>(other); u && u->alive())
            if (const float k = falloff(other->pos); k > 0.0f)
                u->damage(damage * k);
    });
    for (const Ref& r : m_frame)
        if (r.state != s.state && !isCableOrCrossing(r.type))
            if (const float k = falloff(r.entity->pos); k > 0.0f)
                r.state->damage(damage * k);
    m_blasts.push_back({ centre, radius });
    Log::info(oc::format("Ammo store blew up: {:.0f} damage within {:.1f} m", damage, radius));
}

// The rubble flag and what follows from it on every instance: the COLLIDER - rubble is walked over (a body-command:
// off the broadphase and the solver, pose kept), or the enemy could neither bite it nor pass it - and THE LOOK: the
// building's own meshes are hidden (the root's node pass mask to 0, its child pieces disabled) and a rubble PILE
// stands on its ground point - rubble1.pre for a 1x1 tile building, rubble2.pre for a 2x2 one, the same at scale 1.5
// for a 3x3 one. The pile is a root entity of its own, owned by the roster entry.
void StructureSystem::setRubble(const Ref& s, bool rubble)
{
    if (rubble && !s.state->rubble)
        GameSound::post(EGameSound::StructureCollapse, s.entity->pos, 1.0f, (float)footprintCellsOf(s.type));
    s.state->rubble = rubble ? 1 : 0;
    s.rubbleUnsafe = rubble; // a fresh pile is unsafe until refreshRubbleSafety looks (its destroyer stands next to it)
    applyStructureTint(s);
    const bool aloft = s.onShip && !m_shipJoined; // its collider is the ship's box (shipColliderBoxes): its own stays off
    if (const PhysicsComponent* pc = getComponent<PhysicsComponent>(s.entity))
        Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetEnabled, glm::vec3(rubble || aloft ? 0.0f : 1.0f));
    if (s.onShip)
        ++m_shipRevision;
    RenderComponent* rc = getComponent<RenderComponent>(s.entity);
    if (rubble && !s.rubbleVisual)
    {
        if (rc && rc->node.isValid())
        {
            s.rubbleSavedPassMask = rc->node.getPassMask();
            rc->node.setPassMask(0);
        }
        const int cells = footprintCellsOf(s.type);
        const glm::vec3 ground = s.entity->pos - glm::vec3(0.0f, spawnHeightOf(s.type), 0.0f);
        // (a road piece: broken paving, rubbleRoad.pre - no pile to climb)
        s.rubbleVisual = Globals::world.spawnAssetFile(isCableType(s.type) ? "Entities/Game/rubbleRoad.pre"
            : cells <= 1 ? "Entities/Game/rubble1.pre" : "Entities/Game/rubble2.pre",
            Transform(ground, cells >= 3 ? 1.5f : 1.0f, s.entity->rot), true);
        if (s.rubbleVisual)
            Globals::world.addRootEntity(s.rubbleVisual);
    }
    else if (!rubble && s.rubbleVisual)
    {
        if (rc && rc->node.isValid())
            rc->node.setPassMask(s.rubbleSavedPassMask);
        Globals::world.removeRootEntity(s.rubbleVisual.get());
        s.rubbleVisual = EntityPtr();
    }
    if (const SceneComponent* scene = getComponent<SceneComponent>(s.entity))
        for (const EntityPtr& child : scene->children)
            child->setEnabled(!rubble);
    if (!rubble && s.type == EStructureType::ShipRail)
        applyRailSides(s); // only the sides that face off the deck
    if (s.type == EStructureType::ShipRail)
        buildRailBody(s); // rubble: no collider; built again: its pieces
}

void StructureSystem::tickConstructors(float deltaSec)
{
    ProfileScope scope("Structures constructors", EProfileCategory::Game);
    // A powered Constructor invests its conveyor-fed mineral stock into the nearest own-team
    // blueprint OR damaged structure in range; with nothing to build or repair it idles.
    for (const Ref& ref : m_frame)
    {
        GameStructureComponent& s = *ref.state;
        if (s.blueprint || ref.type != EStructureType::Constructor || !s.powered || s.store[2] <= 0.0f)
            continue;
        const float staff = staffing((uint8)s.team);
        const float budget = glm::min(m_settings.constructorBuildRate * staff * deltaSec, s.store[2]);
        s.store[2] -= fundNearbyBlueprint(ref.entity->pos, m_settings.constructorRange, (uint8)s.team, budget, true,
            m_settings.constructorRepairRate * staff * deltaSec);
    }
}

// ---------------------------------------------------------------- debug draw

// buildMode: the player's build UI is open - the builders' and the towers' ranges show. armed: the type on the ghost
// (Count = none) - the harvest ranges of the built yards show only while a yard of THAT kind is on the ghost, the light
// hubs' link ranges only while a type that uses light is, the Healer's huts' reach only while a Healer's hut is, and the
// armories' post range only while an Armory or a Mage tower is. selectedId: the selected structure (0 = none) - a
// Healer's hut shows its reach, an armory (the Beacon too) its post range while it is selected.
void StructureSystem::drawDebug(bool buildMode, EStructureType armed, uint32 selectedId) const
{
    ProfileScope scope("Structures debug draw", EProfileCategory::Game);
    // SATURATED segments: a red pulsing ring over every segment whose ~2 s average throughput
    // sits at or above 90 % of its out-rate - the bottlenecks, and nothing else (the segments
    // themselves are real meshes; the selected-cable label carries the numbers). Clients read the
    // mirrored average (GCf), so they see the same rings.
    for (const TransportNode& node : m_net.nodes)
    {
        if (node.junction || node.movedAvg < 0.9f * m_settings.cableThroughput[glm::min((int)node.medium, 2)])
            continue;
        const int index = structureIndexById(node.structureId);
        if (index < 0)
            continue;
        // Loud on purpose: a bright red double ring floating well above the cable, breathing in
        // size, plus a spike up from the segment so it reads from any camera angle.
        const float pulse = 0.5f + 0.5f * std::sin(m_time * 5.0f);
        const uint32 color = packColor(glm::vec3(1.0f, 0.15f, 0.1f) * (0.8f + 0.2f * pulse));
        const glm::vec3 base = m_frame[index].entity->pos;
        const glm::vec3 top = base + glm::vec3(0.0f, 0.9f, 0.0f);
        drawCircle(top, 0.4f + 0.12f * pulse, color, 14);
        drawCircle(top, 0.32f + 0.12f * pulse, color, 14);
        drawCircle(top, 0.24f + 0.12f * pulse, color, 12);
        Globals::rendererVK.addDebugLine(base + glm::vec3(0.0f, 0.3f, 0.0f), top, color);
    }

    // Node rings: blue = mineral, orange = fuel.
    for (const Node& node : m_nodes)
    {
        const glm::vec3 base = node.type == ENodeType::Mineral
            ? glm::vec3(0.2f, 0.4f, 1.0f) : glm::vec3(1.0f, 0.6f, 0.15f);
        drawCircle(glm::vec3(node.pos.x, node.pos.y - c_nodeHeight + 0.3f, node.pos.z), 3.0f, packColor(base), 24);
    }
    // The harvest range of the built yards of the kind on the ghost (where a new one would share with them): green =
    // a lumberyard, grey = a quarry, dimmer with a lower yield.
    for (const Ref& s : m_frame)
        if (isHarvestType(s.type) && s.type == armed && !s.state->blueprint)
            drawCircle(s.entity->pos - glm::vec3(0.0f, spawnHeightOf(s.type) - 0.3f, 0.0f), m_settings.harvestRadius,
                packColor((s.type == EStructureType::Lumberyard ? glm::vec3(0.35f, 0.8f, 0.3f) : glm::vec3(0.7f, 0.7f, 0.75f))
                    * glm::mix(0.35f, 0.8f, s.yield)), 48);

    // THE LIGHT HUBS' LINK RANGE, while a type that uses light is on the ghost: a gold ring around each built Crystal
    // store, the Beacon and each Lens, at the distance the armed type links over (the same choice as
    // rebuildLightLinks: a Lens end is never longer than "Lens range"). Inside a ring the new building gets a link,
    // if the line between the two crystals is clear.
    if (m_settings.lightBeams && armed != EStructureType::Count && !isCableOrCrossing(armed) && energyCapacityOf(armed) > 0.0f)
    {
        const float own = armed == EStructureType::Lens ? m_settings.lensRange
            : armed == EStructureType::Solar ? m_settings.mirrorBeamRange
            : transportRoleOf(armed, 0) == ETransportRole::Storage ? m_settings.lightHubLinkRange : m_settings.lightHubReach;
        const uint32 hubRing = packColor(glm::vec3(1.0f, 0.85f, 0.4f) * 0.7f);
        for (const Ref& s : m_frame)
        {
            const bool hub = s.type == EStructureType::Battery || s.type == EStructureType::Base || s.type == EStructureType::Lens;
            if (!hub || s.state->blueprint)
                continue;
            const float range = s.type == EStructureType::Lens ? glm::min(own, m_settings.lensRange) : own;
            drawCircle(s.entity->pos - glm::vec3(0.0f, spawnHeightOf(s.type) - 0.35f, 0.0f), range, hubRing, 64);
        }
    }

    // In build mode only: constructor build/repair reach (amber), light tower and searchlight range (pale blue).
    // The Healer's hut's heal reach (green): while it is selected, or while a Healer's hut is on the ghost (every built
    // one, so the new one's place shows against them). Always: red rings on unpowered consumers.
    const uint32 constructorRing = packColor(glm::vec3(1.0f, 0.75f, 0.45f) * 0.6f);
    const uint32 towerRing = packColor(glm::vec3(0.6f, 0.8f, 1.0f) * 0.5f);
    const uint32 medicRing = packColor(glm::vec3(0.4f, 1.0f, 0.5f) * 0.6f);
    const uint32 postRing = packColor(glm::vec3(1.0f, 0.45f, 0.35f) * 0.7f);
    const uint32 unpoweredColor = packColor(glm::vec3(1.0f, 0.25f, 0.2f));
    const GameStructureParams& params = Globals::settings.game.structureParams;
    for (const Ref& s : m_frame)
    {
        const glm::vec3 ground = s.entity->pos - glm::vec3(0.0f, spawnHeightOf(s.type), 0.0f);
        if (buildMode && s.type == EStructureType::Constructor && !s.state->blueprint)
            drawCircle(ground + glm::vec3(0.0f, 0.4f, 0.0f), m_settings.constructorRange, constructorRing, 40);
        if (buildMode && (s.type == EStructureType::Turret || s.type == EStructureType::Searchlight) && !s.state->blueprint)
            drawCircle(ground + glm::vec3(0.0f, 0.4f, 0.0f),
                s.type == EStructureType::Turret ? params.turretRange : params.searchlightRange, towerRing, 40);
        if (s.type == EStructureType::MedicStation && !s.state->blueprint && s.state->powered
            && (armed == EStructureType::MedicStation || (selectedId != 0 && s.state->structureId == selectedId)))
            drawCircle(ground + glm::vec3(0.0f, 0.4f, 0.0f), medicHealRadius(), medicRing, 40);
        // An armory's POST RANGE (the Armory, the Mage tower and the Beacon, which is an armory too): while it is
        // selected, or while an Armory or a Mage tower is on the ghost (every built one, so the new one's place shows
        // against them).
        if ((s.type == EStructureType::Armory || s.type == EStructureType::MageTower || s.type == EStructureType::Base)
            && !s.state->blueprint
            && (armed == EStructureType::Armory || armed == EStructureType::MageTower
                || (selectedId != 0 && s.state->structureId == selectedId)))
            drawCircle(ground + glm::vec3(0.0f, 0.4f, 0.0f), m_settings.armoryPostRange, postRing, 48);
        if ((hasShieldEmitter(s.type) || s.type == EStructureType::MedicStation
            || s.type == EStructureType::Fabricator) && !s.state->powered && !s.state->blueprint)
            drawCircle(ground + glm::vec3(0.0f, 0.3f, 0.0f), 1.0f, unpoweredColor, 16);
    }
}
