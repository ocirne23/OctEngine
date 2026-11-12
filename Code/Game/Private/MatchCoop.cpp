module Game;

import Core;
import Core.glm;
import Core.Log;
import Entity;
import Network;
import Nav;
import RendererVK; // the beacon's beam (debug lines + a point light)
import Threading; // the ambient wander rides the post-update batch
import File; // AssetNode (the trickle save state)
import :Match;
import :Structures;
import :Npc;

// ---- CO-OP director: waves, the spawn trickle, the ambient scatter and its wander - authority only (the state: Match.ixx CO-OP) ----

// Wave ARCHETYPES: every wave rolls ONE recipe - a named mix of up to 4 unit types - gated by the
// wave index so early waves stay simple and the heavy stuff unlocks over time. Single-type rushes
// and combined-arms mixes both happen, but never every type in every wave; queueWave jitters the
// picked recipe's weights so two waves of the same archetype still differ.
namespace
{
    struct WaveArchetype
    {
        const char* name;
        int minWave; // first wave index this recipe can roll (m_waveIndex is 1-based at roll time)
        struct { ENpcType type; float weight; } mix[5]; // weight 0 = unused slot
        // THE FIRST NIGHT (the sandbox world, where the recipes unlock by night): the wave of that night (0-based)
        // from which this recipe can roll already, under its minWave; -1 = not before its minWave. So the first
        // night is not swarm after swarm: its first wave is the plain swarm, and the light mixes come in after it.
        int firstNight = -1;
    };
    // The ELITE tier (Elite/Giant/Titan/Lobber) enters at wave 7 and dominates from ~10 on: the
    // Lobber's splash shells punish packed defences, the Giant/Titan soak turret fire, and the
    // budget growth is what lets a late wave afford a Titan next to its escort.
    constexpr WaveArchetype c_waveArchetypes[] = {
        { "swarm",           1, { { ENpcType::Swarm, 1.0f } } },
        { "swarm + runners", 2, { { ENpcType::Swarm, 0.75f }, { ENpcType::Runner, 0.25f } }, 1 },
        { "grunt push",      2, { { ENpcType::Grunt, 0.65f }, { ENpcType::Swarm, 0.35f } }, 1 },
        { "skirmish",        2, { { ENpcType::Swarm, 0.5f }, { ENpcType::Runner, 0.25f },
                                  { ENpcType::Grunt, 0.25f } }, 2 },
        { "runner rush",     3, { { ENpcType::Runner, 1.0f } }, 3 },
        { "spitter probe",   3, { { ENpcType::Spitter, 0.15f }, { ENpcType::Swarm, 0.85f } } }, // no spitter on the first night
        { "warrior scouts",  3, { { ENpcType::Warrior, 0.2f }, { ENpcType::Swarm, 0.8f } }, 4 },
        { "warrior line",    4, { { ENpcType::Warrior, 0.4f }, { ENpcType::Grunt, 0.3f },
                                  { ENpcType::Swarm, 0.3f } } },
        { "shield wall",     6, { { ENpcType::Warrior, 0.6f }, { ENpcType::Spitter, 0.15f },
                                  { ENpcType::Swarm, 0.25f } } },
        { "spitter siege",   4, { { ENpcType::Spitter, 0.3f }, { ENpcType::Swarm, 0.7f } } },
        { "brute hammer",    5, { { ENpcType::Brute, 0.25f }, { ENpcType::Swarm, 0.75f } } },
        { "combined arms",   6, { { ENpcType::Grunt, 0.3f }, { ENpcType::Runner, 0.25f },
                                  { ENpcType::Spitter, 0.2f }, { ENpcType::Swarm, 0.25f } } },
        { "brute wall",      7, { { ENpcType::Brute, 0.85f }, { ENpcType::Spitter, 0.15f } } },
        { "elite guard",     7, { { ENpcType::Elite, 0.45f }, { ENpcType::Grunt, 0.25f },
                                  { ENpcType::Swarm, 0.3f } } },
        { "the works",       8, { { ENpcType::Swarm, 0.4f }, { ENpcType::Runner, 0.25f },
                                  { ENpcType::Spitter, 0.15f }, { ENpcType::Brute, 0.2f } } },
        { "lobber barrage",  8, { { ENpcType::Lobber, 0.3f }, { ENpcType::Elite, 0.2f },
                                  { ENpcType::Swarm, 0.5f } } },
        { "giant push",      9, { { ENpcType::Giant, 0.15f }, { ENpcType::Elite, 0.35f },
                                  { ENpcType::Swarm, 0.5f } } },
        { "siege column",   10, { { ENpcType::Giant, 0.2f }, { ENpcType::Lobber, 0.3f },
                                  { ENpcType::Brute, 0.2f }, { ENpcType::Swarm, 0.3f } } },
        { "titan",          11, { { ENpcType::Titan, 0.05f }, { ENpcType::Giant, 0.15f },
                                  { ENpcType::Lobber, 0.2f }, { ENpcType::Swarm, 0.6f } } },
        { "endgame",        13, { { ENpcType::Titan, 0.1f }, { ENpcType::Giant, 0.2f },
                                  { ENpcType::Elite, 0.3f }, { ENpcType::Lobber, 0.2f },
                                  { ENpcType::Swarm, 0.2f } } },
        { "hive",            9, { { ENpcType::Spawner, 0.1f }, { ENpcType::Elite, 0.3f },
                                  { ENpcType::Swarm, 0.6f } } },
        { "hive siege",     12, { { ENpcType::Spawner, 0.15f }, { ENpcType::Giant, 0.15f },
                                  { ENpcType::Lobber, 0.2f }, { ENpcType::Swarm, 0.5f } } },
    };
    constexpr int c_numWaveArchetypes = (int)(sizeof(c_waveArchetypes) / sizeof(c_waveArchetypes[0]));
    constexpr int c_maxArchetypeMinWave = [] {
        int m = 1;
        for (const WaveArchetype& a : c_waveArchetypes)
            m = a.minWave > m ? a.minWave : m;
        return m;
    }();

    // One unit from an archetype's authored mix (the ambient scatter's per-spawn roll - the wave
    // path samples its stored, jittered copy instead).
    ENpcType sampleArchetype(const WaveArchetype& arch)
    {
        float total = 0.0f;
        for (const auto& e : arch.mix)
            total += e.weight;
        float r = glm::linearRand(0.0f, glm::max(total, 1e-3f));
        for (const auto& e : arch.mix)
        {
            r -= e.weight;
            if (e.weight > 0.0f && r <= 0.0f)
                return e.type;
        }
        return ENpcType::Swarm;
    }
}

ENpcType GameMatch::sampleMix(const oc::fixed_vector<WaveMixEntry, 4>& mix) const
{
    float total = 0.0f;
    for (const WaveMixEntry& e : mix)
        total += e.weight;
    if (total <= 0.0f)
        return ENpcType::Swarm; // no rolled mix (shouldn't happen): the classic mass
    float r = glm::linearRand(0.0f, total);
    for (const WaveMixEntry& e : mix)
    {
        r -= e.weight;
        if (r <= 0.0f)
            return e.type;
    }
    return mix.back().type;
}

// The wave clock (authority, co-op only). The actual entity spawns are TRICKLED by tickCoopSpawns
// - a They-are-Billions-sized wave materialized in one frame would be a multi-hundred-spawn hitch.
void GameMatch::tickWaves(float deltaSec)
{
    // A NIGHT BUILDS UP: each wave is bigger than the one before, every Nth one is a surge, and the size and the
    // pause both vary at random - the night has lulls and peaks, and its last hours are its worst. m_waveScale is
    // rolled for the NEXT wave (at dawn for the night's first one, at each launch for the one after), so the HUD's
    // "Next wave power" shows the wave that will come.
    const float variation = glm::clamp(m_settings.nightWaveVariation, 0.0f, 0.9f);
    const auto rollNextWave = [&]
    {
        const bool surge = m_settings.nightSurgeEvery > 0 && (m_nightWave + 1) % m_settings.nightSurgeEvery == 0;
        m_waveScale = (1.0f + m_settings.nightWaveRamp * (float)m_nightWave) * glm::linearRand(1.0f - variation, 1.0f + variation)
            * (surge ? m_settings.nightSurgeSize : 1.0f);
    };
    if (m_sandboxWorld) // the light rules: demons come out at night only
    {
        const bool night = Globals::settings.sky.sunHeight() < 0.0f;
        if (night != m_night)
        {
            m_night = night;
            if (night)
            {
                ++m_nightIndex;
                m_waveTimer = 0.0f; // the first wave launches at dusk
                Log::info(oc::format("Co-op: night {} falls", m_nightIndex));
            }
            else
            {
                m_wavePendingBudget = 0.0f; // nothing more comes out in daylight
                m_nightWave = 0;
                rollNextWave(); // the coming night's first wave
                Log::info(oc::format("Co-op: dawn after night {}", m_nightIndex));
            }
        }
        if (!night)
            return;
        // The night's first wave waits for the Beacon: its ignition (tickBeacon) must be over before a demon comes out.
        if (m_nightWave == 0 && m_settings.beaconEnabled && m_beaconSet && m_beaconIgnite < 1.0f)
            return;
    }
    m_waveTimer -= deltaSec;
    if (m_waveTimer > 0.0f)
        return;
    m_waveTimer = m_settings.waveInterval;
    queueWave();
    if (m_sandboxWorld)
    {
        m_waveTimer = m_settings.nightWaveInterval * glm::linearRand(1.0f - variation, 1.0f + variation);
        ++m_nightWave;
        rollNextWave();
    }
}

