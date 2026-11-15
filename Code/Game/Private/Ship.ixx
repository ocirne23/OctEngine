export module Game:Ship;

import Core;
import Core.glm;
import Entity;
import Physics;
import Settings;

// THE SHIP: the players' flying magic ship (the sandbox world, authority only). A render-only entity (airship.pre) plus
// a KINEMATIC body GameShip makes itself (the deck, the hull, and while it flies the structures on the deck), so players
// and units stand on it. GameShip integrates its own state - a weighty flight model ("Game/Ship" tweaks) - and every
// tick drives the body with it and writes the entity's pose. It LANDS on the world's grid, and then the structures on
// its deck are part of the base. Main thread, pre-physics (GameMatch::updatePlayer). See Docs/Context/Game/Ship.md.
export class GameShip final
{
public:
    // The ground under a point (the terrain collider; the fallback where there is none), for the hover.
    using GroundFn = oc::function<float(float x, float z, float fallback)>;

    struct Input
    {
        float throttle = 0.0f; // -1 (S) .. 1 (W): moves the throttle lever; 0 holds it
        float turn = 0.0f;     // -1 (D, right) .. 1 (A, left)
        float climb = 0.0f;    // -1 (Ctrl) .. 1 (Space)
        bool piloted = false;  // nobody at the helm: the lever eases back to stop
    };

    // BLOCKY: 28 x 11 grid tiles of 2 m (airship.obj; rowHalfTiles is the same outline). A CENTRE COLUMN of tiles: the
    // origin stays on a tile corner (the landing grid), so the middle line is x = DeckCentreX and the beam x -10 .. 12.
    static constexpr float DeckHalfWidth = 11.0f;  // X (the widest beam), around DeckCentreX
    static constexpr float DeckCentreX = 1.0f;
    static constexpr int MaxHalfTiles = 5;         // the widest row: the tiles -5 .. 5
    static constexpr float DeckHalfLength = 28.0f; // Z (the bow -28, the stern +28)
    static constexpr float KeelDepth = 7.0f;       // the keel's bottom under the deck top
    // Where the PILOT stands (the deck top): the tile right behind the Beacon (sternward). No wheel is drawn.
    static glm::vec3 helmLocal() { return glm::vec3(1.0f, 0.0f, 3.0f); }

    // ---- THE DECK'S TILES (the ship-local 2 m grid: tile (cx, cz) spans x 2cx .. 2cx + 2, z 2cz .. 2cz + 2) ----
    static constexpr int DeckRows = 28;    // 2 m rows from the bow (z -DeckHalfLength) to the stern
    static int rowHalfTiles(int row);      // the outline: a row of half-width h holds the tiles -h .. h (0 = off the deck)
    static bool deckCell(int cx, int cz);  // a tile of the deck
    // THE RAIL (the ship's wall, a ShipRail structure per edge tile): bit 0 +X, 1 -X, 2 +Z, 3 -Z = the tile's
    // neighbour on that side is off the deck. 0 = no rail: an inner tile, or an ENTRANCE (entranceCell).
    static uint8 railSides(int cx, int cz);
    // THE ENTRANCES: the gaps in the rail, the way on and off a landed ship (each gets stairs down to the ground). A group
    // of `count` edge tiles from `first` along the edge, opening toward `out` (a unit step in tiles: +-X or +-Z).
    struct Entrance { glm::ivec2 first; int count; glm::ivec2 out; };
    static oc::span<const Entrance> entrances();
    static bool entranceCell(int cx, int cz);
    static bool helmCell(int cx, int cz);     // the pilot's tile (helmLocal): nothing builds there
    static glm::ivec2 baseCell() { return glm::ivec2(0, -1); } // the Beacon's centre tile (3 x 3, on the middle line)

    // A box of the ship's body in ship space (a structure on the deck while it flies: StructureSystem::shipColliderBoxes).
    struct Box { glm::vec3 center{ 0.0f }, half{ 0.5f }; };

    void spawn(const glm::vec3& deckPos, float yaw);
    void despawn();
    bool exists() const { return (bool)m_entity; }
    void teleport(const glm::vec3& deckPos, float yaw); // a load: the pose, the state at rest
    // The structures' boxes the body carries while it flies (none while it is landed: they have their own colliders
    // then). A change rebuilds the body.
    void setStructureBoxes(oc::span<const Box> boxes);

