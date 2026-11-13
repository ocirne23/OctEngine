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
// and the searches for work skip it (blueprintNeed, fundNearbyBlueprint).
bool StructureSystem::rubbleBlocked(const Ref& s) const
{
    if (!s.state->rubble)
        return false;
    // A BODY ON THE PILE blocks it too (a player or a unit of any team): rubble is walked over, and the building
    // that comes back would close around whoever stands there - nothing moves a body out of a building.
    if (actorInFootprint(s.type, s.entity->pos - glm::vec3(0.0f, spawnHeightOf(s.type), 0.0f)))
        return true;
    if (m_settings.rubbleSafeRadius <= 0.0f)
        return false;
    const float radius = m_settings.rubbleSafeRadius;
    const glm::vec3 pos = s.entity->pos;
    const uint32 team = s.state->team;
    bool enemy = false;
    Globals::spatialIndex.forEachInSphere(glm::dvec3(pos), radius, SpatialLayer_Render, [&](uint64 user)
    {
        Entity* other = reinterpret_cast<Entity*>(user);
        const GameUnitComponent* unit = getComponent<GameUnitComponent>(other);
        if (enemy || !unit || unit->puppet || unit->team == team || !unit->alive())
            return;
        const glm::vec2 d(other->pos.x - pos.x, other->pos.z - pos.z);
        enemy = glm::dot(d, d) <= radius * radius;
    });
    return enemy;
}

float StructureSystem::investMaterials(const Ref& s, float amount, float repairHealCap)
{
    if (rubbleBlocked(s))
        return 0.0f;
    // HEALTH IS THE PROGRESS: materials heal at cost/healthMax per hp - the same price builds a
    // blueprint and repairs a damaged structure. A full-health blueprint flips to BUILT. Heals
    // against the COMPONENT's healthMax (cables are softer than buildings).
    if (amount <= 0.0f || (!isPlaceableType(s.type) && s.type != EStructureType::Base))
        return 0.0f; // the Base is never placed, but it repairs at its m_settings.costs entry like the rest
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
    bool includeRepairs, float repairHealCap)
{
    if (amount <= 0.0f)
        return 0.0f;
    int best = -1;
    float bestDist = radius;
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        const Ref& s = m_frame[i];
        if (s.state->team != team)
            continue;
        // The Base is never a blueprint, but it IS repairable now that it takes damage.
        const bool wantsMaterials = s.state->blueprint
            || (includeRepairs && s.state->health < s.state->healthMax - 1e-3f);
        if (!wantsMaterials || rubbleBlocked(s)) // (rubble with an enemy near takes nothing: the next one gets the stone)
            continue;
        const float dist = glm::distance(glm::vec2(s.entity->pos.x, s.entity->pos.z), glm::vec2(pos.x, pos.z));
        if (dist < bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }
    return best >= 0 ? investMaterials(m_frame[best], amount, repairHealCap) : 0.0f;
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
    const auto consumerDraw = [&](EStructureType type) {
        return type == EStructureType::Emitter ? m_settings.emitterEnergyPerSec
             : type == EStructureType::MedicStation ? m_settings.medicEnergyPerSec : 0.0f;
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
            const float target = paid ? 1.0f : 0.0f;
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
        // The HUD's "energy stored" counts DEDICATED storage only (Battery) - production buffers
        // and consumer trickle buffers are working charge, not reserves.
        if (s.type == EStructureType::Battery)
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
            c.store[0] = c.store[1] = c.store[2] = 0.0f;
            c.powered = false;
            setRubble(s, true);
            m_linksDirty = true; // what it joined is no longer joined
            ++i;
            continue;
        }
        Log::info(oc::string(structureTypeName(s.type)) + " destroyed");
        destroyStructureAt(i);
    }
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
    applyStructureTint(s);
    if (const PhysicsComponent* pc = getComponent<PhysicsComponent>(s.entity))
        Globals::physics.queueBodyCommand(pc->body, PhysicsWorld::EBodyCommand::SetEnabled, glm::vec3(rubble ? 0.0f : 1.0f));
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
// (Count = none) - the harvest ranges of the built yards show only while a yard of THAT kind is on the ghost, and
// the light hubs' link ranges only while a type that uses light is.
void StructureSystem::drawDebug(bool buildMode, EStructureType armed) const
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
    // Always: medic heal reach (green) + red rings on unpowered consumers.
    const uint32 constructorRing = packColor(glm::vec3(1.0f, 0.75f, 0.45f) * 0.6f);
    const uint32 towerRing = packColor(glm::vec3(0.6f, 0.8f, 1.0f) * 0.5f);
    const uint32 medicRing = packColor(glm::vec3(0.4f, 1.0f, 0.5f) * 0.6f);
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
        if (s.type == EStructureType::MedicStation && !s.state->blueprint && s.state->powered)
            drawCircle(ground + glm::vec3(0.0f, 0.4f, 0.0f), medicHealRadius(), medicRing, 40);
        if ((hasShieldEmitter(s.type) || s.type == EStructureType::MedicStation
            || s.type == EStructureType::Fabricator) && !s.state->powered && !s.state->blueprint)
            drawCircle(ground + glm::vec3(0.0f, 0.3f, 0.0f), 1.0f, unpoweredColor, 16);
    }
}