int GameMatch::waveLevel() const
{
    if (!m_sandboxWorld)
        return m_waveIndex;
    return glm::max(m_night ? m_nightIndex - 1 : m_nightIndex, 0); // by day: the coming night
}

// The light rules own three runtime params. The sandbox world: a field does not push a demon (it walks in and burns),
// daylight burns the AI team, and the solar yield follows the daylight. Every other mode gets the neutral values.
void GameMatch::stampLightRules()
{
    GameUnitParams& unit = Globals::settings.game.unitParams;
    unit.fieldPush = !m_sandboxWorld || m_settings.fieldsBlockDemons;
    unit.sunBurnTeam = m_sandboxWorld ? (int)CoopAiTeam : -1;
    unit.slowTeam = m_sandboxWorld ? (int)CoopAiTeam : -1;
    unit.slowMult = glm::clamp(m_settings.demonSpeed, 0.05f, 2.0f);
    unit.fieldSlowMult = glm::clamp(m_settings.demonFieldSpeed, 0.05f, 1.0f);
    unit.shadowTeam = m_sandboxWorld && m_settings.demonShadowForm ? (int)CoopAiTeam : -1;
    // The burn starts only with the sun OVER the horizon (daylight() already rises a little under it, for the dusk
    // of the fields and the mirrors): a wave that comes out right after sunset must not burn in the dark.
    unit.sunDps = m_sandboxWorld ? m_settings.sunlightDps * glm::smoothstep(0.02f, 0.2f, Globals::settings.sky.sunHeight()) : 0.0f;
    Globals::settings.game.structures.solarDaylight = daylight();
    Globals::settings.game.structures.fieldNight = m_sandboxWorld ? 1.0f - daylight() : 1.0f; // elsewhere no field sleeps
}

float GameMatch::daylight() const
{
    return m_sandboxWorld ? glm::smoothstep(-0.05f, 0.15f, Globals::settings.sky.sunHeight()) : 1.0f;
}

// THE SHROUD's night (0 = day .. 1 = night), from the sun. AT DUSK (the afternoon) it comes in over a sun-height span of
// 0.2 down to duskFull ("Shroud full at sun height (dusk)"; 0 = the horizon). AT DAWN it goes from sun height 0 to 0.25.
// Both are 1 under the horizon and 0 at noon, so the switch does not show. The clouds (tickBeacon) and the faction
// look (tickFaction) follow it.
static float shroudNight(float duskFull)
{
    const SkyParams& sky = Globals::settings.sky;
    return 1.0f - (sky.timeOfDay >= 12.0f ? glm::smoothstep(duskFull, duskFull + 0.2f, sky.sunHeight()) : glm::smoothstep(0.0f, 0.25f, sky.sunHeight()));
}

// THE BEACON. The cloud hole is the clouds' vortex: this only places it on the Base and drives its three live values
// (the strength is a Runtime row, the spin is CPU-only, the coverage and its two variations are Runtime rows), so nothing here re-bakes a
// shader after the one-time enable. Every instance runs it off its own structure mirror.
void GameMatch::tickBeacon(float deltaSec)
{
    if (!m_sandboxWorld)
        return;
    if (!m_settings.beaconEnabled)
    {
        restoreBeacon();
        return;
    }
    const GameStructureComponent* base = nullptr;
    glm::vec3 basePos(0.0f);
    for (const StructureSystem::Ref& ref : m_structures.structures()) // the Base is the first structure spawned
        if (ref.type == EStructureType::Base && ref.state && ref.owner && ref.state->team == 0)
        {
            base = ref.state;
            basePos = ref.owner->pos;
            break;
        }
    if (!base)
        return;
    // The force bake is one horizontal slice of the fields ("Force/Bake" "Sample height", made for the y = 0 maps):
    // on the terrain it goes 1 m over the Base's ground, where the bodies around the base are (the Base's origin is
    // 3 m over its ground). ~GameMatch puts the old height back.
    Globals::settings.forceSystem.bakeSampleHeight = basePos.y - 2.0f;
    CloudParams& clouds = Globals::settings.clouds;
    if (!m_beaconSet)
    {
        m_beaconSaved = { clouds.vortexEnabled, clouds.vortexCentreXKm, clouds.vortexCentreZKm, clouds.vortexStrength,
            clouds.vortexSpinDegPerMin, clouds.coverage, clouds.coverageVariation, clouds.macroVariation, clouds.vortexEyeScale,
            clouds.vortexLightIntensity, clouds.vortexLightColor };
        clouds.vortexEnabled = true;
        clouds.vortexCentreXKm = basePos.x * 0.001f;
        clouds.vortexCentreZKm = basePos.z * 0.001f;
        Tweak::notifyChanged(clouds.vortexEnabled);
        Tweak::notifyChanged(clouds.vortexCentreXKm);
        Tweak::notifyChanged(clouds.vortexCentreZKm);
        m_beaconSet = true;
        m_beaconPower = 0.0f;
        m_beaconNightSec = 0.0f;
        m_beaconIgnite = 0.0f;
        m_beaconClimb = 0.0f;
        m_beaconGlow = 0.0f;
        m_beaconLit = false;
    }
    // Power = the Base field's output (it latches dark when starved) x its charge: a low store shows as a weak hole.
    const float charge = base->capacity[0] > 0.0f ? glm::clamp(base->store[0] / base->capacity[0], 0.0f, 1.0f) : 0.0f;
    const bool alive = !base->blueprint && base->health > 0.0f;
    const float target = alive ? glm::clamp(base->emitter.outputFrac, 0.0f, 1.0f) * glm::mix(0.4f, 1.0f, charge) : 0.0f;
    m_beaconPower += (target - m_beaconPower) * (1.0f - std::exp(-glm::max(deltaSec, 0.0f) / 2.0f));
    // The SHROUD's night: the clouds close at dusk and open at dawn by it (the night's first wave is due at the horizon:
    // it waits for the ignition below).
    const float night = shroudNight(m_settings.shroudDuskSunHeight);
    // THE IGNITION: at dusk the clouds close first, with no hole. "Ignite delay" after the clouds are closed the Beacon turns
    // on, in three steps. THE GLOW ("Beam glow time", m_beaconGlow 0 -> 1): a short beam on the Base brightens. THE CLIMB
    // ("Beam climb time", m_beaconClimb): its tip goes up to the cloud base, slow near the ground and faster in the sky.
    // THE PIERCE ("Pierce time", m_beaconIgnite): the beam TEARS through - the hole is there within the first quarter,
    // as a small eye, and the eye then GROWS to its size over the rest (the vortex's live "Eye size scale").
    // AT DAWN: the clouds start to open (the shroud's night falls under 1), the light goes off (the glow runs back), then
    // the hole closes SLOWLY (the pierce runs back over "Hole close time": calm, like the clouds drifting shut).
    const float dt = glm::max(deltaSec, 0.0f);
    m_beaconNightSec = night >= 0.999f ? m_beaconNightSec + dt : 0.0f;
    // On: the clouds closed + the delay. Off: only when the shroud is down to "Light off at shroud" (dawn: the clouds
    // are half open and a lot of sun is through them already). Then the beam FADES out over "Beam fade out time".
    if (!m_beaconLit)
        m_beaconLit = night >= 0.999f && m_beaconNightSec >= m_settings.beaconIgniteDelay;
    else if (night < glm::clamp(m_settings.beaconOffShroud, 0.0f, 0.99f))
        m_beaconLit = false;
    const bool lit = m_beaconLit;
    const float glowStep = dt / glm::max(m_settings.beaconGlowSeconds, 0.01f);
    const float climbStep = dt / glm::max(m_settings.beaconClimbSeconds, 0.01f);
    const float pierceStep = dt / glm::max(m_settings.beaconPierceSeconds, 0.1f);
    if (lit)
    {
        if (m_beaconGlow < 1.0f)
            m_beaconGlow = glm::min(m_beaconGlow + glowStep, 1.0f);
        else if (m_beaconClimb < 1.0f)
            m_beaconClimb = glm::min(m_beaconClimb + climbStep, 1.0f);
        else
            m_beaconIgnite = glm::min(m_beaconIgnite + pierceStep, 1.0f);
    }
    else if (m_beaconGlow > 0.0f) // DAWN: the light goes off first (the whole beam fades), then the hole closes
        m_beaconGlow = glm::max(m_beaconGlow - dt / glm::max(m_settings.beaconFadeSeconds, 0.01f), 0.0f);
    else
    {
        m_beaconClimb = 0.0f;
        m_beaconIgnite = glm::max(m_beaconIgnite - dt / glm::max(m_settings.beaconCloseSeconds, 0.1f), 0.0f);
    }
    // The tear: 0 until the beam is at the clouds, full within the first quarter. The CLOSE (unlit) is calm: the hole
    // fades over the whole "Hole close time", like the clouds drifting shut.
    const float pierce = glm::smoothstep(0.0f, lit ? 0.25f : 1.0f, m_beaconIgnite);
    const float grow = glm::mix(0.05f, 1.0f, glm::smoothstep(0.0f, 1.0f, m_beaconIgnite));
    // The hole follows the IGNITION, not the shroud: it stays open under the beam while the clouds thin at dawn. The
    // day's hole ("Day hole strength", full size) comes in with the daylight where the ignition's is weaker.
    const float dayHole = glm::clamp(m_settings.beaconDayStrength, 0.0f, 1.0f) * (1.0f - night);
    clouds.vortexEyeScale = pierce >= dayHole && pierce > 0.0f ? grow : 1.0f;
    clouds.vortexStrength = m_beaconPower * glm::max(pierce, dayHole);
    clouds.vortexSpinDegPerMin = glm::mix(m_settings.beaconCalmSpin, m_settings.beaconPressureSpin, night * (1.0f - charge));
    clouds.coverage = glm::mix(m_beaconSaved.coverage, m_settings.shroudCoverage, night);
    // The night's deck is UNIFORM: no weather-map or macro spread, so the shroud covers the whole sky.
    clouds.coverageVariation = m_beaconSaved.coverageVariation * (1.0f - night);
    clouds.macroVariation = m_beaconSaved.macroVariation * (1.0f - night);

    // The beam, from the Base's top: it lights the ground and the buildings around it, and the fog apply draws its
    // column in the air (see BeamLight in the renderer's Light.ixx).
    // Only while LIT (the glow): by day the Beacon is dark (its diamond and the cloud hole still mark it).
    // It also lights the clouds around the hole: the vortex's eye light (a live value), on the eye's axis = the Base.
    const float beam = m_beaconPower;
    const bool beamOn = beam >= 0.02f && m_beaconGlow > 0.0f && m_settings.beaconLightIntensity > 0.0f;
    const glm::vec3 beamColor(1.0f, 0.82f, 0.45f);
    clouds.vortexLightColor = beamColor;
    const float glow = glm::smoothstep(0.0f, 1.0f, m_beaconGlow);
    clouds.vortexLightIntensity = beamOn ? glm::max(m_settings.beaconCloudLight, 0.0f) * beam * pierce * glow : 0.0f;
    if (!beamOn)
        return;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const float fullHeight = glm::max(m_settings.beaconBeamHeight, 50.0f);
    const float toClouds = glm::clamp(clouds.bottom - basePos.y, 50.0f, fullHeight);
    // The tip's height grows GEOMETRICALLY (a constant factor per second): slow near the ground, faster in the sky.
    constexpr float c_glowHeight = 8.0f; // the short beam of the glow step
    const float climbed = c_glowHeight * std::pow(toClouds / c_glowHeight, m_beaconClimb);
    const float height = climbed * std::pow(fullHeight / climbed, pierce);
    const glm::vec3 foot = basePos + up * 3.2f; // over the Base's crown, under its diamond, which floats INSIDE the beam (base.pre)
    // TWO records: the tall beam is the glow in the air ALONE, and a SHORT beam at its foot lights the surfaces. The
    // light grid holds every 32 m grid in a light's range: the tall beam with its range took gigabytes of it.
    constexpr float c_surfaceBeamHeight = 30.0f;
    const float range = glm::max(m_settings.beaconLightRange, 1.0f);
    const float radius = glm::max(m_settings.beaconBeamRadius, 0.05f);
    const float intensity = m_settings.beaconLightIntensity * beam * glow;
    Globals::rendererVK.addBeamColumn(BeamLight(foot + up * (height * 0.5f), range, beamColor, intensity, up, radius, height,
        height * glm::clamp(m_settings.beaconBeamTipFade, 0.0f, 1.0f))); // the tip fades at every height: soft while it climbs too
    const float surfaceHeight = glm::min(height, c_surfaceBeamHeight);
    Globals::rendererVK.addLightInfo(BeamLight(foot + up * (surfaceHeight * 0.5f), range, beamColor, intensity, up, radius, surfaceHeight), false);
}

