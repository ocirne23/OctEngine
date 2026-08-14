export module Entity:NetworkComponent;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;

// Snapshot record flags (the wire's per-record `recFlags` byte, mirrored into `targetFlags`).
// A decoder that meets a bit it doesn't know drops the message — it cannot know the record's size.
export constexpr uint8 NetRecFlag_Physics = 1 << 0; // record carries body WORLD pose + velocities (else entity LOCAL pos/rot)
export constexpr uint8 NetRecFlag_Asleep  = 1 << 1; // the server's body is asleep: hard-sync once and sleep too
export constexpr uint8 NetRecFlag_Forced  = 1 << 2; // owner must accept this correction (claims were rejected); non-owners ignore it
export constexpr uint8 NetRecFlag_Arbitrated = 1 << 3; // player-vs-player contact: the server solver owns the pose — the owner softly corrects toward it while still steering (claims are velocity intent)
export constexpr uint8 NetRecFlag_ServerPlayer = 1 << 4; // the SERVER's own player body: observers use the remote-player interp ring and never apply the interaction grace (locally shoving an actively-steered body only fabricates divergence)
export constexpr uint8 NetRecFlag_Game = 1 << 5; // record carries the 5-byte GAME blob (GameUnitComponent state: health/energy/output/materials/flags) — see NetworkManager's packGameStateBlob

// Which simulation drives this entity, seen from the LOCAL process (derived, never stored):
// the same entity reads LocalOwner on the client that owns it and RemoteOwner on the server/everyone else.
export enum class ENetAuthority : uint8
{
    Local,       // netId == 0: not part of the session (single player / client-local content)
    ServerOwned, // ownerClientId == 0: the server's simulation is the authority
    LocalOwner,  // owned by THIS process (client): my claims drive it — simulate freely, obey only Forced
    RemoteOwner, // owned by another client: their claims drive it (the server validates, everyone else replicates)
};

// Outcome of the server's plausibility gate on the newest claim (debug/editor surface).
export enum class EClaimResult : uint8
{
    None,             // no claim seen yet
    Accepted,
    RejectedSpeed,    // displacement exceeded the per-tick speed budget
    RejectedVelocity, // claimed velocity magnitude over the cap
    RejectedPath,     // trajectory raycast hit world geometry (teleporting through walls)
    RejectedTeleport, // displacement over the hard cap regardless of elapsed time
};

// Snapshot history for a REMOTE-OWNED entity, replayed a couple of ticks behind (see
// NetworkComponent::update). Slots carry their own tick for ring-wrap and loss validation. Owned by
// NetworkManager for stable addresses; written main-thread, read from workers.
export struct NetSnapshotRecord
{
    uint32 tick = 0;
    glm::vec3 pos = glm::vec3(0.0f);
    glm::quat rot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 linVel = glm::vec3(0.0f);
    glm::vec3 angVel = glm::vec3(0.0f);
};
export struct NetSnapshotRing
{
    static constexpr uint32 Capacity = 16;
    NetSnapshotRecord records[Capacity]; // indexed tick % Capacity
    uint32 newestTick = 0;
    uint32 count = 0;
};

// The owner's sampled intent, streamed with each claim. Symmetric: the owning client's controller
// writes it locally, the server stores the latest accepted one on its twin so server-side gameplay
// can read the player's input without extra plumbing.
export constexpr uint32 NetInputButton_Jump = 1 << 0;
export struct NetInputState
{
    uint32 buttons = 0;
    glm::vec3 move = glm::vec3(0.0f);
    glm::vec3 look = glm::vec3(0.0f);
};

// Mutable per-entity sync state, heap-allocated only inside a session so single-player spawns of
// networked prefabs pay nothing. THREADING: NetworkManager writes it on the main thread (receive()
// before the parallel entity pass, send() after); NetworkComponent::update reads/writes only its
// own entity's copy from job workers.
export struct NetEntityState
{
    // ---- shared by both roles ----
    NetInputState input; // owner writes its sampled intent here; the server mirrors the latest accepted one

    // Ownership TRANSFER (server-decided, proximity-based — see NetworkManager::updateOwnershipTransfers):
    // ownership acquired via OwnerChange rather than the entity's Spawn. On the server it marks a
    // release candidate (reverts to server-owned once away from the owner's PRIMARY bodies); on the
    // owning client it marks "not my primary entity" — player control drives only primaries, while
    // the claim stream carries both.
    bool transferredOwnership = false;

    // awake since the last asleep record: send one final record at rest. Both roles use it (server
    // for snapshots, owning client for claims), so it stays outside the union
    bool sleepDirty = true;

    // ---- role-exclusive: a process is ONE role for its whole life, so exactly one member is ever
    // active. Reading the other one is a bug. ----

    struct ServerState
    {
        // validation state (meaningful only for client-owned entities)
        uint32 lastClaimSeq = 0;         // newest claim seq seen (dedups the redundant resends)
        uint32 lastAcceptedClaimSeq = 0;
        // metres of displacement still spendable, refilled at maxClaimSpeed per second of wall
        // clock: bounds total movement over any window regardless of how many packets arrive in it
        float claimBudget = 0.0f;
        double claimBudgetTime = 0.0;
        glm::vec3 lastAcceptedClaimPos = glm::vec3(0.0f); // displacement-budget anchor: what was last ACCEPTED, not the live twin (contacts/corrections perturb it)
        EClaimResult lastClaimResult = EClaimResult::None;
        uint16 violations = 0;           // rejected-claim count (saturating) — cheat telemetry
        uint32 forcedUntilTick = 0;      // while serverTick < this, this entity's snapshot records carry NetRecFlag_Forced