    // ---- LANDING (Docs/Context/Game/Ship.md "Landing") ----
    // A LANDED ship sits on the world's 2 m grid: its yaw a quarter turn, its centre on a tile corner, its deck top at
    // the ground. Every deck tile is then a world grid cell, and the structures on it join the world (StructureSystem).
    // land() starts the SETTLE toward that pose (the flight is off from then on); landed() once it is there. takeOff()
    // lets go: the flight takes over at rest and the floor push lifts the hull out of the ground.
    static int quarterTurnsOf(float yaw);                 // the nearest quarter turn (0..3) of a yaw
    static float yawOfQuarterTurns(int turns) { return (float)(turns & 3) * glm::half_pi<float>(); }
    static glm::vec3 snapCentre(const glm::vec3& pos);   // x, z to the nearest tile corner (a multiple of 2 m)
    void land(const glm::vec3& centre, int quarterTurns);
    void landAt(const glm::vec3& centre, int quarterTurns); // at once (the start, a load): no settle
    void takeOff();
    bool landed() const { return m_land == ELand::Landed; }
    bool grounded() const { return m_land != ELand::Flying; } // landed, or settling down to it
    int quarterTurns() const { return m_landTurns; }
    glm::vec3 landedPos() const { return m_landPos; } // the landed pose's deck top centre (grounded)

    void tick(float deltaSec, const Input& input, const GroundFn& ground);

    glm::vec3 position() const { return m_pos; }   // the deck top's centre
    float yaw() const { return m_yaw; }
    glm::quat rotation() const { return m_rot; }   // the full pose (yaw, pitch, bank)
    bool sailing() const { return m_sailing; }
    float speed() const { return m_forwardSpeed; } // m/s along the bow
    float throttle() const { return m_throttle; }
    float height() const { return m_height; }       // the wanted height over the ground (m)
    void setWantedHeight(float keelOverGround);     // the start: clamped to "Min height (m)" .. "Max height (m)"
    glm::vec3 toWorld(const glm::vec3& local) const { return m_pos + m_rot * local; }
    glm::vec3 toLocal(const glm::vec3& world) const { return glm::inverse(m_rot) * (world - m_pos); }
    // The deck's velocity at a world point (the spin included): what a body standing there moves with.
    glm::vec3 pointVelocity(const glm::vec3& world) const;
    // Over the deck, and up to `above` m over its top (a standing capsule's centre is ~2 m up).
    bool onDeck(const glm::vec3& world, float above = 3.5f) const;

private:
    void makeBody();
    void tickGrounded(float dt, const GroundFn& ground);
    float m_travelY = 0.0f; // the settle's cruise height on the way to a far site (land(); it rises over the ground ahead)
    // The body to the state's pose: its own velocity (and the yaw rate in rad/s as its spin) plus a gentle pull.
    void driveBody(const glm::vec3& next, const glm::quat& nextRot, const glm::vec3& velocity, float yawRate);
    enum class ELand : uint8 { Flying, Settling, Landed };
    ELand m_land = ELand::Flying;
    glm::vec3 m_landPos{ 0.0f }; // the landed pose: the deck top's centre
    int m_landTurns = 0;
    oc::vector<Box> m_structureBoxes;

    EntityPtr m_entity;
    PhysicsBody m_body;
    glm::vec3 m_pos{ 0.0f };
    glm::quat m_rot{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec2 m_vel{ 0.0f };        // horizontal (x, z)
    float m_vy = 0.0f;
    float m_yaw = 0.0f;             // radians; the bow is -Z rotated by it about +Y
    float m_yawRate = 0.0f;         // deg/s
    float m_throttle = 0.0f;        // -0.35 .. 1
    float m_forwardSpeed = 0.0f;
    float m_height = 8.0f;          // the wanted height of the keel over the ground
    float m_pitch = 0.0f, m_bank = 0.0f; // deg, smoothed
    float m_time = 0.0f;
    bool m_sailing = false;
    float m_seaLeave = 0.0f;        // s off deep water while sailing (it stops sailing after a second)
    glm::vec3 m_spin{ 0.0f };       // the body's angular velocity this tick (rad/s), for pointVelocity
    glm::vec3 m_linear{ 0.0f };     // and its linear velocity
};