void GameMatch::addSpawnFog(const glm::vec3& pos, float radius)
{
    m_spawnFog[m_spawnFogNext] = { pos, glm::clamp(radius, 6.0f, 60.0f), 0.0f };
    m_spawnFogNext = (m_spawnFogNext + 1) % c_spawnFogSites;
}

void GameMatch::tickFogEffects(float deltaSec)
{
    if (!m_sandboxWorld)
        return;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 smoke(0.35f, 0.16f, 0.6f); // the demons' violet
    const float life = glm::max(m_settings.spawnFogSeconds, 1.0f);
    for (SpawnFog& site : m_spawnFog)
    {
        if (site.age < 0.0f)
            continue;
        site.age += deltaSec;
        if (site.age >= life || m_settings.spawnFogDensity <= 0.0f)
        {
            site.age = -1.0f;
            continue;
        }
        const float fade = glm::smoothstep(0.0f, 4.0f, site.age) * (1.0f - glm::smoothstep(life * 0.7f, life, site.age));
        // The bank on the ground, wider than the spawn blob.
        RendererVKLayout::FogVolumeInfo bank;
        bank.pos = site.pos + up * 5.0f;
        bank.density = m_settings.spawnFogDensity * fade;
        bank.halfExtents = glm::vec3(site.radius * 1.4f, 9.0f, site.radius * 1.4f);
        bank.edgeSoftness = 0.8f;
        bank.albedo = smoke;
        bank.emissive = m_settings.spawnFogGlow;
        Globals::rendererVK.addFogVolume(bank);
        // The column: the demons come down out of the clouds. Thinner than the bank, up to the cloud base.
        const float height = glm::max(Globals::settings.clouds.bottom - site.pos.y, 60.0f);
        RendererVKLayout::FogVolumeInfo column = bank;
        column.pos = site.pos + up * (height * 0.5f);
        column.density = bank.density * 0.5f;
        column.halfExtents = glm::vec3(site.radius * 0.45f, height * 0.5f, site.radius * 0.45f);
        column.edgeSoftness = 1.0f;
        Globals::rendererVK.addFogVolume(column);
        // THE SIGNAL: a red beam through the column, from the ground to the cloud base, that pulses - a beam's glow is
        // not dimmed by the fog or the distance (vol_apply), so the players see from the Base where a wave comes down.
        // Red, to tell it from an outpost's steady violet beam. One of the frame's beam columns.
        if (m_settings.spawnBeamGlow > 0.0f)
        {
            const float pulse = 0.65f + 0.35f * std::sin(site.age * 4.0f);
            Globals::rendererVK.addBeamColumn(BeamLight(site.pos + up * (height * 0.5f), glm::max(site.radius * 1.5f, 20.0f),
                glm::vec3(1.0f, 0.16f, 0.2f), m_settings.spawnBeamGlow * fade * pulse, up, glm::max(m_settings.spawnBeamRadius, 0.05f),
                height, height * 0.3f));
        }
    }
    // The fields clear the fog: AFTER the sites above (a clearing volume removes what the list holds before it).
    if (m_settings.fieldFogClear > 0.0f)
    {
        constexpr size_t c_maxClear = 16; // every volume is one box test per froxel
        for (size_t i = 0; i < glm::min(m_fieldZones.size(), c_maxClear); ++i)
        {
            const glm::vec4& zone = m_fieldZones[i];
            RendererVKLayout::FogVolumeInfo clear;
            clear.pos = glm::vec3(zone);
            clear.density = -glm::min(m_settings.fieldFogClear, 1.0f);
            clear.halfExtents = glm::vec3(zone.w); // a box around the sphere: its soft edge rounds the corners off
            clear.edgeSoftness = 0.45f;
            clear.albedo = glm::vec3(0.0f);
            clear.emissive = 0.0f;
            Globals::rendererVK.addFogVolume(clear);
        }
    }
}