        // newest ACCEPTED claim, re-emitted by snapshot ticks in place of the twin's pose (see
        // sendSnapshotTick) — the twin's pose age wobbles with clock drift and reads as pulsing
        glm::vec3 claimStreamPos = glm::vec3(0.0f);
        glm::quat claimStreamRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 claimStreamLinVel = glm::vec3(0.0f);
        glm::vec3 claimStreamAngVel = glm::vec3(0.0f);
        uint32 claimStreamSeq = 0;     // seq of the stored state; 0 = none since (re)transfer
        uint32 claimStreamSentSeq = 0; // last seq a snapshot emitted (same twice = extrapolate)
        uint8 claimStreamExtrapTicks = 0;

        float releaseTimer = 0.0f; // seconds spent outside the owner's release radius

        // ARBITRATION (see NetworkManager::stealOwnershipOnContact):
        // primaries: while serverTick < arbitratedUntilTick (player-vs-player contact, refreshed per
        // contact) the twin's pose is solver-owned — claims apply as bounded velocity nudges and the
        // owner's records carry NetRecFlag_Arbitrated. Objects: the last two DISTINCT clients to
        // touch, with ages — both fresh = CONTESTED, the object reverts to server ownership until
        // the window decays (single-owner transfer behavior resumes after).
        uint32 arbitratedUntilTick = 0;
        uint32 contestClients[2] = { 0, 0 };
        float contestAges[2] = { 0.0f, 0.0f };
        uint32 contestedUntilTick = 0;

        // This entity is the SERVER's own primary (its player body, marked via
        // NetworkManager::setServerPrimary): it acts as a transfer SOURCE — re-claims transferred
        // objects it approaches, exactly like a client's primary acquires — and is never itself
        // handed to a client by the proximity transfer.
        bool serverPrimary = false;

        // change detection against the last sent state (non-physics; physics uses sleepDirty)
        glm::vec3 lastSentPos = glm::vec3(FLT_MAX); // FLT_MAX forces the first change-detection send
        glm::quat lastSentRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        uint8 lastSentGameBlob[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; // forces the first blob send
    };

    struct ClientState
    {
        // owner-side: next outgoing claim sequence (the redundancy ring lives in the manager —
        // only a handful of entities are ever locally owned)
        uint32 claimSeq = 0;

        // seconds of correction suspension left because one of OUR claim-driven bodies is (or just
        // was) touching this server-owned body — corrections would fight the player's push with the
        // server's RTT-old pre-push state (see NetSyncParams::interactionRadius)
        float localInteractionGrace = 0.0f;

        // latest server state, written by NetworkManager::receive on the main thread BEFORE the
        // (parallel) entity pass — update() only reads it, so no synchronization is needed
        glm::vec3 targetPos = glm::vec3(0.0f);   // body WORLD pos when NetRecFlag_Physics, else entity LOCAL pos
        glm::quat targetRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 targetLinVel = glm::vec3(0.0f);
        glm::vec3 targetAngVel = glm::vec3(0.0f); // radians/second about each world axis
        uint32 serverTick = 0;          // tick of the applied record (per-entity stale-drop for reordered packets)
        uint32 lastAppliedTick = 0;     // one-shot latch: velocities/sleep apply once per new snapshot
        float timeSinceSnapshot = 0.0f;
        // remote-owned entities: interpolation playback state (see NetSnapshotRing above)
        NetSnapshotRing* remoteBuffer = nullptr; // manager-owned, set by handleSnapshot (main thread)
        float playbackTick = 0.0f;               // fractional tick the observer is currently displaying
        uint8 targetFlags = 0;          // NetRecFlag bits of the applied record
        bool hasTarget = false;
    };

    union
    {
        ServerState server;
        ClientState client;
    };

    explicit NetEntityState(bool isServer)
    {
        if (isServer)
            new (&server) ServerState();
        else
            new (&client) ClientState();
    }
};
// the union has no destructor dance only because both members need none — keep it that way
static_assert(std::is_trivially_destructible_v<NetEntityState::ServerState>
    && std::is_trivially_destructible_v<NetEntityState::ClientState>);

// Marks the entity as networked state ("Component Network" in .pre is pure PRESENCE — netIds are
// minted only by the server). A client's component either carries the server's id, or stays
// LOCAL-INERT with netId 0 (client-local content, never synced). update() runs client-side only and
// corrects toward the latest target by the "Network/Correction" thresholds. DYNAMIC bodies sync the
// BODY through the thread-safe PhysicsWorld queue — PhysicsComponent::update rewrites entity.pos/rot
// from the body right after, so correcting the entity directly would be overwritten. Kinematic and
// static bodies use the entity-transform path.
export struct NetworkComponent
{
    static constexpr EComponentID getId() { return EComponentID_Network; }

    uint32 netId = 0;         // 0 = local-inert (client-local content / single player); anything else is server-minted
    uint32 ownerClientId = 0; // 0 = server-owned; else the clientId whose claims drive this entity (set via NetworkManager::setOwner, carried in the Spawn message — never authored)

    // null outside a session (role None, or a local-inert registration). NetworkManager only
    // touches registered components so it never null-checks; update() and outside readers must.
    std::unique_ptr<NetEntityState> state;

    // Local, ServerOwned, LocalOwner or RemoteOwner — from THIS process's perspective (see the enum)
    ENetAuthority authority() const;

    struct SpawnInfo {}; // presence only — ids are assigned in code (see registerEntity), never authored

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, float deltaSeconds); // client correction toward the latest target
};

export const NetworkComponent::SpawnInfo* getNetworkSpawnInfo(const Entity* entity);

// Serializes a network spawn recipe into a "Component Network" node.
export void writeNetworkSpawnInfo(const NetworkComponent::SpawnInfo& info, AssetNode& out);
