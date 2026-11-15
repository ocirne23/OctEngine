module Game;

import Core;
import Core.glm;
import Core.Transform;
import Entity;
import Physics;
import Settings;
import :Ship;

// THE SHIP's flight (see Ship.ixx). The state is GameShip's own; the kinematic body and the entity follow it.

namespace
{
    constexpr const char* c_shipPrefab = "Entities/Game/airship.pre";

    // THE OUTLINE: the half-width in 2 m tiles per 2 m row, from the bow (z -DeckHalfLength) to the stern. airship.obj's
    // generator used the same table. A bow that narrows a tile every two rows, the widest beam a little forward of the
    // middle, then a tile in, and another for a broad flat stern.
    constexpr int c_rows[] = { 1, 2, 2, 3, 3, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 3, 3 };
    constexpr int c_rowCount = (int)(sizeof(c_rows) / sizeof(c_rows[0]));
    constexpr float c_tile = 2.0f;
    static_assert(c_rowCount * c_tile == 2.0f * GameShip::DeckHalfLength, "the row table spans the deck's length");

    float moveTowards(float value, float target, float step)
    {
        return value < target ? glm::min(value + step, target) : glm::max(value - step, target);
    }
    static_assert(c_rowCount == GameShip::DeckRows);
}

int GameShip::rowHalfTiles(int row)
{
    return row >= 0 && row < c_rowCount ? c_rows[row] : 0;
}

bool GameShip::deckCell(int cx, int cz)
{
    const int half = rowHalfTiles(cz + DeckRows / 2);
    return half > 0 && cx >= -half && cx <= half;
}

oc::span<const GameShip::Entrance> GameShip::entrances()
{
    // Two tiles on each side amidships (z 4 .. 8, behind the Beacon; the widest rows: tiles -5 .. 5), one on each side
    // forward (z -16 .. -14; tiles -4 .. 4) and one in the middle of the stern (x 0 .. 2, z 26 .. 28).
    static constexpr Entrance c_entrances[] = {
        { { 5, 2 }, 2, { 1, 0 } }, { { -5, 2 }, 2, { -1, 0 } },
        { { 4, -8 }, 1, { 1, 0 } }, { { -4, -8 }, 1, { -1, 0 } },
        { { 0, 13 }, 1, { 0, 1 } } };
    return c_entrances;
}

bool GameShip::entranceCell(int cx, int cz)
{
    for (const Entrance& e : entrances())
    {
        const glm::ivec2 along = e.out.x != 0 ? glm::ivec2(0, 1) : glm::ivec2(1, 0);
        for (int i = 0; i < e.count; ++i)
            if (e.first + along * i == glm::ivec2(cx, cz))
                return true;
    }
    return false;
}

bool GameShip::helmCell(int cx, int cz)
{
    return cx == 0 && cz == 1; // x 0 .. 2, z 2 .. 4: right behind the Beacon (its tiles end at z 2)
}

uint8 GameShip::railSides(int cx, int cz)
{
    if (!deckCell(cx, cz) || entranceCell(cx, cz))
        return 0;
    return (uint8)((deckCell(cx + 1, cz) ? 0 : 1) | (deckCell(cx - 1, cz) ? 0 : 2) | (deckCell(cx, cz + 1) ? 0 : 4)
        | (deckCell(cx, cz - 1) ? 0 : 8));
}

int GameShip::quarterTurnsOf(float yaw)
{
    const int turns = (int)std::lround(yaw / glm::half_pi<float>());
    return ((turns % 4) + 4) % 4;
}

glm::vec3 GameShip::snapCentre(const glm::vec3& pos)
{
    return glm::vec3(std::round(pos.x * 0.5f) * 2.0f, pos.y, std::round(pos.z * 0.5f) * 2.0f);
}

void GameShip::setStructureBoxes(oc::span<const Box> boxes)
{
    m_structureBoxes.assign(boxes.begin(), boxes.end());
    if (m_body.isValid())
    {
        m_body = PhysicsBody();
        makeBody();
    }
}

void GameShip::land(const glm::vec3& centre, int quarterTurns)
{
    m_landPos = centre;
    m_landTurns = quarterTurns & 3;
    m_land = ELand::Settling;
    m_throttle = 0.0f;
    m_travelY = glm::max(m_pos.y, centre.y + 12.0f); // a far site: over there at this height first, then down
}