// THE FACTION LAYER (Game:Faction): made around the Base when it exists; every frame it gets its sources and a slice
// of its spread. At the end of each sweep the renderer gets the bytes for the look.
void GameMatch::tickFaction(float deltaSec)
{
    const GameFactionSettings& s = Globals::settings.game.faction;
    if (!m_sandboxWorld || !s.enabled)
    {
        if (m_faction.valid())
        {
            m_faction.clear();
            Globals::rendererVK.clearFactionMap();
        }
        return;
    }
    const StructureSystem::Ref* base = nullptr;
    for (const StructureSystem::Ref& ref : m_structures.structures()) // the Base is the first structure spawned
        if (ref.type == EStructureType::Base && ref.state && ref.owner && ref.state->team == 0)
        {
            base = &ref;
            break;
        }
    const glm::vec3 basePos = base ? base->owner->pos : glm::vec3(0.0f);
    tickOutposts(deltaSec, base ? &basePos : nullptr);
    // BY DAY only the land the enemy's sources hold has the enemy's look; BY NIGHT all land under neutral has it (the
    // untouched land starts one byte under neutral). The renderer does the step: a live value, no upload.
    Globals::rendererVK.setFactionNight(shroudNight(m_settings.shroudDuskSunHeight));
    // The renderer puts "Tint curve" on the bytes on their way to the GPU: its copy holds the curve it was sent with.
    const float curve = Globals::settings.terrain.factionTintCurve;
    const auto send = [&](const glm::ivec4* dirty) // dirty null = all of the map (both images of the renderer's pair, over the next two sends)
    {
        Globals::rendererVK.setFactionMap(m_faction.bytes(), FactionLayer::Size, m_faction.origin(), m_faction.cellSize(), m_faction.startValue(), dirty, curve);
    };
    if (!m_faction.valid())
    {
        if (!base)
            return;
        m_faction.reset(glm::vec2(base->owner->pos.x, base->owner->pos.z), s.cellSize, (uint8)glm::clamp(s.startValue, 0, 255));
        send(nullptr);
        return; // one send a frame: the spread starts next frame
    }
    if (Globals::rendererVK.factionMapCurveChanged(curve)) // the tweak moved: send all of it again
    {
        send(nullptr);
        return;
    }
    m_factionSources.clear();
    if (base && !base->state->blueprint && base->state->health > 0.0f)
        m_factionSources.push_back({ glm::vec2(base->owner->pos.x, base->owner->pos.z), s.baseRadius, 1.0f });
    for (const glm::vec4& zone : m_fieldZones) // the lit field bubbles (refreshed on the SIM LOD timer)
        m_factionSources.push_back({ glm::vec2(zone.x, zone.z), zone.w * s.fieldRadiusScale, 1.0f });
    for (const SpawnFog& site : m_spawnFog) // where a wave came down: enemy land while its fog stands
        if (site.age >= 0.0f)
            m_factionSources.push_back({ glm::vec2(site.pos.x, site.pos.z), s.enemySiteRadius, -1.0f });
    for (const EnemyOutpost& outpost : m_outposts) // the dark beacons
        m_factionSources.push_back({ glm::vec2(outpost.pos.x, outpost.pos.z), s.outpostRadius, -1.0f });
    m_faction.setSources(m_factionSources);
    glm::ivec4 dirty;
    if (m_faction.tick(deltaSec, dirty)) // a sweep ended and changed bytes: only their rectangle goes to the renderer
        send(&dirty);
}

int GameMatch::physicsFocus(glm::vec3* out, int maxPoints) const
{
    if (!m_enabled || !m_sandboxWorld)
        return 0;
    int count = 0;
    const auto add = [&](const glm::vec3& pos)
    {
        if (count < maxPoints)
            out[count++] = pos;
    };
    add(m_player.bodyPos());
    for (const auto& [id, twin] : m_clientPlayers)
        if (twin)
            add(twin->pos);
    add(m_basePos); // the waves march to it and fight there, also while every player is away
    return count;
}

// THE ENEMY OUTPOSTS: "Count" dark beacons on a regular polygon "Distance from the Base" out (8 = the octagon, turned
// half a step so none stands due north / east). Every instance makes the same ring from the Base's position. Each is
// an enemy source of the faction layer (tickFaction) and draws a violet beam (a glow-only column: no light record).
// Its BUILDING needs the ground's height, and the terrain collider exists only near a camera: a site with no building
// is probed once a second and gets it when the ray first hits.
void GameMatch::tickOutposts(float deltaSec, const glm::vec3* basePos)
{
    const GameFactionSettings& s = Globals::settings.game.faction;
    if (!s.outpostsEnabled)
    {
        for (EnemyOutpost& outpost : m_outposts)
            if (outpost.entity)
                Globals::world.removeRootEntity(outpost.entity.get());
        m_outposts.clear();
        return;
    }
    if (m_outposts.empty())
    {
        if (!basePos)
            return;
        const int count = glm::clamp(s.outpostCount, 1, 16);
        for (int i = 0; i < count; ++i)
        {
            const float angle = ((float)i + 0.5f) * glm::two_pi<float>() / (float)count;
            m_outposts.push_back({ *basePos + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * s.outpostDistance, EntityPtr() });
        }
        m_outpostGroundTimer = 0.0f;
    }

    m_outpostGroundTimer -= deltaSec;
    const bool probe = m_outpostGroundTimer <= 0.0f;
    if (probe)
        m_outpostGroundTimer = 1.0f;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 violet(0.6f, 0.15f, 1.0f);
    for (EnemyOutpost& outpost : m_outposts)
    {
        if (probe && !outpost.entity)
        {
            constexpr float c_noGround = -1.0e30f;
            const float y = m_structures.groundHeightAt(outpost.pos.x, outpost.pos.z, c_noGround); // the collider's EXACT height only: a building
            if (y > c_noGround * 0.5f)
            {
                outpost.pos.y = y + 3.0f; // the prefab's origin is 3 m over its ground (the Base's box)
                outpost.entity = Globals::world.spawnAssetFile("Entities/Game/enemyOutpost.pre", Transform(outpost.pos), true);
                if (outpost.entity)
                {
                    outpost.entity->setName("Enemy outpost");
                    Globals::world.addRootEntity(outpost.entity);
                }
            }
        }
        if (s.outpostBeamIntensity > 0.0f)
        {
            const float height = glm::max(s.outpostBeamHeight, 10.0f);
            Globals::rendererVK.addBeamColumn(BeamLight(outpost.pos + up * (3.2f + height * 0.5f), glm::max(s.outpostBeamRange, 1.0f), violet,
                s.outpostBeamIntensity, up, glm::max(s.outpostBeamRadius, 0.05f), height, height * glm::clamp(s.outpostBeamTipFade, 0.0f, 1.0f)));
        }
    }
}

// LIGHTNING: at night, flashes of light inside the clouds around the player (the clouds' flash slots, lit in the cloud
// march), a share of them with an ARC to the ground - a zigzag of glow-only beam columns (Renderer::addBeamColumn: no
// light record, nothing in the light grid), sometimes with a side fork. A look only, rolled per instance.
namespace
{
    float lightningHash(uint32 seed, uint32 n)
    {
        uint32 h = seed + n * 0x9E3779B9u;
        h = (h ^ (h >> 16)) * 0x7feb352du;
        h = (h ^ (h >> 15)) * 0x846ca68bu;
        return (float)((h ^ (h >> 16)) >> 8) * (1.0f / 16777216.0f);
    }

    // A flash is 1 to 6 STROKES, all from its seed: each at a random time after the one before (most gaps short, some
    // long), with a random strength and a random decay - so one flash is a single pop, the next a long stutter. Returns
    // the light's level at `age`; outLife = when the last stroke has died. timeScale stretches all of it.
    float lightningLevel(uint32 seed, float age, float timeScale, float& outLife)
    {
        const int strokes = 1 + (int)(lightningHash(seed, 1u) * lightningHash(seed, 2u) * 5.99f); // few strokes are the most common
        float level = 0.0f, start = 0.0f;
        outLife = 0.0f;
        for (int k = 0; k < strokes; ++k)
        {
            const uint32 n = 10u + (uint32)k * 4u;
            const float gap = lightningHash(seed, n);
            if (k > 0)
                start += timeScale * glm::mix(0.03f, 0.45f, gap * gap * gap);
            const float strength = glm::mix(0.25f, 1.0f, lightningHash(seed, n + 1u));
            const float decay = timeScale * glm::mix(0.03f, 0.22f, lightningHash(seed, n + 2u) * lightningHash(seed, n + 3u));
            if (age >= start)
                level += strength * std::exp(-(age - start) / decay);
            outLife = glm::max(outLife, start + decay * 5.0f);
        }
        return glm::min(level, 1.5f);
    }
}

