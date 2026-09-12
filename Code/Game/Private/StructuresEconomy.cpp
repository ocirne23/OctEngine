module Game;

import Core;
import Core.glm;
import Core.Log;
import Entity;
import Force;
import RendererVK;
import :Structures;

// THE ECONOMY TICKS + materials (see Structures.ixx): production (income, fuel burn, consumer
// drain, the emitter ramps - over the roster, the transport moves the cells between them), the
// death sweep, the constructors, the shared materials model (HEALTH IS THE PROGRESS: the same
// price builds a blueprint and repairs a damaged structure), the tint, and the debug draw.

static constexpr glm::vec3 c_blueprintColor(0.45f, 0.55f, 0.7f); // ghost tint until built

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
    glm::vec3 color = c_blueprintColor;
    if (!s.state->blueprint)
    {
        color = glm::vec3(1.0f); // fallback: authored tint missing
        if (const RenderComponent::SpawnInfo* info = getRenderSpawnInfo(s.entity); info && info->color.x >= 0.0f)
            color = info->color;
        // The three crossings share one authored-gray prefab: the type's medium hues it.
        if (isCrossingType(s.type))
            color = mediumHue(crossingMediumOf(s.type));
    }
    const auto material = Globals::rendererVK.createSolidColorMaterial(color);
    rc->node.setMaterialOverride(material);
    // Cables/crossings are COMPOSITES (arm/end/ramp child entities): tint the child pieces along -
    // this is also what turns them blueprint-gray and back.
    if (isCableOrCrossing(s.type))
        if (const SceneComponent* sc = getComponent<SceneComponent>(s.entity))
            for (const EntityPtr& child : sc->children)
                if (RenderComponent* crc = getComponent<RenderComponent>(child.get());
                    crc && crc->node.isValid())
                    crc->node.setMaterialOverride(material);
}

float StructureSystem::investMaterials(const Ref& s, float amount)
{
    // HEALTH IS THE PROGRESS: materials heal at cost/healthMax per hp - the same price builds a
    // blueprint and repairs a damaged structure. A full-health blueprint flips to BUILT. Heals
    // against the COMPONENT's healthMax (cables are softer than buildings).
    if (amount <= 0.0f || (!isPlaceableType(s.type) && s.type != EStructureType::Base))
        return 0.0f; // the Base is never placed, but it repairs at its m_costs entry like the rest
    const float healthMax = glm::max(s.state->healthMax, 1e-3f);
    const float materialsPerHp = glm::max(m_costs[(int)s.type], 0.01f) / healthMax;
    const float heal = glm::min(amount / materialsPerHp, healthMax - s.state->health);
    if (heal <= 0.0f)
        return 0.0f;
    s.state->health += heal;
    if (s.state->blueprint && s.state->health >= healthMax - 1e-3f)
    {
        s.state->blueprint = false;
        applyStructureTint(s); // back to the authored color
        m_linksDirty = true;   // a finished cable segment starts conducting
        Log::info(oc::string(structureTypeName(s.type)) + " constructed");
        if (onStructureBuilt) // server: re-fire GPl - cables are outside GSt, so this IS the
            onStructureBuilt(s.state->structureId); // client's only built notification
    }
    return heal * materialsPerHp;
}

float StructureSystem::takeStoredMinerals(const glm::vec3& pos, float radius, uint8 team, float amount)
{
    // Silos, the Base, AND mineral extractors: a player standing at a fresh dig site can courier
    // its buffer by hand before any conveyor exists (a fuel extractor's mineral store stays 0, so
    // including the type unconditionally is safe).
    const auto holdsMinerals = [](EStructureType t) {
        return t == EStructureType::MineralSilo || t == EStructureType::Base
            || t == EStructureType::Extractor; };
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
    bool includeRepairs)
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
        if (!wantsMaterials)
            continue;
        const float dist = glm::distance(glm::vec2(s.entity->pos.x, s.entity->pos.z), glm::vec2(pos.x, pos.z));
        if (dist < bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }
    return best >= 0 ? investMaterials(m_frame[best], amount) : 0.0f;
}

// ---------------------------------------------------------------- production + sweeps