void GameShip::landAt(const glm::vec3& centre, int quarterTurns)
{
    m_landPos = centre;
    m_landTurns = quarterTurns & 3;
    m_land = ELand::Landed;
    m_pos = centre;
    m_yaw = yawOfQuarterTurns(m_landTurns);
    m_rot = glm::angleAxis(m_yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    m_vel = glm::vec2(0.0f);
    m_vy = m_yawRate = m_throttle = m_forwardSpeed = m_pitch = m_bank = 0.0f;
    m_linear = m_spin = glm::vec3(0.0f);
    if (m_body.isValid())
    {
        Globals::physics.teleportBody(m_body, m_pos, m_rot);
        m_body.setLinearVelocity(glm::vec3(0.0f));
        m_body.setAngularVelocity(glm::vec3(0.0f));
    }
    if (m_entity)
    {
        m_entity->pos = m_pos;
        m_entity->rot = m_rot;
    }
}

void GameShip::setWantedHeight(float keelOverGround)
{
    const GameShipSettings& s = Globals::settings.game.ship;
    m_height = glm::clamp(keelOverGround, s.minHeight, glm::max(s.maxHeight, s.minHeight));
}

void GameShip::takeOff()
{
    if (m_land == ELand::Flying)
        return;
    m_land = ELand::Flying;
    m_vel = glm::vec2(0.0f);
    m_vy = m_yawRate = m_throttle = 0.0f;
    // Back up to the height it flew at before it landed (land() keeps m_height); the hull is in the ground, the floor
    // push lifts it out.
    const GameShipSettings& s = Globals::settings.game.ship;
    m_height = glm::clamp(m_height, s.minHeight + 2.0f, glm::max(s.maxHeight, s.minHeight + 2.0f));
}

void GameShip::makeBody()
{
    // FROM THE OUTLINE (c_rows, the same table as airship.obj): per tier, one box per run of equal half-width - the deck
    // tier (y 0 .. -2), then two tiers that step in by a tile each side and start further back at the bow than at the
    // stern (the stem rakes) - plus the keel. THE WALL is not the hull's: it is the ShipRail structures on the edge
    // tiles, whose boxes the body carries while the ship flies (m_structureBoxes, with every other structure on the
    // deck); landed, they have their own colliders.
    const auto box = [](const glm::vec3& half, const glm::vec3& offset)
    {
        PhysicsShape s;
        s.type = EPhysicsShapeType::Box;
        s.halfExtents = half;
        s.offset = offset;
        s.friction = 0.9f;
        return s;
    };
    struct Tier { int shrink, trimBow, trimStern; float y; };
    constexpr Tier c_tiers[] = { { 0, 0, 0, -1.0f }, { 1, 2, 1, -3.0f }, { 2, 5, 2, -5.0f } };
    const auto halfTiles = [](const Tier& t, int row)
    {
        return row < t.trimBow || row >= c_rowCount - t.trimStern ? 0 : glm::max(c_rows[row] - t.shrink, 1);
    };
    const float z0 = -DeckHalfLength;
    oc::vector<PhysicsShape> shapes;
    for (const Tier& t : c_tiers)
    {
        for (int i = 0; i < c_rowCount;)
        {
            const int h = halfTiles(t, i);
            int j = i;
            while (j + 1 < c_rowCount && halfTiles(t, j + 1) == h)
                ++j;
            const float hx = h * c_tile + 0.5f * c_tile, za = z0 + i * c_tile, zb = z0 + (j + 1) * c_tile; // the tiles -h .. h
            const float zc = 0.5f * (za + zb), hz = 0.5f * (zb - za);
            if (h > 0)
                shapes.push_back(box(glm::vec3(hx, 1.0f, hz), glm::vec3(DeckCentreX, t.y, zc)));
            i = j + 1;
        }
    }
    const float keelA = z0 + 6 * c_tile, keelB = z0 + (c_rowCount - 4) * c_tile;
    shapes.push_back(box(glm::vec3(1.0f, 0.5f, 0.5f * (keelB - keelA)), glm::vec3(DeckCentreX, -6.5f, 0.5f * (keelA + keelB))));   // the keel
    for (const Box& b : m_structureBoxes) // the structures on the deck (while it flies)
        shapes.push_back(box(b.half, b.center));
    PhysicsBodyDesc desc;
    desc.type = EPhysicsBodyType::Kinematic;
    desc.transform = Transform(m_pos, 1.0f, m_rot);
    desc.userData = m_entity.get();
    m_body = Globals::physics.createBody(desc, oc::span<const PhysicsShape>(shapes.data(), shapes.size()));
}

void GameShip::spawn(const glm::vec3& deckPos, float yaw)
{
    despawn();
    m_pos = deckPos;
    m_yaw = yaw;
    m_rot = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    m_vel = glm::vec2(0.0f);
    m_vy = m_yawRate = m_throttle = m_forwardSpeed = m_pitch = m_bank = 0.0f;
    m_height = Globals::settings.game.ship.minHeight + 4.0f;
    m_land = ELand::Flying;
    m_structureBoxes.clear();
    m_entity = Globals::world.spawnAssetFile(c_shipPrefab, Transform(m_pos, 1.0f, m_rot), true);
    if (!m_entity)
        return;
    m_entity->setName("Ship");
    Globals::world.addRootEntity(m_entity);
    makeBody();
}

void GameShip::despawn()
{
    m_body = PhysicsBody();
    if (m_entity)
        Globals::world.removeRootEntity(m_entity.get());
    m_entity = EntityPtr();
}

void GameShip::teleport(const glm::vec3& deckPos, float yaw)
{
    if (!exists())
        return;
    m_pos = deckPos;
    m_yaw = yaw;
    m_rot = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    m_vel = glm::vec2(0.0f);
    m_vy = m_yawRate = m_throttle = m_forwardSpeed = 0.0f;
    m_land = ELand::Flying;
    if (m_body.isValid())
    {
        Globals::physics.teleportBody(m_body, m_pos, m_rot);
        m_body.setLinearVelocity(glm::vec3(0.0f));
        m_body.setAngularVelocity(glm::vec3(0.0f));
    }
    m_entity->pos = m_pos;
    m_entity->rot = m_rot;
}

glm::vec3 GameShip::pointVelocity(const glm::vec3& world) const
{
    return m_linear + glm::cross(m_spin, world - m_pos);
}

bool GameShip::onDeck(const glm::vec3& world, float above) const
{
    const glm::vec3 l = toLocal(world);
    return glm::abs(l.x - DeckCentreX) <= DeckHalfWidth && glm::abs(l.z) <= DeckHalfLength && l.y >= -0.5f && l.y <= above;
}

// THE FLIGHT. Horizontal: a throttle LEVER (W / S move it, it holds where it is) pushes along the bow against a quadratic
// drag (so the thrust and the drag meet at the top speed), the sideways slip dies on the keel, and the rudder spins the
// hull up against its inertia. Vertical: a spring-damper hover toward the wanted height over the HIGHEST ground under
// the hull and ahead of it along the velocity (it climbs before a hill; a hill it cannot climb in time brakes it).
// THE SEA: over deep water at the sea level the magic does not hold - it settles on the water and sails, slower; rivers
// and lakes are shallow or high and do not count. The look: it banks into a turn, pitches with the acceleration, bobs.
void GameShip::tick(float deltaSec, const Input& input, const GroundFn& ground)
{
    if (!exists() || deltaSec <= 0.0f)
        return;
    const GameShipSettings& s = Globals::settings.game.ship;
    const float dt = glm::min(deltaSec, 0.1f);
    m_time += dt;
    if (m_land != ELand::Flying)
    {
        tickGrounded(dt, ground);
        return;
    }

    // ---- the ground and the water under the hull ----
    const glm::vec3 fwd(-std::sin(m_yaw), 0.0f, -std::cos(m_yaw));
    const glm::vec3 right(std::cos(m_yaw), 0.0f, -std::sin(m_yaw));
    const bool waterActive = Globals::physics.isWaterActive();
    const float seaLevel = Globals::settings.terrain.seaLevel;
    struct Sample { float ground, water, surface; bool deep, dry; };
    const auto sample = [&](const glm::vec3& at)
    {
        Sample r;
        r.ground = ground(at.x, at.z, m_pos.y - KeelDepth - m_height);
        r.water = waterActive ? Globals::physics.sampleWaterHeight(at.x, at.z) : -FLT_MAX;
        // THE SEA by its BED, not by its surface: waves move the surface (a high crest is far over the sea level), never
        // the ground. A seabed "Deep water (m)" under the sea level, with water over it, is the sea; a river's or a
        // lake's bed lies over the sea level.
        r.deep = r.water > r.ground + 1.0f && r.ground < seaLevel - s.deepWater;
        r.dry = r.water < r.ground + 0.5f;
        r.surface = glm::max(r.ground, r.water); // water that is not deep is a surface to hover over like the ground
        return r;
    };
    // UNDER = the highest surface under the hull (deep water under a corner is no surface to lift over), LAND = the
    // highest DRY ground under it (what lifts a sailing ship out of the water at a shore), AHEAD = UNDER and the two
    // samples ahead along the velocity.
    float under = -FLT_MAX, land = -FLT_MAX, ahead = -FLT_MAX;
    const Sample centre = sample(m_pos);
    {
        under = centre.surface;
        if (centre.dry)
            land = centre.ground;
        // The bow and the stern tips, the widest beam's front corners (z -12) and the rear's corners (z +22).
        constexpr float c_tipZ = DeckHalfLength - 1.0f, c_beamX = DeckHalfWidth - 1.0f;
        const glm::vec3 corners[] = { fwd * c_tipZ, -fwd * c_tipZ, right * c_beamX + fwd * 12.0f, -right * c_beamX + fwd * 12.0f,
            right * 7.0f - fwd * 22.0f, -right * 7.0f - fwd * 22.0f };
        for (const glm::vec3& c : corners)
        {
            const Sample h = sample(m_pos + c);
            if (!h.deep)
                under = glm::max(under, h.surface);
            if (h.dry)
                land = glm::max(land, h.ground);
        }
        const glm::vec3 lead(m_vel.x * s.lookAhead, 0.0f, m_vel.y * s.lookAhead);
        const Sample a = sample(m_pos + lead), b = sample(m_pos + fwd * (DeckHalfLength - 1.0f) + lead);
        ahead = glm::max(under, glm::max(a.deep ? -FLT_MAX : a.surface, b.deep ? -FLT_MAX : b.surface));
    }
    // In at once, out only after a second off deep water: a shallow patch or a trough does not throw it into the air.
    m_seaLeave = centre.deep ? 0.0f : m_seaLeave + dt;
    m_sailing = centre.deep || (m_sailing && m_seaLeave < 1.0f);
    // THE WAVES (sailing): the water surface at the bow, the stern and both sides - the hull floats on them, pitches with
    // the bow / stern difference and rolls with the side difference.
    float waterMean = centre.water, wavePitch = 0.0f, waveRoll = 0.0f;
    if (m_sailing)
    {
        const auto waterAt = [&](const glm::vec3& at)
        {
            const float w = Globals::physics.sampleWaterHeight(at.x, at.z);
            return w > -1e30f ? w : centre.water;
        };
        constexpr float c_waveZ = DeckHalfLength - 4.0f, c_waveX = DeckHalfWidth;
        const float wBow = waterAt(m_pos + fwd * c_waveZ), wStern = waterAt(m_pos - fwd * c_waveZ);
        const float wLeft = waterAt(m_pos - right * c_waveX), wRight = waterAt(m_pos + right * c_waveX);
        waterMean = (wBow + wStern + wLeft + wRight + centre.water) * 0.2f;
        wavePitch = glm::degrees(std::atan2(wBow - wStern, 2.0f * c_waveZ)) * s.waveFollow;   // the bow on a crest: nose up
        waveRoll = glm::degrees(std::atan2(wRight - wLeft, 2.0f * c_waveX)) * s.waveFollow;    // the left side in a trough: it rolls left
    }

    // ---- the lever and the rudder ----
    const float leverTarget = !input.piloted ? 0.0f : input.throttle > 0.0f ? 1.0f : input.throttle < 0.0f ? -0.35f : m_throttle;
    m_throttle = moveTowards(m_throttle, leverTarget, s.throttleRate * dt * (input.piloted ? 1.0f : 2.0f));
    const float cap = glm::max(s.maxSpeed * (m_sailing ? s.seaSpeedMult : 1.0f), 0.5f);
    const float thrust = s.thrust * (m_throttle >= 0.0f ? m_throttle : m_throttle * (1.0f / 3.0f) / 0.35f);
    float vf = glm::dot(glm::vec3(m_vel.x, 0.0f, m_vel.y), fwd);
    float vs = glm::dot(glm::vec3(m_vel.x, 0.0f, m_vel.y), right);
    const float drag = s.thrust / (cap * cap);
    float accForward = thrust - drag * vf * glm::abs(vf) - 0.05f * vf;
    // A hill it cannot climb in time: the ground ahead stands over the keel by more than it can rise to -> brake.
    const float keel = m_pos.y - KeelDepth;
    const float wall = ahead + s.minHeight * 0.5f - keel;
    if (wall > 6.0f && vf > 0.0f)
        accForward -= glm::min(wall - 6.0f, 6.0f) * 0.6f * vf;
    vf += accForward * dt;
    vs *= std::exp(-s.sideDrag * dt);
    const float speedFactor = 0.35f + 0.65f * glm::clamp(glm::abs(vf) / cap, 0.0f, 1.0f);
    const float turnTarget = input.piloted ? glm::clamp(input.turn, -1.0f, 1.0f) * s.turnRate * speedFactor : 0.0f;
    m_yawRate = moveTowards(m_yawRate, turnTarget, s.turnAccel * dt);
    if (turnTarget == 0.0f)
        m_yawRate *= std::exp(-s.turnDrag * dt);
    const float yawStep = glm::radians(m_yawRate) * dt;
    m_yaw += yawStep;
    // The velocity turns with the hull (the keel holds it along the bow): re-express it in the new frame.
    const glm::vec3 fwd2(-std::sin(m_yaw), 0.0f, -std::cos(m_yaw));
    const glm::vec3 right2(std::cos(m_yaw), 0.0f, -std::sin(m_yaw));
    const glm::vec3 v3 = fwd2 * vf + right2 * vs;
    m_vel = glm::vec2(v3.x, v3.z);
    m_forwardSpeed = vf;

    // ---- the height ----
    if (m_sailing)
        m_height = s.minHeight; // the magic does not hold over the sea: the lever for height does nothing
    else
        m_height = glm::clamp(m_height + glm::clamp(input.piloted ? input.climb : 0.0f, -1.0f, 1.0f) * s.climbRate * dt,
            s.minHeight, glm::max(s.maxHeight, s.minHeight));
    // The hover bob in the air; at sea the waves themselves move it.
    const float bob = m_sailing ? 0.0f : std::sin(m_time * glm::two_pi<float>() / glm::max(s.bobPeriod, 0.5f)) * s.bobHeight;
    float targetY = m_sailing ? waterMean + s.freeboard : ahead + m_height + KeelDepth + bob;
    if (m_sailing && land > -1e30f) // its bow over a shore: it lifts out of the water over the DRY land
        targetY = glm::max(targetY, land + s.minHeight + KeelDepth);
    // Afloat it follows the water far more firmly than it hovers (the water carries the hull).
    const float stiffness = m_sailing ? s.seaStiffness : s.verticalStiffness;
    const float damping = m_sailing ? s.seaDamping : s.verticalDamping;
    const float maxAccY = glm::max(s.maxVerticalAccel * (m_sailing ? 2.0f : 1.0f), 0.1f);
    const float accY = glm::clamp(stiffness * (targetY - m_pos.y) - damping * m_vy, -maxAccY, maxAccY);
    m_vy += accY * dt;
    glm::vec3 next = m_pos + glm::vec3(m_vel.x, m_vy, m_vel.y) * dt;
    // Never INTO the ground (or deep into the water when sailing): under that floor it is PUSHED up hard - never moved:
    // a position snap is a teleport (a mode switch or a wave moves the floor by metres in one tick). The push is never
    // faster than the hover can brake before the wanted height (0.8 of its acceleration): a lift-off from the ground
    // (the hull deep in it) would otherwise fly far past the height and sink back.
    const float floor = (m_sailing ? waterMean + s.freeboard - 1.5f : under + 0.5f + KeelDepth);
    if (next.y < floor)
    {
        const float brakeable = std::sqrt(2.0f * 0.8f * maxAccY * glm::max(targetY - next.y, 0.0f));
        m_vy = glm::max(m_vy, glm::min(glm::min((floor - next.y) * 3.0f, 12.0f), glm::max(brakeable, 1.0f)));
    }

    // ---- the look: bank into the turn, pitch with the acceleration (the deck tilts with it) ----
    const float speedShare = glm::clamp(glm::abs(vf) / cap, 0.0f, 1.0f);
    const float bankTarget = glm::clamp(m_yawRate * s.bankPerTurn * speedShare, -s.maxBank, s.maxBank)
        + (m_sailing ? waveRoll : std::sin(m_time * 0.37f) * 0.3f);
    const float pitchTarget = glm::clamp((accForward + (m_sailing ? 0.0f : accY * 0.5f)) * s.pitchPerAccel, -s.maxPitch, s.maxPitch)
        + wavePitch;
    const float smooth = 1.0f - std::exp(-(m_sailing ? 3.0f : 2.0f) * dt);
    m_bank += (bankTarget - m_bank) * smooth;
    m_pitch += (pitchTarget - m_pitch) * smooth;
    const glm::quat nextRot = glm::angleAxis(m_yaw, glm::vec3(0.0f, 1.0f, 0.0f))
        * glm::angleAxis(glm::radians(m_pitch), glm::vec3(1.0f, 0.0f, 0.0f))
        * glm::angleAxis(glm::radians(m_bank), glm::vec3(0.0f, 0.0f, 1.0f));

    driveBody(next, nextRot, glm::vec3(m_vel.x, m_vy, m_vel.y), glm::radians(m_yawRate));
}

// LANDING: the settle toward the landed pose (the flight is off), then the pose held. A FAR site first: TRAVEL over to
// it at the cruise height (m_travelY, raised over the ground under and ahead of the hull), then the set-down. The deck
// tilts back level and the yaw turns to the quarter turn on the way.
void GameShip::tickGrounded(float dt, const GroundFn& ground)
{
    const float targetYaw = m_yaw + std::remainder(yawOfQuarterTurns(m_landTurns) - m_yaw, glm::two_pi<float>());
    glm::vec3 next = m_landPos;
    float nextYaw = targetYaw;
    constexpr float c_maxTurn = 0.5f; // rad/s
    const glm::vec2 gap(m_landPos.x - m_pos.x, m_landPos.z - m_pos.z);
    const float distance = glm::length(gap);
    if (m_land == ELand::Settling && distance > 2.0f)
    {
        constexpr float c_travelSpeed = 20.0f; // m/s; slower over the last stretch
        const glm::vec2 dir = gap / distance;
        const float speed = glm::min(c_travelSpeed, 1.0f + distance * 0.6f);
        const glm::vec2 stepXZ = dir * glm::min(speed * dt, distance);
        const float clearance = KeelDepth + glm::max(Globals::settings.game.ship.minHeight, 2.0f) + 2.0f;
        const float under = ground(m_pos.x, m_pos.z, m_landPos.y);
        const float ahead = ground(m_pos.x + dir.x * 25.0f, m_pos.z + dir.y * 25.0f, m_landPos.y);
        m_travelY = glm::max(m_travelY, glm::max(under, ahead) + clearance);
        const float dy = glm::clamp(m_travelY - m_pos.y, -4.0f * dt, 6.0f * dt);
        next = m_pos + glm::vec3(stepXZ.x, dy, stepXZ.y);
        const float k = 1.0f - std::exp(-2.0f * dt);
        nextYaw = m_yaw + glm::clamp((targetYaw - m_yaw) * k, -c_maxTurn * dt, c_maxTurn * dt);
        m_pitch *= 1.0f - k;
        m_bank *= 1.0f - k;
    }
    else if (m_land == ELand::Settling)
    {
        constexpr float c_rate = 1.5f;      // 1/s: the gap closes like this (slow at the end: it sets down)
        constexpr float c_maxSpeed = 10.0f; // m/s
        constexpr float c_minSpeed = 1.5f;  // m/s, and c_minTurn rad/s: no slow tail - the last metres take no seconds
        constexpr float c_minTurn = 0.15f;
        const float k = 1.0f - std::exp(-c_rate * dt);
        const glm::vec3 gap3 = m_landPos - m_pos;
        const float gapLen = glm::length(gap3);
        const float stepLen = glm::min(glm::clamp(gapLen * k, c_minSpeed * dt, c_maxSpeed * dt), gapLen);
        next = gapLen > 1e-6f ? m_pos + gap3 * (stepLen / gapLen) : m_landPos;
        const float yawGap = targetYaw - m_yaw;
        const float yawStep = glm::min(glm::clamp(glm::abs(yawGap) * k, c_minTurn * dt, c_maxTurn * dt), glm::abs(yawGap));
        nextYaw = m_yaw + (yawGap < 0.0f ? -yawStep : yawStep);
        m_pitch *= 1.0f - k;
        m_bank *= 1.0f - k;
        if (glm::length(m_landPos - next) < 0.03f && glm::abs(targetYaw - nextYaw) < 0.002f)
        {
            next = m_landPos;
            nextYaw = targetYaw;
            m_land = ELand::Landed;
        }
    }
    if (m_land == ELand::Landed)
        m_pitch = m_bank = 0.0f;
    const float yawRate = (nextYaw - m_yaw) / dt;
    const glm::vec3 velocity = (next - m_pos) / dt;
    m_yaw = m_land == ELand::Landed ? yawOfQuarterTurns(m_landTurns) : nextYaw;
    m_vel = glm::vec2(0.0f);
    m_vy = m_yawRate = m_forwardSpeed = 0.0f;
    const glm::quat nextRot = glm::angleAxis(m_yaw, glm::vec3(0.0f, 1.0f, 0.0f))
        * glm::angleAxis(glm::radians(m_pitch), glm::vec3(1.0f, 0.0f, 0.0f))
        * glm::angleAxis(glm::radians(m_bank), glm::vec3(0.0f, 0.0f, 1.0f));
    driveBody(next, nextRot, m_land == ELand::Landed ? glm::vec3(0.0f) : velocity, m_land == ELand::Landed ? 0.0f : yawRate);
}

void GameShip::driveBody(const glm::vec3& next, const glm::quat& nextRot, const glm::vec3& velocity, float yawRate)
{
    // ---- the body: the ship's OWN velocity, plus a gentle pull onto the pose ----
    // NOT "the gap over this tick": the physics step can be longer than a frame (several frames, no step, then one), so
    // a velocity sized to close the gap in one FRAME overshoots by steps/frames and the error grows each step - the
    // collider swings ever faster (the look follows the state, so it shows nothing) and launches what it touches.
    // The feed-forward velocity moves it with the state; the pull (c_poseGain per second, capped) only bleeds the drift.
    if (m_body.isValid())
    {
        constexpr float c_poseGain = 4.0f;     // 1/s
        constexpr float c_maxPull = 4.0f;      // m/s
        constexpr float c_snap = 8.0f;         // m: a body this far off is put there (a hitch, a load) - never in plain flight
        const glm::vec3 bodyPos = m_body.getPosition();
        const glm::quat bodyRot = m_body.getRotation();
        const glm::vec3 error = next - bodyPos;
        if (glm::length(error) > c_snap)
            Globals::physics.teleportBody(m_body, next, nextRot);
        glm::vec3 pull = error * c_poseGain;
        if (const float len = glm::length(pull); len > c_maxPull)
            pull *= c_maxPull / len;
        m_linear = velocity + pull;
        glm::quat delta = nextRot * glm::conjugate(bodyRot);
        if (delta.w < 0.0f)
            delta = -delta;
        const float angle = 2.0f * std::acos(glm::clamp(delta.w, -1.0f, 1.0f));
        const float sinHalf = std::sqrt(glm::max(1.0f - delta.w * delta.w, 0.0f));
        const glm::vec3 turnPull = sinHalf > 1e-5f ? glm::vec3(delta.x, delta.y, delta.z) / sinHalf * (glm::min(angle, 0.5f) * c_poseGain)
            : glm::vec3(0.0f);
        m_spin = glm::vec3(0.0f, yawRate, 0.0f) + turnPull;
        m_body.setLinearVelocity(m_linear);
        m_body.setAngularVelocity(m_spin);
    }
    m_pos = next;
    m_rot = nextRot;
    m_entity->pos = m_pos;
    m_entity->rot = m_rot;
}