void GameMatch::tickLightning(float deltaSec)
{
    if (!m_sandboxWorld)
        return;
    CloudParams& clouds = Globals::settings.clouds;
    const float dt = glm::max(deltaSec, 0.0f);
    const float night = 1.0f - daylight();
    const glm::vec3 centre = m_player.bodyPos();

    // A new flash: the gaps are random around the mean, and longer while the night is not full.
    if (m_settings.lightningEnabled && night > 0.5f && m_settings.lightningPerMinute > 0.0f)
    {
        m_lightningTimer -= dt * night;
        if (m_lightningTimer <= 0.0f)
        {
            m_lightningTimer = 60.0f / m_settings.lightningPerMinute * glm::linearRand(0.2f, 1.8f);
            // Slot 0 alone may carry the arc (one zigzag of columns a frame at most).
            const bool wantStrike = glm::linearRand(0.0f, 1.0f) < m_settings.lightningStrikeShare && m_lightning[0].age < 0.0f;
            Lightning* slot = wantStrike ? &m_lightning[0] : m_lightning[1].age < 0.0f ? &m_lightning[1] : m_lightning[0].age < 0.0f ? &m_lightning[0] : nullptr;
            if (slot)
            {
                const float minDist = glm::max(m_settings.lightningMinDistance, 0.0f);
                const float dist = glm::mix(minDist, glm::max(m_settings.lightningMaxDistance, minDist + 1.0f), std::sqrt(glm::linearRand(0.0f, 1.0f)));
                const float angle = glm::linearRand(0.0f, glm::two_pi<float>());
                const float thickness = glm::max(clouds.top - clouds.bottom, 100.0f);
                // An arc leaves from just inside the cloud base; a flash with no arc sits deeper in the layer.
                const float alt = clouds.bottom + thickness * (wantStrike ? 0.08f : glm::linearRand(0.15f, 0.6f));
                slot->pos = glm::vec3(centre.x + std::cos(angle) * dist, alt, centre.z + std::sin(angle) * dist);
                // THE VORTEX: no flash in the hole (no cloud there). A point that falls into it, and "Vortex wall share"
                // of all flashes while the hole is open, go to the EYEWALL instead, at any height of the layer (10 - 85 %)
                // - an arc from there starts at the hole's inner edge and runs down inside it.
                if (clouds.vortexEnabled && clouds.vortexStrength > 0.3f)
                {
                    const glm::vec2 eyeCentre(clouds.vortexCentreXKm * 1000.0f, clouds.vortexCentreZKm * 1000.0f);
                    const float eye = glm::max(clouds.vortexEyeRadiusKm, 0.05f) * 1000.0f * glm::clamp(clouds.vortexEyeScale, 0.0f, 1.0f);
                    const float wall = glm::max(clouds.vortexEyewallWidthKm, 0.05f) * 1000.0f;
                    glm::vec2 fromEye = glm::vec2(slot->pos.x, slot->pos.z) - eyeCentre;
                    if (glm::length(fromEye) < eye * 1.05f || glm::linearRand(0.0f, 1.0f) < m_settings.lightningWallShare)
                    {
                        const float wallAngle = glm::linearRand(0.0f, glm::two_pi<float>());
                        fromEye = glm::vec2(std::cos(wallAngle), std::sin(wallAngle)) * (eye * 1.05f + (wantStrike ? 0.0f : glm::linearRand(0.0f, wall * 0.5f)));
                        slot->pos = glm::vec3(eyeCentre.x + fromEye.x, clouds.bottom + thickness * glm::linearRand(0.1f, 0.85f), eyeCentre.y + fromEye.y);
                    }
                }
                slot->age = 0.0f;
                slot->strike = wantStrike;
                slot->fork = wantStrike && glm::linearRand(0.0f, 1.0f) < m_settings.lightningForkShare;
                slot->seed = (uint32)glm::linearRand(1, 0x7ffffffe);
                lightningLevel(slot->seed, 0.0f, glm::max(m_settings.lightningDuration, 0.05f) / 0.6f, slot->life);
                if (wantStrike)
                {
                    const float lean = (slot->pos.y - centre.y) * 0.3f; // the arc leans: its foot is not straight under its top
                    const glm::vec2 foot(slot->pos.x + glm::linearRand(-lean, lean), slot->pos.z + glm::linearRand(-lean, lean));
                    slot->ground = glm::vec3(foot.x, groundY(foot.x, foot.y, centre.y), foot.y);
                }
            }
        }
    }

    clouds.flashColor = m_settings.lightningColor;
    clouds.flashRadius = glm::max(m_settings.lightningCloudRadius, 10.0f);
    for (int i = 0; i < 2; ++i)
    {
        Lightning& flash = m_lightning[i];
        clouds.flashes[i] = glm::vec4(0.0f);
        if (flash.age < 0.0f)
            continue;
        flash.age += dt;
        if (flash.age >= flash.life)
        {
            flash.age = -1.0f;
            continue;
        }
        // The level: the flash's own strokes (lightningLevel), x a little shimmer 30 times a second.
        float life;
        const float level = lightningLevel(flash.seed, flash.age, glm::max(m_settings.lightningDuration, 0.05f) / 0.6f, life)
            * (0.8f + 0.2f * lightningHash(flash.seed, 1000u + (uint32)(flash.age * 30.0f)));
        clouds.flashes[i] = glm::vec4(flash.pos, m_settings.lightningCloudLight * level);

        if (!flash.strike || m_settings.lightningArcIntensity <= 0.0f)
            continue;
        // THE ARC: a random walk sideways around the straight line, pulled back to it at the ground. The same seed
        // every frame, so the arc holds its shape while it flickers.
        constexpr int c_segments = 12;
        constexpr int c_forkAt = 5, c_forkSegments = 4;
        const glm::vec3 span = flash.ground - flash.pos;
        const float length = glm::length(span);
        if (length < 1.0f)
            continue;
        const glm::vec3 dir = span / length;
        const glm::vec3 side = glm::normalize(glm::cross(dir, glm::abs(dir.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f)));
        const glm::vec3 side2 = glm::cross(dir, side);
        const float step = glm::clamp(m_settings.lightningArcJitter, 0.0f, 0.5f) * length / std::sqrt((float)c_segments);
        const float radius = glm::max(m_settings.lightningArcRadius, 0.05f);
        const float glow = m_settings.lightningArcIntensity * level;
        const auto column = [&](const glm::vec3& a, const glm::vec3& b, float intensity, float coreRadius)
        {
            const glm::vec3 seg = b - a;
            const float len = glm::length(seg);
            if (len > 0.01f)
                Globals::rendererVK.addBeamColumn(BeamLight((a + b) * 0.5f, coreRadius * 25.0f, m_settings.lightningColor, intensity, seg, coreRadius, len));
        };
        glm::vec2 walk(0.0f);
        glm::vec3 prev = flash.pos;
        for (int k = 1; k <= c_segments; ++k)
        {
            const float s = (float)k / (float)c_segments;
            walk += glm::vec2(lightningHash(flash.seed, 100u + (uint32)k), lightningHash(flash.seed, 200u + (uint32)k)) * 2.0f - 1.0f;
            const glm::vec2 off = walk * (step * (1.0f - s * s));
            const glm::vec3 point = flash.pos + span * s + side * off.x + side2 * off.y;
            column(prev, point, glow, radius);
            if (flash.fork && k == c_forkAt)
            {
                // THE FORK: a thinner, dimmer branch off to one side, ending in the air.
                const float turn = lightningHash(flash.seed, 300u) * glm::two_pi<float>();
                const glm::vec3 out = glm::normalize(dir + (side * std::cos(turn) + side2 * std::sin(turn)) * 0.9f);
                const float forkStep = length * 0.3f / (float)c_forkSegments;
                glm::vec3 from = point;
                for (int j = 1; j <= c_forkSegments; ++j)
                {
                    const glm::vec2 kink = glm::vec2(lightningHash(flash.seed, 400u + (uint32)j), lightningHash(flash.seed, 500u + (uint32)j)) * 2.0f - 1.0f;
                    const glm::vec3 to = from + out * forkStep + (side * kink.x + side2 * kink.y) * (forkStep * 0.35f);
                    column(from, to, glow * 0.4f, radius * 0.6f);
                    from = to;
                }
            }
            prev = point;
        }
    }
}

void GameMatch::restoreBeacon()
{
    CloudParams& clouds = Globals::settings.clouds;
    for (glm::vec4& flash : clouds.flashes) // the lightning's cloud flashes end with the match
        flash = glm::vec4(0.0f);
    if (!m_beaconSet)
        return;
    clouds.vortexEnabled = m_beaconSaved.vortexEnabled;
    clouds.vortexCentreXKm = m_beaconSaved.centreXKm;
    clouds.vortexCentreZKm = m_beaconSaved.centreZKm;
    clouds.vortexStrength = m_beaconSaved.strength;
    clouds.vortexSpinDegPerMin = m_beaconSaved.spin;
    clouds.coverage = m_beaconSaved.coverage;
    clouds.coverageVariation = m_beaconSaved.coverageVariation;
    clouds.macroVariation = m_beaconSaved.macroVariation;
    clouds.vortexEyeScale = m_beaconSaved.eyeScale;
    clouds.vortexLightIntensity = m_beaconSaved.lightIntensity;
    clouds.vortexLightColor = m_beaconSaved.lightColor;
    Tweak::notifyChanged(clouds.vortexEnabled);
    Tweak::notifyChanged(clouds.vortexCentreXKm);
    Tweak::notifyChanged(clouds.vortexCentreZKm);
    m_beaconSet = false;
}

// Live units - ambient and previous waves alike (player-team units too): the component's own
// count, kept at its spawn / destroy edges.
int GameMatch::aiAliveCount() const
{
    return NpcSystem::countUnits();
}

// The NEXT wave's budget in points, before the "Max enemy units" cap: base + growth per wave so
// far, where the growth itself climbs by "Wave growth growth" every wave -
// base + growth*i + growthGrowth * (0 + 1 + ... + (i-1)) for the 0-based wave index i.
// THE SANDBOX WORLD: a night's BASE power is the AVERAGE wave power of the night before it. A night's waves are
// base x (1 + ramp x k) x (surge ? size : 1) for its k-th wave (tickWaves; the random variation averages to 1), so
// the average of a night is base x nightAverageScale(), and night n (0-based) has the base
// "Wave budget" x nightAverageScale()^n - a closed form: no night's power is kept.
float GameMatch::nextWaveBudget() const
{
    const float wave = (float)waveLevel();
    if (m_sandboxWorld)
        return glm::min((float)m_settings.waveBudget * std::pow(nightAverageScale(), wave), 1e7f) * m_waveScale * m_settings.waveSizeMult;
    return ((float)m_settings.waveBudget + m_settings.waveBudgetGrowth * wave + m_settings.waveGrowthGrowth * wave * (wave - 1.0f) * 0.5f)
        * m_waveScale * m_settings.waveSizeMult; // the night's build-up (tickWaves; 1 outside the sandbox world) x the user's knob
}