void StructureSystem::tickProduction(float deltaSec)
{
    ProfileScope scope("Structures production", EProfileCategory::Game);
    const float dt = glm::max(deltaSec, 1e-6f);
    const auto consumerDraw = [&](EStructureType type) {
        return type == EStructureType::Emitter ? m_emitterEnergyPerSec
             : type == EStructureType::Extractor ? m_extractorEnergyPerSec
             : type == EStructureType::Constructor ? m_extractorEnergyPerSec // powered while building
             : type == EStructureType::MedicStation ? m_medicEnergyPerSec
             : type == EStructureType::Fabricator ? m_fabricatorEnergyPerSec : 0.0f;
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

        // ---- income: powered extractors fill their OWN buffer/tank (full = production stalls),
        // the Base trickles minerals into its bank, fabricators convert (energy+fuel -> minerals).
        if (ref.type == EStructureType::Extractor && s.powered && ref.nodeIndex >= 0
            && ref.nodeIndex < (int)m_nodes.size())
        {
            if (m_nodes[ref.nodeIndex].type == ENodeType::Fuel)
                s.store[1] = glm::min(s.store[1] + m_fuelRate * dt, s.capacity[1]);
            else
                s.store[2] = glm::min(s.store[2] + m_mineralRate * dt, s.capacity[2]);
        }
        else if (ref.type == EStructureType::Fabricator && s.powered)
            s.store[2] = glm::min(s.store[2] + m_fabricatorMineralsPerSec * dt, s.capacity[2]);
        else if (ref.type == EStructureType::Base)
            s.store[2] = glm::min(s.store[2] + m_mineralRate * m_baseIncomeMult * dt, s.capacity[2]);

        // ---- producers: generators burn their OWN tank into their OWN buffer (full buffer =
        // export-limited = no fuel burn), solar trickles for free. The Base self-generates the
        // same way into its own store - a baseline its shield draw eats from; sieges outpace it.
        if (ref.type == EStructureType::Base && m_baseEnergyGenPerSec > 0.0f)
        {
            const float add = glm::min(m_baseEnergyGenPerSec * dt, glm::max(s.capacity[0] - s.store[0], 0.0f));
            s.store[0] += add;
            if (dt > 1e-9f) // paused (sim dt 0): 0/0 would put a NaN into the HUD's gen rate
                m_genRateTotal += add / dt;
        }
        else if (ref.type == EStructureType::Solar)
        {
            const float add = glm::min(m_solarEnergyPerSec * dt, glm::max(s.capacity[0] - s.store[0], 0.0f));
            s.store[0] += add;
            if (dt > 1e-9f)
                m_genRateTotal += add / dt;
        }
        else if (ref.type == EStructureType::Generator && m_genEnergyPerSec > 0.0f)
        {
            float want = glm::min(m_genEnergyPerSec * dt, glm::max(s.capacity[0] - s.store[0], 0.0f));
            if (want > 0.0f)
            {
                const float fuelNeeded = m_fuelBurnRate * want / m_genEnergyPerSec;
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
            // THE BASE'S SHIELD IS FREE - no per-second draw, no pressure surcharge: the Base is a
            // storage building (it banks energy for the grid and self-generates a trickle). Only
            // the enemy siege load still drains it, so a pressed Base can still go dark.
            const bool freeShield = ref.type == EStructureType::Base;
            const float draw = (freeShield ? 0.0f : emitterDrawOf(ref.type)
                    + pressure * (1.0f + m_pressureDrawTension * pressure) * m_emitterPressureDraw)
                + s.emitter.unitLoad; // enemy units/shots leaning on the bubble
            s.emitter.unitLoad = 0.0f;
            totalDemand += draw;
            if (s.emitter.down && s.store[0] >= glm::min(m_emitterRestartCharge, energyCapacityOf(ref.type)))
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
            const float rampTime = glm::max(target > s.emitter.outputFrac ? m_emitterGrowTime : m_emitterShrinkTime, 0.01f);
            s.emitter.outputFrac = glm::clamp(s.emitter.outputFrac
                + (target > s.emitter.outputFrac ? dt : -dt) / rampTime, 0.0f, 1.0f);
            if (fc)
            {
                // NO sentinel: a dark emitter's field is fully gone.
                fc->emitter.setOutput(emitterOutputOf(ref.type) * s.emitter.outputFrac);
                fc->emitter.setReach(emitterReachOf(ref.type));
            }
            continue;
        }
        const float draw = consumerDraw(ref.type);
        if (draw <= 0.0f)
            continue;
        totalDemand += draw;
        // Fabricators burn fuel alongside energy (piped into their own tank) - both must be there.
        const float fuelDraw = ref.type == EStructureType::Fabricator ? m_fabricatorFuelPerSec : 0.0f;
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
        Log::info(oc::string(structureTypeName(s.type)) + " destroyed");
        destroyStructureAt(i);
    }
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
        const float budget = glm::min(m_constructorBuildRate * deltaSec, s.store[2]);
        s.store[2] -= fundNearbyBlueprint(ref.entity->pos, m_constructorRange, (uint8)s.team, budget, true);
    }
}

// ---------------------------------------------------------------- debug draw

void StructureSystem::drawDebug() const
{
    ProfileScope scope("Structures debug draw", EProfileCategory::Game);
    // SATURATED segments: a red pulsing ring over every segment whose ~2 s average throughput
    // sits at or above 90 % of its out-rate - the bottlenecks, and nothing else (the segments
    // themselves are real meshes; the selected-cable label carries the numbers). Clients read the
    // mirrored average (GCf), so they see the same rings.
    for (const TransportNode& node : m_net.nodes)
    {
        if (node.junction || node.movedAvg < 0.9f * m_cableThroughput[glm::min((int)node.medium, 2)])
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
        drawCircle(glm::vec3(node.pos.x, 0.3f, node.pos.z), m_extractorSnapRadius, packColor(base), 24);
    }

    // Constructor build/repair reach (amber), medic heal reach (green) + red rings on unpowered
    // consumers.
    const uint32 constructorRing = packColor(glm::vec3(1.0f, 0.75f, 0.45f) * 0.6f);
    const uint32 medicRing = packColor(glm::vec3(0.4f, 1.0f, 0.5f) * 0.6f);
    const uint32 unpoweredColor = packColor(glm::vec3(1.0f, 0.25f, 0.2f));
    for (const Ref& s : m_frame)
    {
        if (s.type == EStructureType::Constructor && !s.state->blueprint)
            drawCircle(glm::vec3(s.entity->pos.x, 0.4f, s.entity->pos.z), m_constructorRange, constructorRing, 40);
        if (s.type == EStructureType::MedicStation && !s.state->blueprint && s.state->powered)
            drawCircle(glm::vec3(s.entity->pos.x, 0.4f, s.entity->pos.z), medicHealRadius(), medicRing, 40);
        if ((hasShieldEmitter(s.type) || s.type == EStructureType::Extractor
            || s.type == EStructureType::Constructor || s.type == EStructureType::MedicStation
            || s.type == EStructureType::Fabricator) && !s.state->powered && !s.state->blueprint)
            drawCircle(glm::vec3(s.entity->pos.x, 0.3f, s.entity->pos.z), 1.0f, unpoweredColor, 16);
    }
}