// The mean of a night's wave scales (1 + ramp x k, x the surge size on every Nth wave) over the waves a night HAS: the
// first at dusk, then one per "Night wave interval (s)" for the night's length - from the sun cycle's latitude, day
// of year and speed, so a longer night or a shorter interval (more waves, a higher ramp reached) raises it.
float GameMatch::nightAverageScale() const
{
    const auto& sky = Globals::settings.sky;
    const float decl = glm::radians(23.44f) * std::sin(glm::two_pi<float>() * (284.0f + sky.dayOfYear) / 365.0f); // as SkyParams::cycleSunDirection
    const float cosSunset = glm::clamp(-std::tan(glm::radians(sky.latitude)) * std::tan(decl), -1.0f, 1.0f); // the hour angle of the sunset
    const float nightHours = 24.0f - 2.0f * glm::degrees(std::acos(cosSunset)) / 15.0f;
    const float nightSeconds = nightHours * 3600.0f / glm::max(sky.sunCycleSpeed, 0.01f);
    const int waves = glm::clamp((int)(nightSeconds / glm::max(m_settings.nightWaveInterval, 1.0f)) + 1, 1, 512);
    float sum = 0.0f;
    for (int k = 0; k < waves; ++k)
    {
        const bool surge = m_settings.nightSurgeEvery > 0 && (k + 1) % m_settings.nightSurgeEvery == 0;
        sum += (1.0f + m_settings.nightWaveRamp * (float)k) * (surge ? m_settings.nightSurgeSize : 1.0f);
    }
    return sum / (float)waves;
}

// The wave blob's radius for `budget` points of the CURRENT mix. Area - not radius - scales with
// the expected body count, so the areal density is the same in wave 1 and wave 20: the old formula
// grew the radius LINEARLY off the REMAINING budget and capped at 30 m, which meant a big wave
// stacked hundreds of bodies on one disc (parked bodies do not push apart, so the pile only
// resolved as physics shoved them out once a player came near) AND the disc SHRANK as the wave
// trickled, packing the tail tightest.
//
// The blob is clamped to the ground plane and any point inside the barrier square is pushed back
// out (see tickCoopSpawns), so an oversized disc becomes a wide BAND hugging the barrier - the
// only direction with room, since the spawn band is just c_coopGroundEdge - c_coopHalfSize deep.
float GameMatch::waveSpawnRadius(float budget) const
{
    float weight = 0.0f, cost = 0.0f;
    for (const WaveMixEntry& e : m_waveMix)
    {
        weight += e.weight;
        cost += e.weight * waveCostOf(e.type);
    }
    const float meanCost = weight > 0.0f ? glm::max(cost / weight, 0.1f) : 1.0f;
    const float bodies = glm::max(budget, 0.0f) / meanCost;
    const float area = bodies * glm::max(m_settings.waveSpawnAreaPerUnit, 1.0f);
    // 8 m floor keeps a handful of bodies from spawning on one spot; the ceiling is the world.
    return glm::clamp(std::sqrt(area / glm::pi<float>()), 8.0f, c_coopGroundEdge);
}

void GameMatch::queueWave()
{
    const int aiAlive = aiAliveCount(); // ambient + previous waves both count against the cap
    // BUDGET points, not a unit count: the alive cap converts conservatively at the CHEAPEST cost
    // (the worst-case body count a budget could buy).
    float cheapest = FLT_MAX;
    for (int t = 0; t < (int)ENpcType::Count; ++t)
        cheapest = glm::min(cheapest, waveCostOf((ENpcType)t));
    // Wave i (0-based) = base + growth per wave so far, where the growth itself climbs by "Wave
    // growth growth" every wave: base + growth*i + growthGrowth * (0 + 1 + ... + (i-1)).
    const float budget = glm::min(nextWaveBudget(),
        (float)(m_settings.waveMaxAlive - aiAlive) * cheapest - m_ambientPendingBudget - m_wavePendingBudget);
    ++m_waveIndex;
    if (budget <= 0.0f)
        return; // at the cap: the clock (and the scaling) still advanced
    // A random compass direction, projected onto the barrier square and stepped OUTSIDE it: the
    // swarm clusters in the open ring beyond the barrier (its collider only matches players) and
    // pushes at the Base's near face (a point INSIDE the footprint would fail the A* and the move
    // order alike - the pointOutsideFootprint rule). The locked order releases on arrival; the AI
    // takes over there. The carved lanes guarantee a route in from any side.
    const float angle = glm::linearRand(0.0f, glm::two_pi<float>());
    const glm::vec3 dir(std::cos(angle), 0.0f, std::sin(angle));
    if (m_sandboxWorld)
    {
        // No barrier and no map edge: the wave comes down on ENEMY LAND (the faction layer) - out from the Base along
        // the direction, from "Sandbox wave distance" on, at the first point that is the enemy's and stays so for the
        // next step, + a margin; "Sandbox wave max distance" at the latest. So the demons come from farther away as
        // the players' land grows. No layer (it is off): the plain ring at the min distance.
        float distance = m_settings.sandboxWaveDistance;
        if (m_faction.valid())
        {
            constexpr float c_step = 8.0f, c_margin = 16.0f;
            constexpr uint8 c_enemyLand = FactionLayer::Neutral - 1; // under neutral: the untouched land (the start value) and the enemy's
            const float maxDistance = glm::max(m_settings.sandboxWaveMaxDistance, distance);
            const auto enemyAt = [&](float d) { return m_faction.sample(m_basePos.x + dir.x * d, m_basePos.z + dir.z * d) <= c_enemyLand; };
            while (distance < maxDistance && !(enemyAt(distance) && enemyAt(distance + c_step)))
                distance += c_step;
            distance = glm::min(distance + c_margin, maxDistance);
        }
        m_waveOrigin = m_basePos + dir * distance;
        m_waveDest = m_basePos + dir * 6.0f;
    }
    else
    {
        const float k = (c_coopHalfSize + 8.0f) / glm::max(glm::abs(dir.x), glm::abs(dir.z));
        m_waveOrigin = dir * k;
        m_waveDest = dir * 6.0f;
    }
    m_wavePendingBudget += budget;
    // Roll this wave's COMPOSITION among the archetypes the index has unlocked - never the same
    // recipe twice in a row when a choice exists - then jitter its weights so repeats still vary.
    int eligible[c_numWaveArchetypes];
    int numEligible = 0;
    const int level = m_sandboxWorld ? m_nightIndex : m_waveIndex; // 1-based: the recipes unlock by night there
    // The first night also rolls the recipes that name a wave of it (WaveArchetype::firstNight). m_nightWave is the
    // waves launched this night BEFORE this one (tickWaves counts it up after the queue).
    const bool firstNight = m_sandboxWorld && m_nightIndex == 1;
    for (int i = 0; i < c_numWaveArchetypes; ++i)
    {
        const WaveArchetype& arch = c_waveArchetypes[i];
        const bool unlocked = level >= arch.minWave || (firstNight && arch.firstNight >= 0 && m_nightWave >= arch.firstNight);
        if (unlocked && (i != m_lastArchetype || numEligible == 0))
            eligible[numEligible++] = i;
    }
    if (numEligible > 1 && eligible[0] == m_lastArchetype) // slot 0 was only a can't-be-empty seed
    {
        eligible[0] = eligible[numEligible - 1];
        --numEligible;
    }
    const int pick = eligible[glm::clamp((int)(glm::linearRand(0.0f, 1.0f) * (float)numEligible),
        0, numEligible - 1)];
    m_lastArchetype = pick;
    m_waveMix.clear();
    for (const auto& entry : c_waveArchetypes[pick].mix)
        if (entry.weight > 0.0f)
            m_waveMix.push_back({ entry.type, entry.weight * glm::linearRand(0.6f, 1.4f) });
    // Size the blob for EVERYTHING still queued (a previous wave's tail included) - once, with the
    // mix that will spend it, so the density holds from the first body to the last.
    m_waveRadius = waveSpawnRadius(m_wavePendingBudget);
    // One planned lane from the spawn ring to the Base - the swarm commits to it, and the units'
    // own periodic seed requests keep it fresh (Nav's proximity dedup makes the wave one plan).
    Globals::navSystem.seedPath(CoopAiTeam, m_waveOrigin, m_waveDest, laneSeedSpeed(), laneSeedWidth());
    Log::info(oc::format("Co-op: wave {} incoming - budget {:.0f} ({}) from ({:.0f}, {:.0f})", m_waveIndex,
        budget, c_waveArchetypes[pick].name, m_waveOrigin.x, m_waveOrigin.z));
    glm::vec3 fogPos = m_waveOrigin;
    float fogRadius = 0.0f; // 0 = no spawn fog (the flat maps)
    if (m_sandboxWorld)
    {
        fogPos.y = groundY(fogPos.x, fogPos.z, m_basePos.y);
        fogRadius = glm::min(m_waveRadius, m_settings.sandboxWaveDistance * 0.5f);
        addSpawnFog(fogPos, fogRadius);
    }
    if (m_isServer)
    {
        uint8 buffer[24];
        NetWriter writer(buffer);
        writer.write<uint16>((uint16)m_waveIndex);
        writer.write<float>(fogPos.x); writer.write<float>(fogPos.y); writer.write<float>(fogPos.z);
        writer.write<float>(fogRadius);
        Globals::networkManager.fireNetworkEvent("GWv", writer.data());
    }
}

// The trickle spawner (authority, per frame): a fixed budget of entity spawns serves the wave
// first, then the world-start ambient scatter - thousands of units enter the world over a few
// seconds instead of one giant frame hitch.
void GameMatch::tickCoopSpawns()
{
    // The frame's spawns are ROLLED first (budget math + RNG stay serial on main - glm's linearRand
    // is not thread-safe), then materialized in ONE NpcSystem::spawnLooseUnits batch: the entity
    // creations fan out over the job system instead of running one by one.
    ProfileScope scope("Coop spawn trickle", EProfileCategory::Game);
    oc::small_vector<NpcSystem::LooseSpawn, 64> spawns;
    int budget = glm::max(m_settings.spawnsPerFrame, 1);
    while (budget > 0 && m_wavePendingBudget > 0.0f)
    {
        --budget;
        // Roll the type from the wave's mix, then SPEND its cost. A roll the remaining budget
        // can't afford downgrades to the cheapest type in the mix; if even that doesn't fit, the
        // wave is done (the tail rounds down instead of overspending into the next wave's points).
        ENpcType type = sampleMix(m_waveMix);
        if (waveCostOf(type) > m_wavePendingBudget)
        {
            for (const WaveMixEntry& e : m_waveMix)
                if (waveCostOf(e.type) < waveCostOf(type))
                    type = e.type;
            if (waveCostOf(type) > m_wavePendingBudget)
            {
                m_wavePendingBudget = 0.0f;
                break;
            }
        }
        m_wavePendingBudget -= waveCostOf(type);
        // Cluster around the ring point in the blob queueWave sized for THIS wave's body count
        // (m_waveRadius - constant for the whole trickle, so the tail is as loose as the head).
        // The blob stays in the band OUTSIDE the barrier but inside the ground plane: a point that
        // drifted through the barrier line pushes back out along the wave's dominant axis, which
        // is what turns a big wave's oversized disc into a wide band along the barrier face.
        // A few rolls against the recent spawn points: a spot inside a body placed a moment ago
        // is rejected (parked bodies never push each other apart until a player is near).
        constexpr float c_spawnSpacing = 2.0f; // > two grunt radii
        glm::vec3 pos;
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            const float a = glm::linearRand(0.0f, glm::two_pi<float>());
            if (m_sandboxWorld)
            {
                // A disc kept clear of the Base (the blob would otherwise reach it), 1 m over the terrain.
                const float r = glm::min(m_waveRadius, m_settings.sandboxWaveDistance * 0.5f) * std::sqrt(glm::linearRand(0.0f, 1.0f));
                pos = m_waveOrigin + glm::vec3(std::cos(a) * r, 0.0f, std::sin(a) * r);
                pos.y = groundY(pos.x, pos.z, m_basePos.y) + 1.0f;
            }
            else
            {
                const float r = m_waveRadius * std::sqrt(glm::linearRand(0.0f, 1.0f)); // uniform by area
                pos = m_waveOrigin + glm::vec3(std::cos(a) * r, 1.0f, std::sin(a) * r);
                pos.x = glm::clamp(pos.x, -c_coopGroundEdge, c_coopGroundEdge);
                pos.z = glm::clamp(pos.z, -c_coopGroundEdge, c_coopGroundEdge);
                if (glm::abs(pos.x) < c_coopHalfSize + 1.5f && glm::abs(pos.z) < c_coopHalfSize + 1.5f)
                {
                    if (glm::abs(m_waveOrigin.x) >= glm::abs(m_waveOrigin.z))
                        pos.x = glm::sign(m_waveOrigin.x) * (c_coopHalfSize + 2.0f + glm::linearRand(0.0f, 10.0f));
                    else
                        pos.z = glm::sign(m_waveOrigin.z) * (c_coopHalfSize + 2.0f + glm::linearRand(0.0f, 10.0f));
                }
            }
            bool clear = true;
            for (int i = 0; i < m_waveRecentCount && clear; ++i)
                clear = glm::distance(m_waveRecent[i], glm::vec2(pos.x, pos.z)) >= c_spawnSpacing;
            if (clear)
                break;
        }
        m_waveRecent[m_waveRecentNext] = glm::vec2(pos.x, pos.z);
        m_waveRecentNext = (m_waveRecentNext + 1) % c_waveRecentSpawns;
        m_waveRecentCount = glm::min(m_waveRecentCount + 1, c_waveRecentSpawns);
        spawns.push_back({ .pos = pos,
            .orderDest = m_waveDest + glm::vec3(glm::linearRand(-4.0f, 4.0f), 0.0f,
                glm::linearRand(-4.0f, 4.0f)),
            .type = type, .team = (uint8)CoopAiTeam, .hasOrder = true });
    }
    while (budget > 0 && m_ambientPendingBudget > 0.0f)
    {
        --budget;
        // AMBIENT units come in small GROUPS: a cluster anchored on a random REACHABLE open cell
        // of the generated map (uniform by area, outside the safe ring, reachable from the Base
        // by the flood fill's guarantee), its bodies scattered in a disc around the anchor and
        // slid off any rock edge - a loose blob per group instead of one body per cell, which
        // lined up on the 10 m lattice. The Nav team fields never pull them (they cover the whole
        // map, which marched every scattered unit to the base) - only the local search aggroes
        // them, so an ambient group holds its patch until players expand near it. Wave units
        // above stay field-driven after their order releases.
        if (m_coopMap.reachable.empty())
            break;
        if (m_ambientSpawn.remaining <= 0)
        {
            const int cell = m_coopMap.reachable[glm::clamp(
                (int)(glm::linearRand(0.0f, 1.0f) * (float)m_coopMap.reachable.size()),
                0, (int)m_coopMap.reachable.size() - 1)];
            const glm::vec3 center = coopCellCenter(cell);
            if (glm::length(glm::vec2(center.x, center.z)) < m_settings.ambientSafeRadius)
                continue; // safe-ring reject: costs one budget tick, never the points
            // DISTANCE = DIFFICULTY: the group's archetype is gated by GEODESIC depth (BFS
            // distance from the Base over the generated map) exactly like waves gate by index -
            // the BAND is a window of recipes: a group rolls only recipes whose wave gate sits
            // within "Ambient recipe window" BELOW its depth band, so the near ring only rolls the
            // early recipes (swarm-grade) and the deep map rolls ONLY the elite-tier ones (giants,
            // titans, lobbers never appear near the Base, and the outer map never wastes a group
            // on a plain swarm). Costs then make far groups FEWER, TOUGHER bodies for the same
            // points. One recipe per group, so it reads as a unit type holding ground rather than
            // a random assortment.
            // "Ambient depth scale" < 1 reaches the top band before the map's deepest cell, so the
            // final tier (titans) is not confined to the corners: at 0.9 the mid-edges qualify.
            const float depth = glm::min((float)m_coopMap.depth[cell]
                / ((float)m_coopMap.maxDepth * m_settings.ambientDepthScale), 1.0f);
            const int band = 1 + (int)(depth * (float)(c_maxArchetypeMinWave - 1) + 0.5f);
            int eligible[c_numWaveArchetypes];
            int numEligible = 0;
            for (int i = 0; i < c_numWaveArchetypes; ++i)
                if (c_waveArchetypes[i].minWave <= band
                    && c_waveArchetypes[i].minWave >= band - m_settings.ambientRecipeWindow)
                    eligible[numEligible++] = i;
            if (numEligible == 0) // a gap in the gate table under the window: fall back to all unlocked
                for (int i = 0; i < c_numWaveArchetypes; ++i)
                    if (c_waveArchetypes[i].minWave <= band)
                        eligible[numEligible++] = i;
            m_ambientSpawn.archetype = eligible[glm::clamp(
                (int)(glm::linearRand(0.0f, 1.0f) * (float)numEligible), 0, numEligible - 1)];
            m_ambientSpawn.center = center + glm::vec3(glm::linearRand(-4.0f, 4.0f), 0.0f,
                glm::linearRand(-4.0f, 4.0f));
            m_ambientSpawn.radius = glm::linearRand(4.0f, 9.0f);
            m_ambientSpawn.remaining = (int)glm::linearRand(3.0f, 9.99f);
        }
        --m_ambientSpawn.remaining;
        ENpcType type = sampleArchetype(c_waveArchetypes[m_ambientSpawn.archetype]);
        if (waveCostOf(type) > m_ambientPendingBudget)
        {
            type = ENpcType::Swarm; // the tail rounds down to the cheapest body
            if (waveCostOf(type) > m_ambientPendingBudget)
            {
                m_ambientPendingBudget = 0.0f;
                break;
            }
        }
        m_ambientPendingBudget -= waveCostOf(type);
        spawns.push_back({ .pos = ambientPointNear(m_ambientSpawn.center, m_ambientSpawn.radius),
            .type = type, .team = (uint8)CoopAiTeam });
    }
    m_npcs.spawnLooseUnits(oc::span<const NpcSystem::LooseSpawn>(spawns.data(), spawns.size()));
}

// AMBIENT WANDER (co-op authority): an IDLE AI unit now and then takes a short stroll, its heading
// biased toward the Base. Performance: the EXPECTED number of strolls this frame is unit count /
// "Ambient wander interval" × dt (5000 units at 90 s and 60 fps = ~0.93 a frame), carried as a
// fractional budget so every frame issues that many on average - a steady trickle instead of a
// burst - and each stroll costs at most c_wanderScanCap random probes into the World's root list
// (every unit is a root; a non-unit hit is a wasted probe) to find an idle unit. No per-unit
// timer. The order is a wander (orderWander): it never seeds a lane and
// self-expires after "Ambient wander timeout", so a target behind rock cannot pin the unit. Far
// (LOD-skipped) units walk it through the far tick's teleport like any order. Hunting, locked or
// routing units are skipped as candidates. Runs on a POST-UPDATE job (see update): its own RNG,
// since the shared C rand is not a thing to share with main.
void GameMatch::tickAmbientWander(float deltaSec)
{
    if (!m_coop || m_settings.ambientWanderInterval <= 0.0f)
        return;
    // The root list only mutates on main, after this post-update job joins.
    const oc::vector<EntityPtr>& roots = Globals::world.rootEntities();
    const int n = (int)roots.size();
    const int aliveUnits = NpcSystem::countUnits(); // the component's edge-maintained count (any thread)
    if (n == 0 || aliveUnits == 0)
        return;
    const auto rand01 = [&] { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_wanderRng); };
    // The budget is sized from the SELECTED units, not all of them: only selected units are
    // candidates, so a whole-population budget would land every far unit's strolls on the few
    // near a player. The selected fraction is estimated from the random unit probes below (each
    // is a fair sample), smoothed - no walk.
    m_wanderBudget += (float)aliveUnits * m_wanderSelectedFrac * deltaSec / m_settings.ambientWanderInterval;
    int issue = (int)m_wanderBudget;
    if (issue <= 0)
        return;
    m_wanderBudget -= (float)issue;
    constexpr int c_wanderScanCap = 32; // random probes per stroll before giving up this frame
    glm::vec3 basePos(0.0f);
    for (int i = 0; i < m_structures.structureCount(); ++i)
        if (m_structures.structureType(i) == EStructureType::Base && m_structures.structureTeam(i) == 0)
        {
            basePos = m_structures.structurePos(i);
            break;
        }
    for (; issue > 0; --issue)
    {
        // Pre-emption point every 16 strolls (a stroll is up to 32 probes plus a ground clamp):
        // a Normal post-update job, so High work gets through. A stroll is issued whole.
        if ((issue & 15) == 0)
            Globals::jobSystem.preemptionPoint();
        GameUnitComponent* u = nullptr;
        Entity* e = nullptr;
        for (int scan = 0; scan < c_wanderScanCap && !u; ++scan)
        {
            e = roots[glm::clamp((int)(rand01() * (float)n), 0, n - 1)].get();
            u = e ? getComponent<GameUnitComponent>(e) : nullptr;
            if (!u || u->puppet)
            {
                u = nullptr; // not a unit (structure, rock, shot, player capsule): no sample either
                continue;
            }
            // Only SELECTED units (inside the SIM LOD's outer tier of some player) stroll: a far
            // unit is invisible and would walk its order by the far tick's teleport, then arrive
            // in view mid-stroll - a whole patch "starting to wander" the moment a player came
            // near. Tier 2 and closer all behave alike.
            const bool selected = Globals::world.simLodSelected(*e);
            m_wanderSelectedFrac += ((selected ? 1.0f : 0.0f) - m_wanderSelectedFrac) * 0.02f;
            if (u->team != CoopAiTeam || !u->alive() || u->targetLocked || u->hasTarget
                || u->routeIndex < u->routeCount || !selected)
                u = nullptr;
        }
        if (!u)
            continue; // no idle unit found in the probe budget: this stroll is simply dropped
        const float angle = rand01() * 6.2831853f;
        glm::vec2 dir(glm::cos(angle), glm::sin(angle));
        const glm::vec2 toBase(basePos.x - e->pos.x, basePos.z - e->pos.z);
        if (const float len = glm::length(toBase); len > 1e-3f)
            dir += toBase / len * m_settings.ambientWanderBaseBias; // the bias tilts the stroll toward the Base
        if (glm::dot(dir, dir) < 1e-4f)
            continue;
        dir = glm::normalize(dir);
        const float dist = (0.4f + 0.6f * rand01()) * m_settings.ambientWanderDistance;
        const glm::vec3 target = clampToOpenGround(e->pos + glm::vec3(dir.x, 0.0f, dir.y) * dist);
        u->orderWander(target, m_settings.ambientWanderTimeout);
    }
}

// ---- the trickle's save state ----

// The PENDING TRICKLE: a wave is sized in points at queueWave and the bodies materialize over the
// following frames (tickCoopSpawns, "Spawns per frame"). Saving mid-wave used to drop everything
// not yet spawned, so an F9 during a big wave quietly shrank it. Everything the trickle reads is
// written here - the remaining points, WHERE they enter (m_waveOrigin) and march (m_waveDest), and
// the wave's ROLLED, JITTERED mix, which cannot be re-derived from the archetype table. The
// spacing ring (m_waveRecent) is deliberately left out: it only rejects spots for a few spawns.
void GameMatch::saveTrickle(AssetNode& root) const
{
    AssetNode& wave = root.addChild("WaveTrickle");
    wave.set("Pending", m_wavePendingBudget);
    wave.set("Origin", m_waveOrigin);
    wave.set("Dest", m_waveDest);
    wave.set("Radius", m_waveRadius); // the blob queueWave sized for this wave's body count
    wave.set("LastArchetype", oc::to_string(m_lastArchetype)); // keeps "never twice in a row"
    for (const WaveMixEntry& e : m_waveMix)
    {
        AssetNode& entry = wave.addChild("Mix"); // Type is the raw ENpcType int - append-only
        entry.values = { oc::to_string((int)e.type), oc::to_string(e.weight) };
    }
    // The world-start SCATTER trickles through the same budget, plus the group it is mid-way
    // through placing (anchored on this map's cells, which the load regenerates identically).
    AssetNode& ambient = root.addChild("AmbientTrickle");
    ambient.set("Pending", m_ambientPendingBudget);
    ambient.set("Center", m_ambientSpawn.center);
    ambient.set("Radius", m_ambientSpawn.radius);
    ambient.set("Remaining", oc::to_string(m_ambientSpawn.remaining));
    ambient.set("Archetype", oc::to_string(m_ambientSpawn.archetype));
}

// Counterpart of saveTrickle. MUST run after rebuildCoopMap, which voids the in-progress ambient
// group (its anchor belongs to the old map). A save with NO trickle nodes - every save before this
// existed - clears both budgets: that file IS the complete state, and letting the running session's
// own scatter continue on top of it would double-spawn.
void GameMatch::loadTrickle(const AssetNode& root)
{
    m_wavePendingBudget = 0.0f;
    m_ambientPendingBudget = 0.0f;
    m_ambientSpawn.remaining = 0;
    if (const AssetNode* wave = root.find("WaveTrickle"))
    {
        m_wavePendingBudget = glm::max(wave->find("Pending") ? wave->find("Pending")->asFloat() : 0.0f, 0.0f);
        m_waveOrigin = wave->find("Origin") ? wave->find("Origin")->asVec3() : m_waveOrigin;
        m_waveDest = wave->find("Dest") ? wave->find("Dest")->asVec3() : m_waveDest;
        m_lastArchetype = wave->find("LastArchetype") ? wave->find("LastArchetype")->asInt() : -1;
        m_waveMix.clear();
        for (const AssetNode* entry : wave->findAll("Mix"))
        {
            if (m_waveMix.size() >= m_waveMix.capacity())
                break; // fixed_vector: a hand-edited save cannot overrun it
            const int type = glm::clamp(entry->asInt(0), 0, (int)ENpcType::Count - 1);
            m_waveMix.push_back({ (ENpcType)type, glm::max(entry->asFloat(1), 0.0f) });
        }
        // Older trickle saves have no Radius: re-derive it from what is LEFT, which is the best
        // this end can do - the original wave's total is not in the file.
        m_waveRadius = wave->find("Radius") ? glm::clamp(wave->find("Radius")->asFloat(), 8.0f, c_coopGroundEdge)
                                            : waveSpawnRadius(m_wavePendingBudget);
        if (m_waveMix.empty())
            m_wavePendingBudget = 0.0f; // nothing to sample: dropping the points beats a wrong mix
        else if (m_wavePendingBudget > 0.0f) // re-seed the lane the trickled units will follow
            Globals::navSystem.seedPath(CoopAiTeam, m_waveOrigin, m_waveDest, laneSeedSpeed(), laneSeedWidth());
    }
    if (const AssetNode* ambient = root.find("AmbientTrickle"))
    {
        m_ambientPendingBudget = glm::max(ambient->find("Pending") ? ambient->find("Pending")->asFloat() : 0.0f, 0.0f);
        m_ambientSpawn.center = ambient->find("Center") ? ambient->find("Center")->asVec3() : glm::vec3(0.0f);
        m_ambientSpawn.radius = glm::max(ambient->find("Radius") ? ambient->find("Radius")->asFloat() : 6.0f, 0.0f);
        m_ambientSpawn.remaining = glm::max(ambient->find("Remaining") ? ambient->find("Remaining")->asInt() : 0, 0);
        m_ambientSpawn.archetype = glm::clamp(ambient->find("Archetype") ? ambient->find("Archetype")->asInt() : 0,
            0, c_numWaveArchetypes - 1);
    }
}
