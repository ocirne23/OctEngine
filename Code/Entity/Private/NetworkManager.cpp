module Entity;

import Core;
import Core.Log;
import Core.glm;
import Core.Tweaks;
import Network;
import Physics;

// Bump on ANY wire format change: the transport handshake denies mismatched protocol ids, so old
// builds fail to connect instead of misparsing. GameNetVersion rides in Hello/Welcome purely so the
// mismatch produces a readable log line when the protocolId was forgotten.
constexpr uint32 GameProtocolId = 0x4F435346; // snapshot/claim records may carry the GAME blob (NetRecFlag_Game)
constexpr uint16 GameNetVersion = 14;

// Engine-reserved event: Synced-flagged tweak values, server -> clients (full set at join +
// re-broadcast on change). Intercepted in fireEventAttributed — never reaches scripts or the
// game hook, and a client-sent copy is dropped (only clients APPLY it).
constexpr oc::string_view TweakSyncEventName = "OcTweakSync";

enum class ENetMsg : uint8
{
    Hello = 1, // client -> server, ch2
    Welcome,   // server -> client, ch2
    Deny,      // server -> client, ch2
    Snapshot,  // server -> client, ch0
    Event,     // both directions, ch1
    Spawn,       // server -> client, ch2 (ordered after Welcome)
    Despawn,     // server -> client, ch2
    Claim,       // owner client -> server, ch3
    OwnerChange, // server -> client, ch2: [varuint netId][varuint ownerClientId] (per ENTITY, unlike Spawn's per-tree owner)
};

// Bounds on a quantization range read off the wire. The sender declares its own live tweak values,
// so a hostile or misconfigured peer can otherwise scale every decoded velocity in the message.
constexpr float MinWireQuantizeRange = 1.0f;
constexpr float MaxWireQuantizeRange = 1000.0f;

constexpr size_t MaxEventNameLength = 64;  // event names; real ones are identifiers
constexpr size_t MaxEventDataBytes = 1024; // payload cap: bounded so one event stays in one packet
// type byte + senderClientId + senderNetId + name length + name + payload length (varints are <= 5B
// at these magnitudes). Both caps are enforced before send, so a valid event always fits.
constexpr size_t MaxEventMessageBytes = 1 + 5 + 5 + 5 + MaxEventNameLength + 5 + MaxEventDataBytes;
// Events are reliable, so exceeding this would only fragment (correct, just more packets) — the
// assert exists to make that a DECISION rather than something a grown header does behind our backs.
static_assert(MaxEventMessageBytes <= netMaxSinglePacketMessage(NetHostConfig{}.maxPacketSize, true),
    "an event no longer fits one packet: raise maxPacketSize, lower the caps, or accept fragmenting");
constexpr size_t MaxClients = 32;         // connection-slot cap: each accepted client costs a world
                                          // replay + whatever the app spawns for it in onClientJoined

constexpr uint8 SnapshotFlag_Quantized = 1 << 0;
constexpr uint8 ChannelSnapshot = 0;
constexpr uint8 ChannelEvent = 1;
constexpr uint8 ChannelSession = 2;
constexpr uint8 ChannelClaim = 3;

// ---- tweaks (server send policy; correction thresholds live in NetSyncParams) ------------------
static float s_snapshotHz = 20.0f;       // matches the physics fixed step
static bool  s_quantize = true;
// Snapshots are UNRELIABLE, so an oversized one is DROPPED, not fragmented — this must stay under
// the transport's single-packet budget, which is why the cap is derived rather than written down.
static int   s_snapshotMaxBytes = int(netMaxSinglePacketMessage(NetHostConfig{}.maxPacketSize, true)) - 64;
static int   s_maxEntitiesPerTick = 200;
static int   s_keyframeEveryTicks = 20;  // unmoved entities refresh on this rotation (drift/late-join repair)
static float s_sendPosEpsilon = 0.001f;
static float s_sendRotEpsilonDeg = 0.1f;
static float s_maxVel = 50.0f;    // velocity quantization range (m/s); sent per message so both ends agree
static float s_maxAngVel = 50.0f; // angular velocity quantization range (rad/s)
static bool  s_showStats = true;
// ECDH handshake + AES-128-GCM on every payload packet. Not a tweak: the transport reads it at
// open() and it cannot change on a live host, and both ends must agree or the handshake denies.
// Without it a peer is identified only by source address, so anyone able to forge one (trivial on a
// shared LAN) can inject packets as another player. Costs +16 B/packet and well under 1% of a core.
static bool  s_encrypt = true;

// Thinning limit, not a clock: claims fire on physics step boundaries (see send()), so this only
// caps them lower and above the step rate never binds. The two ends need not agree — the
// displacement budget is wall-clock, so an inflated rate buys nothing.
static float s_maxUpdateHz = 20.0f;
static float s_maxClaimSpeed = 60.0f;     // m/s the movement token bucket refills at (cap = half a second
                                          // of it); must exceed the fastest LEGITIMATE motion, free fall included
// How hard the server's twin chases an accepted claim (1/sec — 1/step closes the gap in one step).
// It follows through the solver rather than being teleported onto the claim; see handleClaimMessage.
static float s_twinFollowGain = 10.0f;
static float s_twinResyncDistance = 2.0f; // past this the follow can't close it: hard teleport
static float s_maxClaimVelocity = 50.0f;  // cap on the claimed linear velocity magnitude
static float s_maxClaimAngVel = 50.0f;    // ...and angular (rad/s); both applied on every accept path
static float s_claimTeleportCap = 10.0f;  // hard displacement cap regardless of elapsed time
static bool  s_claimPathRaycast = true;   // reject claims whose path crosses world geometry
static int   s_forcedTicks = 30;          // snapshot ticks the owner stays force-corrected after a rejection
static float s_claimReanchorRadius = 2.0f; // a claim this close to the twin's CURRENT state always accepts (guaranteed rejection recovery)
static int   s_claimRedundancy = 4;       // past claims carried in EVERY packet (<= ring capacity 8): a claim survives unless this many consecutive packets drop
// Claim passthrough: re-emit an accepted claim extrapolated this far forward along its velocity,
// cancelling the hold until the next snapshot tick. Must stay CONSTANT — a varying, arrival-phase
// dependent shift is what makes remote motion pulse. Higher = prediction, and overshoot on turns.
static float s_ownerPredictTicks = 0.5f;

// proximity ownership transfer (server): a server-owned dynamic body near a client's PRIMARY owned
// body transfers to that client (its physics then drives the object, claims-validated); it reverts
// once outside the release radius for the delay. Release > transfer = hysteresis, no flapping.
static bool  s_transferEnabled = true;
static float s_transferRadius = 2.5f;
static float s_releaseRadius = 4.0f;
static float s_releaseDelaySec = 1.0f;
static float s_arbitrateSec = 1.0f;  // player-vs-player contact: how long both primaries stay server-arbitrated (refreshed per contact)
static float s_contestSec = 1.5f;    // object touched by two DISTINCT clients within this window = contested -> server-owned until it decays

// Every wire float passes this before reaching physics or a transform: NaN defeats the plausibility
// gate silently, since every comparison against it is false ("not greater than the cap").
static bool isFinite(float v) { return std::isfinite(v); }
static bool isFinite(const glm::vec3& v) { return isFinite(v.x) && isFinite(v.y) && isFinite(v.z); }
static bool isFinite(const glm::quat& q) { return isFinite(q.x) && isFinite(q.y) && isFinite(q.z) && isFinite(q.w); }

// wire quaternions must also be UNIT — box3d builds a rotation matrix from it
static bool sanitizeRotation(glm::quat& q)
{
    if (!isFinite(q))
        return false;
    const float len = glm::length(q);
    if (len < 1e-4f)
        return false;
    q /= len;
    return true;
}

static float quatAngleDeg(const glm::quat& a, const glm::quat& b)
{
    const float d = glm::min(1.0f, glm::abs(glm::dot(a, b)));
    return glm::degrees(2.0f * glm::acos(d));
}

// Smallest-three quaternion packing: 2-bit largest-component index + 3x10-bit snorm over
// [-1/sqrt2, 1/sqrt2] (the largest component is >= the others, so the rest fit that range).
static uint32 packQuat(const glm::quat& q)
{
    const float comps[4] = { q.x, q.y, q.z, q.w };
    uint32 largest = 0;
    for (uint32 i = 1; i < 4; ++i)
        if (glm::abs(comps[i]) > glm::abs(comps[largest]))
            largest = i;
    const float sign = comps[largest] < 0.0f ? -1.0f : 1.0f; // q and -q are the same rotation
    constexpr float invSqrt2 = 0.70710678f;
    uint32 packed = largest << 30;
    uint32 shift = 0;
    for (uint32 i = 0; i < 4; ++i)
    {
        if (i == largest)
            continue;
        const float v = glm::clamp(comps[i] * sign / invSqrt2, -1.0f, 1.0f);
        packed |= uint32((v * 0.5f + 0.5f) * 1023.0f + 0.5f) << (shift * 10);
        ++shift;
    }
    return packed;
}

static glm::quat unpackQuat(uint32 packed)
{
    const uint32 largest = packed >> 30;
    constexpr float invSqrt2 = 0.70710678f;
    float comps[4];
    float sumSq = 0.0f;
    uint32 shift = 0;
    for (uint32 i = 0; i < 4; ++i)
    {
        if (i == largest)
            continue;
        const float v = (float((packed >> (shift * 10)) & 1023u) / 1023.0f * 2.0f - 1.0f) * invSqrt2;
        comps[i] = v;
        sumSq += v * v;
        ++shift;
    }
    comps[largest] = glm::sqrt(glm::max(0.0f, 1.0f - sumSq));
    return glm::normalize(glm::quat(comps[3], comps[0], comps[1], comps[2]));
}

static const char* disconnectReasonName(ENetDisconnectReason reason)
{
    switch (reason)
    {
    case ENetDisconnectReason::Local:         return "local";
    case ENetDisconnectReason::Remote:        return "remote";
    case ENetDisconnectReason::Timeout:       return "timeout";
    case ENetDisconnectReason::ConnectFailed: return "connect failed";
    case ENetDisconnectReason::Denied:        return "denied";
    case ENetDisconnectReason::Overflow:      return "reliable backlog overflow";
    default:                                  return "none";
    }
}

void NetworkManager::initialize()
{
    static bool s_registered = false;
    if (s_registered)
        return;
    s_registered = true;

    Tweak::floatVar("Network", "Snapshot Hz", &s_snapshotHz, 1.0f, 60.0f, 0.5f);
    Tweak::boolean("Network", "Quantize", &s_quantize);
    Tweak::intVar("Network", "Snapshot max bytes", &s_snapshotMaxBytes, 128, 1400);
    Tweak::intVar("Network", "Max entities per tick", &s_maxEntitiesPerTick, 1, 4096);
    Tweak::intVar("Network", "Keyframe every ticks", &s_keyframeEveryTicks, 1, 255);
    Tweak::floatVar("Network", "Send pos epsilon", &s_sendPosEpsilon, 0.0f, 0.1f, 0.0005f);
    Tweak::floatVar("Network", "Send rot epsilon (deg)", &s_sendRotEpsilonDeg, 0.0f, 10.0f, 0.01f);
    Tweak::floatVar("Network", "Max vel (quantize m/s)", &s_maxVel, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Network", "Max ang vel (quantize rad/s)", &s_maxAngVel, 1.0f, 200.0f, 1.0f);
    Tweak::boolean("Network", "Show stats", &s_showStats);

    Tweak::floatVar("Network/Correction", "Pos deadzone", &m_params.posDeadzone, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Network/Correction", "Pos snap threshold", &m_params.posSnapThreshold, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Network/Correction", "Rot deadzone (deg)", &m_params.rotDeadzoneDeg, 0.0f, 30.0f, 0.1f);
    Tweak::floatVar("Network/Correction", "Rot snap threshold (deg)", &m_params.rotSnapThresholdDeg, 0.0f, 180.0f, 0.5f);
    Tweak::floatVar("Network/Correction", "Blend rate", &m_params.blendRate, 0.0f, 30.0f, 0.1f);
    Tweak::boolean("Network/Correction", "Extrapolate", &m_params.extrapolate);
    Tweak::intVar("Network/Correction", "Remote interp (ticks)", &m_params.remoteInterpTicks, 2, 8);
    Tweak::floatVar("Network/Correction", "Interaction radius", &m_params.interactionRadius, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Network/Correction", "Interaction linger", &m_params.interactionLinger, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Network/Correction", "Push pos gain", &m_params.pushPosGain, 0.0f, 30.0f, 0.1f);
    Tweak::floatVar("Network/Correction", "Push rot gain", &m_params.pushRotGain, 0.0f, 30.0f, 0.1f);
    Tweak::floatVar("Network/Correction", "Push max vel", &m_params.pushMaxVel, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Network/Correction", "Push max ang vel", &m_params.pushMaxAngVel, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Network/Correction", "Push accel limit", &m_params.pushMaxAccel, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Network/Correction", "Push ang accel limit", &m_params.pushMaxAngAccel, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Network/Correction", "Push catch-up boost", &m_params.pushCatchUpBoost, 1.0f, 20.0f, 0.1f);
    Tweak::floatVar("Network/Correction", "Arbitrate deadzone", &m_params.arbitrateDeadzone, 0.0f, 3.0f, 0.02f);
    Tweak::floatVar("Network/Correction", "Pos teleport threshold", &m_params.posTeleportThreshold, 0.0f, 100.0f, 0.5f);

    Tweak::boolean("Network/Ownership", "Transfer enabled", &s_transferEnabled);
    Tweak::floatVar("Network/Ownership", "Transfer radius", &s_transferRadius, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Network/Ownership", "Release radius", &s_releaseRadius, 0.0f, 40.0f, 0.1f);
    Tweak::floatVar("Network/Ownership", "Release delay", &s_releaseDelaySec, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Network/Ownership", "Arbitrate window", &s_arbitrateSec, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Network/Ownership", "Contest window", &s_contestSec, 0.0f, 5.0f, 0.05f);

    Tweak::floatVar("Network/Player", "Max update Hz", &s_maxUpdateHz, 1.0f, 120.0f, 0.5f);
    Tweak::floatVar("Network/Validation", "Max speed", &s_maxClaimSpeed, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Network", "Twin follow gain", &s_twinFollowGain, 0.0f, 40.0f, 0.5f);
    Tweak::floatVar("Network", "Twin resync (m)", &s_twinResyncDistance, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Network/Validation", "Max velocity", &s_maxClaimVelocity, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Network/Validation", "Max ang velocity", &s_maxClaimAngVel, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Network/Validation", "Teleport cap", &s_claimTeleportCap, 0.0f, 100.0f, 0.5f);
    Tweak::boolean("Network/Validation", "Path raycast", &s_claimPathRaycast);
    Tweak::intVar("Network/Validation", "Forced ticks", &s_forcedTicks, 1, 255);
    Tweak::floatVar("Network/Validation", "Re-anchor radius", &s_claimReanchorRadius, 0.1f, 10.0f, 0.1f);
    // NOT validation: redundancy is the owning CLIENT's send-side loss margin, and claim lead is a
    // SERVER emit-side timeline shift. Same miscategorisation "Claim rate Hz" had before it moved.
    Tweak::intVar("Network/Player", "Claim redundancy", &s_claimRedundancy, 1, 8);
    Tweak::floatVar("Network", "Owner predict (ticks)", &s_ownerPredictTicks, 0.0f, 2.0f, 0.05f);

    // live-editable transport link simulation (outgoing packets)
    NetHostConfig& config = m_host.config();
    Tweak::floatVar("Network/Link sim", "Packet loss", &config.simPacketLoss, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Network/Link sim", "Latency (ms)", &config.simLatencyMs, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Network/Link sim", "Jitter (ms)", &config.simJitterMs, 0.0f, 500.0f, 1.0f);
}

void NetworkManager::setEncryption(bool enabled)
{
    s_encrypt = enabled;
}

bool NetworkManager::startServer(uint16 port)
{
    assert(m_role == ENetRole::None);
    NetHostConfig config;
    config.protocolId = GameProtocolId;
    config.acceptIncoming = true;
    config.encrypt = s_encrypt;
    if (!m_host.open(port, config))
    {
        Log::error("Network: failed to open server port " + oc::to_string(port));
        return false;
    }
    m_role = ENetRole::Server;
    Log::info("Network: SERVER listening on port " + oc::to_string(m_host.getLocalPort())
        + (s_encrypt ? " (encrypted)" : " (UNENCRYPTED)"));
    return true;
}

bool NetworkManager::startClient(const oc::string& address, uint16 defaultPort)
{
    assert(m_role == ENetRole::None);
    NetAddress addr = NetAddress::fromString(address);
    if (!addr.isValid())
    {
        // not a dotted quad: treat as hostname[:port]
        const size_t colon = address.rfind(':');
        oc::string host = address;
        uint16 port = defaultPort;
        if (colon != oc::string::npos)
        {
            host = address.substr(0, colon);
            port = uint16(std::atoi(address.c_str() + colon + 1));
        }
        addr = netResolveHost(host, port);
    }
    if (addr.port == 0)
        addr.port = defaultPort;
    if (!addr.isValid())
    {
        Log::error("Network: cannot resolve server address '" + address + "'");
        return false;
    }
    NetHostConfig config;
    config.protocolId = GameProtocolId;
    config.acceptIncoming = false;
    config.encrypt = s_encrypt;
    if (!m_host.open(0, config))
    {
        Log::error("Network: failed to open client socket");
        return false;
    }
    m_serverAddress = addr; // kept for auto-reconnect
    m_serverPeer = m_host.connect(addr);
    m_role = ENetRole::Client;
    Log::info("Network: CLIENT connecting to " + addr.toString()
        + (s_encrypt ? " (encrypted)" : " (UNENCRYPTED)"));
    return true;
}

void NetworkManager::shutdown()
{
    m_host.close();
    m_role = ENetRole::None;
    m_serverPeer = InvalidNetPeerId;
    m_serverAddress = {};
    m_readyPeers.clear();
    m_dynamicSpawns.clear();
    m_dynamicRootIds.clear();
    m_pendingSpawnAnnounce.clear();
    m_pendingDespawn.clear();
    m_peerClients.clear();
    m_claimRings.clear();
    m_remoteBuffers.clear();
    m_preWelcomeEvents.clear();
    m_localClientId = 0;
    m_nextClientId = 1;
}

void NetworkManager::receive(double deltaSec)
{
    if (m_role == ENetRole::None)
        return;
    ProfileScope scope("NetworkManager::receive", EProfileCategory::Network);
    m_netTime += deltaSec;

    // publish this frame's locally-owned body positions for the interaction grace (see the accessor);
    // rebuilt before the entity pass so the parallel component updates read a stable snapshot
    m_localOwnedBodyPositions.clear();
    if (m_role == ENetRole::Client && m_localClientId != 0)
    {
        const std::lock_guard<std::mutex> lock(m_entityMutex);
        for (const auto& [netId, replicated] : m_entities)
        {
            if (replicated.comp->ownerClientId != m_localClientId)
                continue;
            const PhysicsComponent* physics = getComponent<PhysicsComponent>(replicated.entity);
            if (physics && physics->bodyType == EPhysicsBodyType::Dynamic && physics->body.isValid())
                m_localOwnedBodyPositions.push_back(physics->body.getPosition());
        }
    }

    // cap the dt fed to the transport: the first frame after a long init (multi-second asset/model
    // loads) or a debugger pause arrives with SECONDS of deltaSec, and Time::update has no clamp —
    // uncapped it advances the transport clock past the 5s handshake/receive timeouts in ONE step,
    // failing the connection before the buffered replies are even drained
    m_host.update(glm::min(deltaSec, 0.5));
    for (NetEvent& evt : m_host.takeEvents())
    {
        switch (evt.type)
        {
        case ENetEventType::Connected:
            if (m_role == ENetRole::Client)
            {
                Log::info("Network: connected to server " + m_host.getPeerAddress(evt.peer).toString());
                sendHello();
            }
            else
                Log::info("Network: client connected from " + m_host.getPeerAddress(evt.peer).toString());
            break;

        case ENetEventType::Disconnected:
            // NetPeerIds recycle: drop every bit of per-peer state the moment the peer dies
            oc::erase(m_readyPeers, evt.peer);
            if (m_role == ENetRole::Client && evt.peer == m_serverPeer)
            {
                Log::warning(oc::string("Network: disconnected from server (") + disconnectReasonName(evt.reason) + ")");
                m_serverPeer = InvalidNetPeerId;
                m_localClientId = 0;
                m_preWelcomeEvents.clear(); // stale parked events must not replay into a new session
                // auto-reconnect: heals startup-time handshake failures and server restarts alike;
                // a failed attempt takes connectTimeoutSec to report, which self-paces the retries.
                // The re-handshake re-runs Hello->Welcome and the world replay is idempotent.
                if (m_serverAddress.isValid())
                {
                    Log::info("Network: reconnecting to " + m_serverAddress.toString());
                    m_serverPeer = m_host.connect(m_serverAddress);
                }
            }
            else if (m_role == ENetRole::Server)
            {
                Log::info(oc::string("Network: client disconnected (") + disconnectReasonName(evt.reason) + ")");
                if (const auto it = m_peerClients.find(evt.peer); it != m_peerClients.end())
                {
                    const uint32 clientId = it->second;
                    m_peerClients.erase(it);
                    {
                        // everything the client held through proximity transfer reverts to the server
                        // (its primaries are torn down by the app's onClientLeft right after)
                        const std::lock_guard<std::mutex> lock(m_entityMutex);
                        for (const auto& [netId, replicated] : m_entities)
                            if (replicated.comp->ownerClientId == clientId && replicated.comp->state->transferredOwnership)
                                transferOwnership(netId, replicated.comp, 0);
                    }
                    if (m_onClientLeft)
                        m_onClientLeft(clientId); // main thread: teardown of per-player entities is safe here
                }
            }
            break;

        case ENetEventType::Message:
        {
            if (evt.data.empty())
                break;
            const uint8 msgType = evt.data[0];
            NetReader reader(oc::span<const uint8>(evt.data).subspan(1));
            switch (ENetMsg(msgType))
            {
            case ENetMsg::Hello:
            case ENetMsg::Welcome:
            case ENetMsg::Deny:
                handleSessionMessage(evt.peer, reader, msgType);
                break;
            case ENetMsg::Snapshot:
                if (m_role == ENetRole::Client)
                    handleSnapshot(reader);
                break;
            case ENetMsg::Event:
                handleEventMessage(evt.peer, evt.data);
                break;
            case ENetMsg::Spawn:
                if (m_role == ENetRole::Client)
                    handleSpawnMessage(reader);
                break;
            case ENetMsg::Despawn:
                if (m_role == ENetRole::Client)
                    handleDespawnMessage(reader);
                break;
            case ENetMsg::Claim:
                if (m_role == ENetRole::Server)
                    handleClaimMessage(evt.peer, reader);
                break;
            case ENetMsg::OwnerChange:
                if (m_role == ENetRole::Client)
                    handleOwnerChangeMessage(reader);
                break;
            default:
                Log::warning("Network: unknown message type " + oc::to_string(msgType));
                break;
            }
            break;
        }
        }
    }
}

void NetworkManager::sendHello()
{
    uint8 buffer[8];
    NetWriter writer(buffer);
    writer.write<uint8>(uint8(ENetMsg::Hello));
    writer.write<uint16>(GameNetVersion);
    assert(!writer.overflowed());
    m_host.send(m_serverPeer, writer.data(), ENetDelivery::Reliable, ChannelSession);
}

void NetworkManager::handleSessionMessage(NetPeerId peer, NetReader& reader, uint8 msgType)
{
    switch (ENetMsg(msgType))
    {
    case ENetMsg::Hello:
    {
        if (m_role != ENetRole::Server)
            break;
        const uint16 version = reader.read<uint16>();
        if (reader.overflowed())
            break;
        if (version != GameNetVersion)
        {
            Log::warning("Network: denying client with version " + oc::to_string(version) + " (ours " + oc::to_string(GameNetVersion) + ")");
            uint8 buffer[128];
            NetWriter writer(buffer);
            writer.write<uint8>(uint8(ENetMsg::Deny));
            writer.writeString("version mismatch");
            m_host.send(peer, writer.data(), ENetDelivery::Reliable, ChannelSession);
            m_host.disconnect(peer);
            break;
        }
        // slot cap: accepting a client costs a full world replay plus whatever the app spawns for it
        // (a player body), and nothing else bounds how many connections one attacker opens
        if (!m_peerClients.contains(peer) && m_peerClients.size() >= MaxClients)
        {
            Log::warning("Network: refusing client, server full (" + oc::to_string(MaxClients) + ")");
            uint8 fullBuffer[64];
            NetWriter fullWriter(fullBuffer);
            fullWriter.write<uint8>(uint8(ENetMsg::Deny));
            fullWriter.writeString("server full");
            m_host.send(peer, fullWriter.data(), ENetDelivery::Reliable, ChannelSession);
            m_host.disconnect(peer);
            break;
        }
        if (oc::find(m_readyPeers.begin(), m_readyPeers.end(), peer) == m_readyPeers.end())
            m_readyPeers.push_back(peer);
        // duplicate Hello from an already-ready peer (the reliable channel dedups packets, so only a
        // misbehaving client sends two): resend the Welcome with the EXISTING id and do nothing else —
        // minting a fresh id would leak the old one and fire onClientJoined again (a second player)
        if (const auto existing = m_peerClients.find(peer); existing != m_peerClients.end())
        {
            uint8 dupBuffer[32];
            NetWriter dupWriter(dupBuffer);
            dupWriter.write<uint8>(uint8(ENetMsg::Welcome));
            dupWriter.write<uint16>(GameNetVersion);
            dupWriter.writeVarUInt(m_serverTick);
            dupWriter.write<float>(s_snapshotHz);
            dupWriter.writeVarUInt(existing->second);
            m_host.send(peer, dupWriter.data(), ENetDelivery::Reliable, ChannelSession);
            break;
        }
        const uint32 clientId = m_nextClientId++;
        m_peerClients[peer] = clientId; // stable per connection — peer ids recycle, clientIds don't
        uint8 buffer[32];
        NetWriter writer(buffer);
        writer.write<uint8>(uint8(ENetMsg::Welcome));
        writer.write<uint16>(GameNetVersion);
        writer.writeVarUInt(m_serverTick);
        writer.write<float>(s_snapshotHz);
        writer.writeVarUInt(clientId);
        assert(!writer.overflowed());
        m_host.send(peer, writer.data(), ENetDelivery::Reliable, ChannelSession);
        // the networked world arrives HERE: every live spawn record replayed, ordered after the
        // Welcome on the same channel (overlap with this frame's pending announces is harmless —
        // the client spawn is idempotent)
        for (const auto& [baseId, record] : m_dynamicSpawns)
            if (!record.path.empty())
                sendSpawnTo(peer, record);
        // per-entity ownership that diverged from the spawn records (proximity transfers) rides the
        // same ordered channel right after
        {
            const std::lock_guard<std::mutex> lock(m_entityMutex);
            for (const auto& [netId, replicated] : m_entities)
                if (replicated.comp->state->transferredOwnership)
                {
                    uint8 ownerBuffer[16];
                    NetWriter ownerWriter(ownerBuffer);
                    ownerWriter.write<uint8>(uint8(ENetMsg::OwnerChange));
                    ownerWriter.writeVarUInt(netId);
                    ownerWriter.writeVarUInt(replicated.comp->ownerClientId);
                    m_host.send(peer, ownerWriter.data(), ENetDelivery::Reliable, ChannelSession);
                }
        }
        // Synced tweak values ride the same ordered channel: the joiner starts with the server's
        // gameplay configuration instead of its own local defaults.
        {
            oc::vector<oc::vector<uint8>> chunks;
            TweakRegistry::get().packSynced(chunks);
            for (const oc::vector<uint8>& chunk : chunks)
                sendEventTo(peer, TweakSyncEventName, 0, 0, chunk);
        }
        Log::info("Network: client " + oc::to_string(clientId) + " ready (" + oc::to_string(m_readyPeers.size())
            + " total, " + oc::to_string(m_dynamicSpawns.size()) + " spawns replayed)");
        if (m_onClientJoined)
            m_onClientJoined(clientId); // after the replay: per-player spawns arrive via this frame's announce
        break;
    }
    case ENetMsg::Welcome:
    {
        if (m_role != ENetRole::Client)
            break;
        const uint16 version = reader.read<uint16>();
        const uint64 serverTick = reader.readVarUInt();
        const float snapshotHz = reader.read<float>();
        const uint32 clientId = uint32(reader.readVarUInt());
        if (reader.overflowed())
            break;
        // a (re)connect rebuilds the replicated world from the replay that follows this Welcome:
        // drop everything left from a previous session — stale ghosts would linger unsynced, and
        // after a SERVER restart its fresh id counter would collide with our old ids, splattering
        // snapshots/ownership onto the wrong entities. No-op on the first connect (registry empty).
        // Roots collected under the lock, destroyed outside it (the destroy cascade unregisters).
        {
            oc::vector<Entity*> staleRoots;
            {
                const std::lock_guard<std::mutex> lock(m_entityMutex);
                for (const auto& [netId, replicated] : m_entities)
                {
                    Entity* root = replicated.entity;
                    while (root->parent)
                        root = root->parent;
                    if (oc::find(staleRoots.begin(), staleRoots.end(), root) == staleRoots.end())
                        staleRoots.push_back(root);
                }
            }
            if (!staleRoots.empty())
                Log::info("Network: dropping " + oc::to_string(staleRoots.size()) + " replicated roots from the previous session");
            for (Entity* root : staleRoots)
                Globals::world.removeRootEntity(root);
            m_claimRings.clear();
        }
        m_localClientId = clientId; // what makes authority() == LocalOwner resolve for our entities
        m_serverSnapshotHz = glm::clamp(snapshotHz, 1.0f, 240.0f); // the interp playback clock runs in the server's tick units
        Log::info("Network: welcomed by server as client " + oc::to_string(clientId) + " (version "
            + oc::to_string(version) + ", " + oc::to_string(snapshotHz) + " Hz, tick " + oc::to_string(serverTick) + ")");
        // Events that raced ahead of this Welcome on their own channel replay now, in arrival order.
        for (const oc::vector<uint8>& pending : oc::exchange(m_preWelcomeEvents, {}))
            handleEventMessage(m_serverPeer, pending);
        break;
    }
    case ENetMsg::Deny:
    {
        const oc::string_view reason = reader.readString();
        Log::warning("Network: server denied us: " + oc::string(reason));
        break;
    }
    default:
        break;
    }
}

void NetworkManager::handleSpawnMessage(NetReader& reader)
{
    const uint32 baseId = uint32(reader.readVarUInt());
    const uint32 count = uint32(reader.readVarUInt());
    const uint32 ownerClientId = uint32(reader.readVarUInt());
    const oc::string path(reader.readString()); // copied: the spawn below outlives the packet buffer
    const glm::vec3 pos = reader.read<glm::vec3>();
    const glm::quat rot = reader.read<glm::quat>();
    const float scale = reader.read<float>();
    glm::vec3 linVel = reader.read<glm::vec3>();
    glm::vec3 angVel = reader.read<glm::vec3>();
    if (reader.overflowed() || baseId == 0 || count == 0 || path.empty())
        return;
    if (!isFinite(pos) || !isFinite(rot) || !isFinite(scale) || scale <= 0.0f)
        return;
    if (!isFinite(linVel) || !isFinite(angVel))
        return;
    // wire values, so cap them: a hostile server must not launch bodies at absurd speeds
    const float linSpeed = glm::length(linVel), angSpeed = glm::length(angVel);
    if (linSpeed > 200.0f)
        linVel *= 200.0f / linSpeed;
    if (angSpeed > 100.0f)
        angVel *= 100.0f / angSpeed;
    // the path goes to the asset loader, so keep it inside Assets/: a hostile server must not be
    // able to aim a client's file reads anywhere on its disk
    if (path.size() > 256 || path.find("..") != oc::string::npos || path.find(':') != oc::string::npos
        || path.front() == '/' || path.front() == '\\')
    {
        Log::warning("Network: rejecting replicated spawn with suspicious path '" + path + "'");
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(m_entityMutex);
        if (m_entities.contains(baseId))
            return; // duplicate (late-joiner replay overlapping the frame's announce) — idempotent by design
    }
    // spawn the same prefab locally with the server's ids forced onto its NetworkComponents in tree
    // order; main thread, before the entity pass, same context handleEntityChange spawns from
    m_incomingSpawnBase = baseId;
    m_incomingSpawnCount = count;
    m_incomingSpawnCursor = 0;
    m_incomingSpawnOwner = ownerClientId;
    const Transform transform(pos, scale, rot);
    EntityPtr spawned = path.find('.') != oc::string::npos
        ? Globals::world.spawnAssetFile(path, transform, true)
        : Globals::world.spawn(path, transform); // record carried a prefab NAME, not a file path
    const uint32 registered = m_incomingSpawnCursor;
    m_incomingSpawnBase = m_incomingSpawnCount = m_incomingSpawnCursor = 0;
    m_incomingSpawnOwner = 0;
    if (!spawned)
    {
        Log::warning("Network: failed to spawn replicated '" + path + "' (missing asset?)");
        return;
    }
    if (registered != count)
        Log::warning("Network: replicated '" + path + "' registered " + oc::to_string(registered)
            + " NetworkComponents, server announced " + oc::to_string(count) + " (asset mismatch?)");
    // The announced launch velocities start the local twin moving like the original (a projectile
    // flies instead of dropping from rest). Main thread pre-physics — direct setters sanctioned.
    if (PhysicsComponent* pc = getComponent<PhysicsComponent>(spawned.get());
        pc && pc->bodyType == EPhysicsBodyType::Dynamic && pc->body.isValid())
    {
        pc->body.setLinearVelocity(linVel);
        pc->body.setAngularVelocity(angVel);
    }
    Globals::world.addRootEntity(oc::move(spawned));
}

void NetworkManager::handleDespawnMessage(NetReader& reader)
{
    const uint32 baseId = uint32(reader.readVarUInt());
    if (reader.overflowed() || baseId == 0)
        return;
    Entity* entity = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_entityMutex);
        if (const auto it = m_entities.find(baseId); it != m_entities.end())
            entity = it->second.entity;
    }
    if (entity)
        Globals::world.removeRootEntity(entity); // drops the world's ref; the destroy cascade unregisters the ids
}

void NetworkManager::setOwner(Entity& root, uint32 clientId)
{
    if (m_role != ENetRole::Server)
        return;
    // ownership is whole-tree: every NetworkComponent below the root follows the same client
    const oc::function<void(Entity&)> apply = [&](Entity& entity)
    {
        if (NetworkComponent* comp = getComponent<NetworkComponent>(&entity))
        {
            comp->ownerClientId = clientId;
            comp->state->server.lastAcceptedClaimSeq = 0; // next claim seeds the validation anchor
            comp->state->server.claimStreamSeq = 0;       // don't pass a previous owner's claims through
        }
        if (SceneComponent* scene = getComponent<SceneComponent>(&entity))
            for (const EntityPtr& child : scene->children)
                apply(*child);
    };
    apply(root);
    // stamp the spawn record so the Spawn message carries the owner. The root grouping map lives
    // only until this frame's send() — setOwner must be called in the spawn's own frame (live
    // ownership TRANSFER after the announce needs its own message, future work)
    if (const auto it = m_dynamicRootIds.find(&root); it != m_dynamicRootIds.end())
        m_dynamicSpawns[it->second].ownerClientId = clientId;
    else
        Log::warning("Network: setOwner on '" + oc::string(root.getName())
            + "' after its spawn was announced - clients will not learn the ownership");
}

// ---- GAME BLOB: GameUnitComponent state riding the entity sync. Snapshot records (server ->
// clients) and claim packets (owner -> server) append these 5 bytes whenever the entity carries
// the component, so units AND player capsules (puppets) sync shields/teams/materials through the
// ONE per-entity stream — no separate game event. Layout: healthFrac u8, energyFrac u8, emitter
// output u8 (over 0..8), materialsFrac u8, flags u8 (bit0 collapsed, bits 4-6 team).
constexpr float GameBlobOutputRange = 8.0f;
constexpr size_t GameBlobBytes = 5;

static bool packGameStateBlob(Entity* entity, uint8 out[GameBlobBytes])
{
    const GameUnitComponent* unit = getComponent<GameUnitComponent>(entity);
    if (!unit)
        return false;
    const auto frac8 = [](float v) { return (uint8)glm::clamp(v * 255.0f, 0.0f, 255.0f); };
    float output = 0.0f;
    if (const ForceComponent* fc = getComponent<ForceComponent>(entity); fc && fc->emitter.isValid())
        output = fc->emitter.getOutput();
    out[0] = frac8(unit->health / glm::max(unit->healthMax, 1e-3f));
    out[1] = frac8(unit->energy / glm::max(unit->energyMax, 1e-3f));
    out[2] = frac8(output / GameBlobOutputRange);
    out[3] = frac8(unit->materialsFrac);
    out[4] = uint8((unit->collapsed ? 1u : 0u) | (uint32(glm::min((int)unit->team, 7)) << 4u));
    return true;
}

// Which parts of the blob the receiver adopts, by direction:
// * applyShield — health/battery/output. Off on the RECEIVING OWNER: it computes its own.
// * applyMaterials / applyTeam — server-authoritative, so a snapshot (server -> anyone, the owner
//   INCLUDED) always applies them while a claim (owner -> server) never does: a client must not
//   be able to mint its own inventory, nor re-team its twin's field into the enemy's bubbles.
//   Both apply independently of applyShield — an owner still learns the team the server gave it.
static void applyGameStateBlob(Entity* entity, const uint8 blob[GameBlobBytes],
    bool applyShield, bool applyMaterials, bool applyTeam)
{
    GameUnitComponent* unit = getComponent<GameUnitComponent>(entity);
    if (!unit)
        return;
    ForceComponent* fc = getComponent<ForceComponent>(entity);
    const bool hasEmitter = fc && fc->emitter.isValid();
    if (applyMaterials)
        unit->materialsFrac = blob[3] / 255.0f;
    if (applyTeam)
    {
        unit->team = (blob[4] >> 4u) & 7u;
        if (hasEmitter)
            fc->emitter.setTeam(unit->team);
    }
    if (!applyShield)
        return;
    unit->health = blob[0] / 255.0f * unit->healthMax;
    unit->energy = blob[1] / 255.0f * unit->energyMax;
    unit->collapsed = (blob[4] & 1u) != 0;
    if (hasEmitter) // the remote bubble renders the true drain/collapse state
        fc->emitter.setOutput(glm::max(blob[2] / 255.0f * GameBlobOutputRange, 0.01f));
}

void NetworkManager::sendClaims()
{
    // every packet carries the ring's full recent history: a claim only vanishes if ClaimRedundancy
    // consecutive packets drop, without any reliable-channel head-of-line stall
    const std::lock_guard<std::mutex> lock(m_entityMutex);
    for (const auto& [netId, replicated] : m_entities)
    {
        NetworkComponent* comp = replicated.comp;
        if (comp->authority() != ENetAuthority::LocalOwner)
            continue;
        Entity* entity = replicated.entity;

        const PhysicsComponent* physics = getComponent<PhysicsComponent>(entity);
        const bool dynamicBody = physics && physics->bodyType == EPhysicsBodyType::Dynamic && physics->body.isValid();
        if (dynamicBody)
        {
            // claim-side twin of the snapshot sleep policy: every tick while awake, ONE final
            // rest-pose claim on the awake->asleep edge, then silence — an owned-but-settled object
            // costs nothing upstream (sleepDirty serves both policies: the server uses it for
            // snapshots, the owning client here — different processes, never the same entity role)
            if (physics->body.isAwake())
                comp->state->sleepDirty = true;
            else if (comp->state->sleepDirty)
                comp->state->sleepDirty = false;
            else
                continue;
        }

        ClaimRing& ring = m_claimRings[netId];
        const uint32 seq = ++comp->state->client.claimSeq;
        ClaimRecord& record = ring.records[seq % ClaimRedundancy];
        record.input = comp->state->input;
        if (dynamicBody)
        {
            // the raw stepped pose, not the interpolated render pose — that one trails the sim by
            // up to a step, and smoothing belongs on the observer's playback buffer
            record.pos = physics->body.getPosition();
            record.rot = physics->body.getRotation();
            record.linVel = physics->body.getLinearVelocity();
            record.angVel = physics->body.getAngularVelocity();
        }
        else
        {
            record.pos = entity->pos;
            record.rot = entity->rot;
            record.linVel = glm::vec3(0.0f);
            record.angVel = glm::vec3(0.0f);
        }
        ring.newestSeq = seq;
        ring.validCount = glm::min(ring.validCount + 1, ClaimRedundancy);

        // compact wire form: rot/velocities quantized like snapshots (the live-tweakable ranges ride
        // in the packet so the server always decodes with what we encoded), and `look` is sent ONCE
        // per packet (temporally it barely changes across the redundancy window)
        const uint32 count = glm::min(ring.validCount, uint32(glm::clamp(s_claimRedundancy, 1, int(ClaimRedundancy))));
        const float maxVel = glm::max(1.0f, s_maxVel);
        const float maxAngVel = glm::max(1.0f, s_maxAngVel);
        // owner-computed game state (the player capsule's shield) rides once per packet, like look
        uint8 gameBlob[GameBlobBytes];
        const bool hasGame = packGameStateBlob(entity, gameBlob);
        uint8 buffer[1024];
        NetWriter writer(buffer);
        writer.write<uint8>(uint8(ENetMsg::Claim));
        writer.writeVarUInt(netId);
        writer.writeVarUInt(seq);
        writer.write<uint8>(uint8(count));
        writer.write<uint8>(uint8((s_quantize ? 1u : 0u) | (hasGame ? 2u : 0u)));
        if (s_quantize)
        {
            writer.write<float>(maxVel);
            writer.write<float>(maxAngVel);
        }
        for (uint32 i = 0; i < count; ++i) // oldest -> newest
        {
            const ClaimRecord& rec = ring.records[(seq - count + 1 + i) % ClaimRedundancy];
            writer.write<uint32>(rec.input.buttons);
            writer.write(rec.input.move);
            writer.write(rec.pos);
            if (s_quantize)
            {
                writer.write<uint32>(packQuat(rec.rot));
                for (int c = 0; c < 3; ++c) writer.writeQuantized<uint16>(rec.linVel[c], -maxVel, maxVel);
                for (int c = 0; c < 3; ++c) writer.writeQuantized<uint16>(rec.angVel[c], -maxAngVel, maxAngVel);
            }
            else
            {
                writer.write(rec.rot);
                writer.write(rec.linVel);
                writer.write(rec.angVel);
            }
        }
        writer.write(record.input.look); // newest look, applied to every record server-side
        if (hasGame)
            for (size_t c = 0; c < GameBlobBytes; ++c)
                writer.write<uint8>(gameBlob[c]);
        assert(!writer.overflowed());
        m_host.send(m_serverPeer, writer.data(), ENetDelivery::Unreliable, ChannelClaim);
    }
}

void NetworkManager::handleClaimMessage(NetPeerId peer, NetReader& reader)
{
    const auto peerIt = m_peerClients.find(peer);
    if (peerIt == m_peerClients.end())
        return; // not past the handshake
    const uint32 senderClientId = peerIt->second;

    const uint32 netId = uint32(reader.readVarUInt());
    const uint32 newestSeq = uint32(reader.readVarUInt());
    const uint32 count = reader.read<uint8>();
    const uint8 claimFlags = reader.read<uint8>();
    if (reader.overflowed() || netId == 0 || count == 0 || count > ClaimRedundancy || (claimFlags & ~3))
        return;
    const bool quantized = (claimFlags & 1) != 0;
    const bool hasGame = (claimFlags & 2) != 0;
    float maxVel = s_maxVel;       // only read (and only used) when the payload is quantized
    float maxAngVel = s_maxAngVel;
    if (quantized) // the owner encoded with ITS live ranges; decode with the same
    {
        maxVel = glm::clamp(reader.read<float>(), MinWireQuantizeRange, MaxWireQuantizeRange);
        maxAngVel = glm::clamp(reader.read<float>(), MinWireQuantizeRange, MaxWireQuantizeRange);
    }

    Entity* entity = nullptr;
    NetworkComponent* comp = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_entityMutex);
        if (const auto it = m_entities.find(netId); it != m_entities.end())
        {
            entity = it->second.entity;
            comp = it->second.comp;
        }
    }
    if (!comp || comp->ownerClientId != senderClientId)
        return; // unknown entity, or a client claiming something it doesn't own (spoof) — drop

    // parse everything first: `look` rides once at the tail (it barely changes over the redundancy
    // window) and applies to every record
    struct ParsedClaim
    {
        NetInputState input;
        glm::vec3 pos;
        glm::quat rot;
        glm::vec3 linVel;
        glm::vec3 angVel;
    };
    ParsedClaim claims[ClaimRedundancy];
    for (uint32 i = 0; i < count; ++i) // oldest -> newest
    {
        ParsedClaim& claim = claims[i];
        claim.input.buttons = reader.read<uint32>();
        claim.input.move = reader.read<glm::vec3>();
        claim.pos = reader.read<glm::vec3>();
        if (quantized)
        {
            claim.rot = unpackQuat(reader.read<uint32>());
            for (int c = 0; c < 3; ++c) claim.linVel[c] = reader.readQuantized<uint16>(-maxVel, maxVel);
            for (int c = 0; c < 3; ++c) claim.angVel[c] = reader.readQuantized<uint16>(-maxAngVel, maxAngVel);
        }
        else
        {
            claim.rot = reader.read<glm::quat>();
            claim.linVel = reader.read<glm::vec3>();
            claim.angVel = reader.read<glm::vec3>();
        }
    }
    const glm::vec3 look = reader.read<glm::vec3>();
    uint8 gameBlob[GameBlobBytes];
    if (hasGame)
        for (size_t c = 0; c < GameBlobBytes; ++c)
            gameBlob[c] = reader.read<uint8>();
    if (reader.overflowed() || !isFinite(look))
        return;
    // The owner's self-computed game state (its shield) lands on the twin regardless of how the
    // pose claims fare — it is display/team state, and this replaces the old GqE report event.
    // Materials stay server-authoritative: a client cannot claim its own inventory.
    if (hasGame)
        applyGameStateBlob(entity, gameBlob, /*applyShield*/ true, /*applyMaterials*/ false,
            /*applyTeam*/ false);
    for (uint32 i = 0; i < count; ++i)
    {
        // hostile-input gate: non-finite values would pass every plausibility test below (all
        // comparisons against NaN are false) and reach the solver; a non-unit quat skews the body
        ParsedClaim& claim = claims[i];
        if (!isFinite(claim.pos) || !isFinite(claim.linVel) || !isFinite(claim.angVel)
            || !isFinite(claim.input.move) || !sanitizeRotation(claim.rot))
            return;
        claim.input.look = look;
    }

    PhysicsComponent* physics = getComponent<PhysicsComponent>(entity);
    const bool dynamicBody = physics && physics->bodyType == EPhysicsBodyType::Dynamic && physics->body.isValid();

    for (uint32 i = 0; i < count; ++i) // oldest -> newest
    {
        const uint32 seq = newestSeq - count + 1 + i;
        const NetInputState& input = claims[i].input;
        const glm::vec3 pos = claims[i].pos;
        const glm::quat rot = claims[i].rot;
        const glm::vec3 linVel = claims[i].linVel;
        const glm::vec3 angVel = claims[i].angVel;
        if (seq <= comp->state->server.lastClaimSeq && comp->state->server.lastClaimSeq != 0)
            continue; // redundant resend of a claim we already processed

        // ---- plausibility gate (Network/Validation tweaks) ----
        // refill from the WALL CLOCK before validating: seq numbers and packet rate are both
        // attacker-controlled, so any per-claim budget mints displacement per packet sent
        {
            NetEntityState::ServerState& sv = comp->state->server;
            const float refill = float(m_netTime - sv.claimBudgetTime) * s_maxClaimSpeed;
            sv.claimBudget = glm::min(sv.claimBudget + glm::max(0.0f, refill), s_maxClaimSpeed * 0.5f);
            sv.claimBudgetTime = m_netTime;
        }
        EClaimResult result = EClaimResult::Accepted;
        const glm::vec3 twinPos = dynamicBody ? physics->body.getPosition() : entity->pos;
        const bool recovering = m_serverTick < comp->state->server.forcedUntilTick;
        // On EVERY path: the two branches below skip the displacement gate by design, and an
        // accepted claim hands its velocity to the solver verbatim (ownership transfer resets the
        // anchor, so the first-claim case would be repeatable on demand).
        if (glm::length(linVel) > s_maxClaimVelocity || glm::length(angVel) > s_maxClaimAngVel)
            result = EClaimResult::RejectedVelocity;
        else if (recovering && glm::length(pos - twinPos) < s_claimReanchorRadius)
        {
            // Anti-deadlock: a stale anchor walled off from the twin would veto forever. Gated on
            // `recovering` because it BYPASSES validation — ungated, every normal claim lands here
            // (accepted claims pin the twin) and an attacker walks the radius per packet.
        }
        else if (comp->state->server.lastAcceptedClaimSeq == 0)
        {
            // first claim seeds the anchor — validated against the twin's CURRENT authoritative state
            // (the server just spawned/handed it over), or an attacker's opening claim could teleport
            // anywhere once per entity for free
            if (glm::length(pos - twinPos) > s_claimTeleportCap)
                result = EClaimResult::RejectedTeleport;
        }
        else
        {
            const glm::vec3 delta = pos - comp->state->server.lastAcceptedClaimPos;
            const float displacement = glm::length(delta);
            if (displacement > s_claimTeleportCap)
                result = EClaimResult::RejectedTeleport;
            else if (displacement > comp->state->server.claimBudget)
                result = EClaimResult::RejectedSpeed;
            else if (s_claimPathRaycast && displacement > 0.01f
                && Globals::physics.castRayClosest(comp->state->server.lastAcceptedClaimPos, delta, PhysicsLayers::All,
                    dynamicBody ? &physics->body : nullptr, true /*staticOnly*/).hit)
                // trajectory crosses STATIC world geometry (wall teleport). The twin's own body is
                // excluded (it stands in this very path) and so is every other dynamic body — a
                // pushed cube sits right in front of the pusher's claims, and validating against it
                // rejected the pusher's OWN movement (Forced loop = "lost authority of my cube")
                result = EClaimResult::RejectedPath;
        }

        comp->state->server.lastClaimResult = result;
        if (result == EClaimResult::Accepted)
        {
            // authority is back with the owner: stop forcing corrections IMMEDIATELY. Leaving the
            // window armed after acceptance drags the owner toward its own RTT-old state for the
            // remainder — a constant backward pull while moving ("stuck in mud")
            comp->state->server.forcedUntilTick = 0;
            comp->state->server.lastAcceptedClaimSeq = seq;
            // spend the bucket on what this claim actually moved (the re-anchor and first-claim
            // paths spend too — otherwise they would be a free displacement channel)
            comp->state->server.claimBudget = glm::max(0.0f,
                comp->state->server.claimBudget - glm::length(pos - comp->state->server.lastAcceptedClaimPos));
            comp->state->server.lastAcceptedClaimPos = pos;
            comp->state->input = input; // server-side gameplay reads the owner's intent from here
            // claim passthrough source: snapshots re-emit this instead of sampling the twin
            comp->state->server.claimStreamPos = pos;
            comp->state->server.claimStreamRot = rot;
            comp->state->server.claimStreamLinVel = linVel;
            comp->state->server.claimStreamAngVel = angVel;
            comp->state->server.claimStreamSeq = seq;
            if (dynamicBody && comp->state->server.arbitratedUntilTick > m_serverTick)
            {
                // ARBITRATION (player-vs-player contact): the solver owns the pose — the claim
                // becomes bounded velocity INTENT, so the shoving contest resolves with real
                // contacts server-side instead of two teleport-pinned bodies interpenetrating
                Globals::physics.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::NudgeVelocity,
                    linVel, angVel, glm::vec3(s_maxClaimSpeed * 0.5f, s_maxAngVel * 0.5f, 0.0f));
            }
            else if (dynamicBody)
            {
                // Follow the claim through the SOLVER rather than teleport-pinning it. Pinning
                // collapsed the render interpolation and, since claims ride the OWNER's step clock
                // while the server steps on its own, dragged the twin backwards whenever two server
                // steps fell between two claims. The twin only has to be approximately right for
                // server-side collisions — other clients get the owner's stream via passthrough.
                const glm::quat twinRot = physics->body.getRotation();
                glm::vec3 desiredLin = linVel + (pos - twinPos) * s_twinFollowGain;
                if (const float speed = glm::length(desiredLin); speed > s_maxClaimVelocity && speed > 1e-6f)
                    desiredLin *= s_maxClaimVelocity / speed;
                glm::quat rotError = rot * glm::inverse(twinRot);
                if (rotError.w < 0.0f)
                    rotError = -rotError; // shortest arc
                glm::vec3 desiredAng = angVel + glm::axis(rotError) * glm::angle(rotError) * s_twinFollowGain;
                if (const float rate = glm::length(desiredAng); rate > s_maxClaimAngVel && rate > 1e-6f)
                    desiredAng *= s_maxClaimAngVel / rate;
                Globals::physics.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::NudgeVelocity,
                    desiredLin, desiredAng, glm::vec3(s_maxClaimVelocity, s_maxClaimAngVel, 0.0f));

                // hard resync only for divergence the follow can't close (spawn, ownership handover,
                // a long stall). This one IS a teleport, so it stomps the interpolation state.
                if (glm::length(pos - twinPos) > s_twinResyncDistance)
                {
                    Globals::physics.teleportBody(physics->body, pos, rot);
                    Globals::physics.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetLinearVelocity, linVel);
                    Globals::physics.queueBodyCommand(physics->body, PhysicsWorld::EBodyCommand::SetAngularVelocity, angVel);
                    physics->prevPos = physics->currPos = pos;
                    physics->prevRot = physics->currRot = rot;
                }
            }
            else
            {
                // main thread before the entity pass — same context the snapshot targets apply in
                entity->pos = pos;
                entity->rot = rot;
            }
        }
        else
        {
            if (comp->state->server.violations < 0xffff)
                ++comp->state->server.violations;
            // reassert server authority: the owner's snapshot records carry Forced for a while, and
            // the owner's correction gate obeys them — dragging it back to the authoritative state
            comp->state->server.forcedUntilTick = m_serverTick + uint32(glm::max(1, s_forcedTicks));
        }
    }
    comp->state->server.lastClaimSeq = glm::max(comp->state->server.lastClaimSeq, newestSeq);
}

void NetworkManager::transferOwnership(uint32 netId, NetworkComponent* comp, uint32 newOwnerClientId)
{
    comp->ownerClientId = newOwnerClientId;
    comp->state->transferredOwnership = newOwnerClientId != 0;
    comp->state->server.releaseTimer = 0.0f;
    // the next claim re-seeds against the twin's live state; without the reset the new owner's low
    // sequence numbers would be dropped as duplicates of the previous owner's
    comp->state->server.lastClaimSeq = 0;
    comp->state->server.lastAcceptedClaimSeq = 0;
    comp->state->server.lastClaimResult = EClaimResult::None;
    comp->state->server.forcedUntilTick = 0;
    comp->state->server.claimStreamSeq = 0; // don't pass the previous owner's claims through
    comp->state->server.claimStreamSentSeq = 0;
    comp->state->server.claimBudget = 0.0f; // fresh movement bucket, refilled from now
    comp->state->server.claimBudgetTime = m_netTime;

    uint8 buffer[16];
    NetWriter writer(buffer);
    writer.write<uint8>(uint8(ENetMsg::OwnerChange));
    writer.writeVarUInt(netId);
    writer.writeVarUInt(newOwnerClientId);
    for (const NetPeerId peer : m_readyPeers)
        m_host.send(peer, writer.data(), ENetDelivery::Reliable, ChannelSession);
}

void NetworkManager::setOwnershipTransfers(bool enabled)
{
    s_transferEnabled = enabled; // writes through the tweak's live variable — the panel shows it
}

void NetworkManager::setServerPrimary(Entity& entity, bool primary)
{
    if (m_role != ENetRole::Server)
        return; // single player / client: no session state to mark
    NetworkComponent* comp = getComponent<NetworkComponent>(&entity);
    if (comp && comp->netId != 0 && comp->state)
        comp->state->server.serverPrimary = primary;
}

void NetworkManager::updateOwnershipTransfers(double deltaSec)
{
    if (!s_transferEnabled)
        return;
    const std::lock_guard<std::mutex> lock(m_entityMutex);

    // transfer sources: every client's PRIMARY (non-transferred) owned dynamic bodies, plus the
    // SERVER's own primary under clientId 0 (it re-claims, never acquires for a client)
    m_transferSources.clear();
    for (const auto& [netId, replicated] : m_entities)
    {
        const NetworkComponent* comp = replicated.comp;
        const bool clientPrimary = comp->ownerClientId != 0 && !comp->state->transferredOwnership;
        const bool serverPrimary = comp->ownerClientId == 0 && comp->state->server.serverPrimary;
        if (!clientPrimary && !serverPrimary)
            continue;
        const PhysicsComponent* physics = getComponent<PhysicsComponent>(replicated.entity);
        if (physics && physics->bodyType == EPhysicsBodyType::Dynamic && physics->body.isValid())
            m_transferSources.emplace_back(comp->ownerClientId, physics->body.getPosition());
    }
    if (m_transferSources.empty())
        return;

    const float transferRadiusSq = s_transferRadius * s_transferRadius;
    const float releaseRadiusSq = s_releaseRadius * s_releaseRadius;
    for (const auto& [netId, replicated] : m_entities)
    {
        NetworkComponent* comp = replicated.comp;
        // contest-history aging (cheap, every networked entity — the slots are inline)
        comp->state->server.contestAges[0] += float(deltaSec);
        comp->state->server.contestAges[1] += float(deltaSec);
        const PhysicsComponent* physics = getComponent<PhysicsComponent>(replicated.entity);
        if (!physics || physics->bodyType != EPhysicsBodyType::Dynamic || !physics->body.isValid())
            continue;
        const glm::vec3 pos = physics->body.getPosition();

        if (comp->ownerClientId == 0)
        {
            if (comp->state->server.serverPrimary)
                continue; // the server's own player is never handed to a client
            if (comp->state->server.contestedUntilTick > m_serverTick)
                continue; // contested: stays server-owned until the window decays
            if (!physics->body.isAwake())
                continue; // sleeping props stay server-owned (near-zero cost) — the contact steal
                          // takes over the instant a player actually hits one, grace covers the RTT
            // server-owned AND moving: hand it to the first client whose primary body is close
            // enough — its physics then drives the object, so pushing it feels local to that player
            for (const auto& [clientId, sourcePos] : m_transferSources)
                if (clientId != 0 && glm::dot(sourcePos - pos, sourcePos - pos) < transferRadiusSq)
                {
                    transferOwnership(netId, comp, clientId);
                    break;
                }
        }
        else if (comp->state->transferredOwnership)
        {
            // The SERVER's primary RE-CLAIMS by proximity, exactly like a client acquires — the
            // server player pushing a client-held object takes its simulation back. Only ever
            // touches TRANSFERRED objects; a client's own primary is never in this branch.
            bool reclaimed = false;
            for (const auto& [clientId, sourcePos] : m_transferSources)
                if (clientId == 0 && glm::dot(sourcePos - pos, sourcePos - pos) < transferRadiusSq)
                {
                    transferOwnership(netId, comp, 0);
                    reclaimed = true;
                    break;
                }
            if (reclaimed)
                continue;
            // release with hysteresis once away from the owner's primaries (an owner's own primary
            // never releases — it is one of the sources, at distance zero from itself)
            float nearestSq = FLT_MAX;
            for (const auto& [clientId, sourcePos] : m_transferSources)
                if (clientId == comp->ownerClientId)
                    nearestSq = glm::min(nearestSq, glm::dot(sourcePos - pos, sourcePos - pos));
            if (nearestSq < releaseRadiusSq)
                comp->state->server.releaseTimer = 0.0f;
            else if ((comp->state->server.releaseTimer += float(deltaSec)) > s_releaseDelaySec)
                transferOwnership(netId, comp, 0);
        }
    }
}

void NetworkManager::stealOwnershipOnContact(Entity& object, uint32 byClientId)
{
    if (m_role != ENetRole::Server || byClientId == 0 || !s_transferEnabled)
        return;
    NetworkComponent* comp = getComponent<NetworkComponent>(&object);
    if (!comp || comp->netId == 0)
        return;
    const auto ticksFromSec = [](float sec) { return uint32(glm::max(1.0f, sec * glm::clamp(s_snapshotHz, 1.0f, 240.0f))); };

    // PLAYER vs PLAYER: never stealable — instead BOTH primaries enter server ARBITRATION for the
    // window: their twins' poses become solver-owned (claims apply as velocity intent, see
    // handleClaimMessage) so the shoving contest resolves with real contacts in ONE place, and each
    // owner softly corrects toward the arbitrated result while still steering
    if (comp->ownerClientId != 0 && !comp->state->transferredOwnership)
    {
        if (comp->ownerClientId == byClientId)
            return;
        const uint32 until = m_serverTick + ticksFromSec(s_arbitrateSec);
        comp->state->server.arbitratedUntilTick = until;
        const std::lock_guard<std::mutex> lock(m_entityMutex);
        for (const auto& [netId, replicated] : m_entities) // the toucher's own primary arbitrates too
            if (replicated.comp->ownerClientId == byClientId && !replicated.comp->state->transferredOwnership)
                replicated.comp->state->server.arbitratedUntilTick = until;
        return;
    }

    // OBJECT: track the last two DISTINCT clients to touch it (ages ticked in updateOwnershipTransfers)
    if (comp->state->server.contestClients[0] != byClientId)
    {
        comp->state->server.contestClients[1] = comp->state->server.contestClients[0];
        comp->state->server.contestAges[1] = comp->state->server.contestAges[0];
        comp->state->server.contestClients[0] = byClientId;
    }
    comp->state->server.contestAges[0] = 0.0f;

    // both slots fresh with distinct clients = CONTESTED: the server owns it while the contest lasts
    // (the last-collider ping-pong would re-seed the twin between two clients' versions every hit)
    if (comp->state->server.contestClients[1] != 0 && comp->state->server.contestClients[1] != comp->state->server.contestClients[0]
        && comp->state->server.contestAges[1] < s_contestSec)
    {
        comp->state->server.contestedUntilTick = m_serverTick + ticksFromSec(s_contestSec);
        if (comp->ownerClientId != 0)
            transferOwnership(comp->netId, comp, 0);
        return;
    }
    if (comp->state->server.contestedUntilTick > m_serverTick)
        return; // contest cooling down: stays server-owned until the window decays
    if (comp->ownerClientId == byClientId)
        return; // already ours
    transferOwnership(comp->netId, comp, byClientId);
}

void NetworkManager::handleOwnerChangeMessage(NetReader& reader)
{
    const uint32 netId = uint32(reader.readVarUInt());
    const uint32 newOwnerClientId = uint32(reader.readVarUInt());
    if (reader.overflowed() || netId == 0)
        return;
    // fresh ownership episode = fresh redundancy ring: the old one holds the previous episode's
    // positions, and replaying them through the server's reset dedup would teleport the object back
    m_claimRings.erase(netId);
    const std::lock_guard<std::mutex> lock(m_entityMutex);
    if (const auto it = m_entities.find(netId); it != m_entities.end())
    {
        NetworkComponent* comp = it->second.comp;
        comp->ownerClientId = newOwnerClientId;
        // "transferred" client-side means "owned, but not one of my primaries": the claim stream
        // carries it, player control (which drives primaries only) leaves it alone
        comp->state->transferredOwnership = newOwnerClientId != 0 && newOwnerClientId == m_localClientId;
        if (newOwnerClientId == m_localClientId)
        {
            // now OURS: drop the interpolation history recorded while someone else owned it — a
            // stale ring reaching the playback path would teleport the entity into the past
            comp->state->client.remoteBuffer = nullptr;
            m_remoteBuffers.erase(netId);
        }
    }
}

void NetworkManager::sendSpawnTo(NetPeerId peer, const DynamicSpawn& record)
{
    // transform refreshed from the live ROOT when the base id is still registered, so a late joiner
    // spawns the entity where it IS, not where it was born. The base id's entity is the FIRST
    // NetworkComponent registered in the tree — which may be a CHILD (a Scene-only root carries no
    // component), so walk up: reading the child would ship its LOCAL offset as the root transform.
    glm::vec3 pos = record.spawnPos;
    glm::quat rot = record.spawnRot;
    float scale = record.spawnScale;
    glm::vec3 linVel(0.0f), angVel(0.0f);
    {
        const std::lock_guard<std::mutex> lock(m_entityMutex);
        if (const auto it = m_entities.find(record.baseId); it != m_entities.end())
        {
            Entity* root = it->second.entity;
            while (root->parent)
                root = root->parent;
            pos = root->pos; // root local == world
            rot = root->rot;
            scale = root->scale;
            // The root body's velocities ride along, so the receiver's LOCAL twin starts moving
            // like the original instead of dropping from rest until corrections catch it — the
            // announce goes out in send(), AFTER the spawner set any launch velocity (projectiles).
            if (const PhysicsComponent* pc = getComponent<PhysicsComponent>(root);
                pc && pc->bodyType == EPhysicsBodyType::Dynamic && pc->body.isValid())
            {
                linVel = pc->body.getLinearVelocity();
                angVel = pc->body.getAngularVelocity();
            }
        }
    }
    uint8 buffer[512];
    NetWriter writer(buffer);
    writer.write<uint8>(uint8(ENetMsg::Spawn));
    writer.writeVarUInt(record.baseId);
    writer.writeVarUInt(record.componentCount);
    writer.writeVarUInt(record.ownerClientId);
    writer.writeString(record.path);
    writer.write(pos);
    writer.write(rot);
    writer.write<float>(scale);
    writer.write(linVel);
    writer.write(angVel);
    if (writer.overflowed())
    {
        Log::warning("Network: spawn path too long, not replicated: " + record.path);
        return;
    }
    m_host.send(peer, writer.data(), ENetDelivery::Reliable, ChannelSession);
}

void NetworkManager::handleEventMessage(NetPeerId peer, oc::span<const uint8> bytes)
{
    // a transport-connected peer that never completed Hello/Welcome is not a player: ungated, anyone
    // who can open a connection fires arbitrary script events and gets them relayed to every client
    if (m_role == ENetRole::Server)
    {
        if (!m_peerClients.contains(peer))
            return;
    }
    else if (peer != m_serverPeer)
        return; // clients accept events only from the server they are welcomed by
    else if (m_localClientId == 0)
    {
        // Events (ch1) share no ordering with the ch2 Welcome, so the server's join-time burst
        // (game world replay, tweak sync) can arrive BEFORE we process the Welcome. The reliable
        // transport has already acked these — dropping here loses them forever. Park them and
        // replay the moment the Welcome lands.
        if (m_preWelcomeEvents.size() < 256)
            m_preWelcomeEvents.emplace_back(bytes.begin(), bytes.end());
        return;
    }

    NetReader reader(bytes.subspan(1));
    // IDENTITY: the server takes it from the connection the packet arrived on (a client-supplied one
    // would be a free impersonation), and the wire carries no such field in that direction. A client
    // reads it, because the origin is a third party its own connection knows nothing about, and only
    // the server ever writes that copy.
    const uint32 senderClientId = m_role == ENetRole::Server ? m_peerClients[peer]
                                                             : uint32(reader.readVarUInt());
    const uint32 senderNetId = uint32(reader.readVarUInt());
    const oc::string_view name = reader.readString();
    const size_t dataSize = size_t(reader.readVarUInt());
    // both are attacker-controlled; the name also reaches the log ring
    if (reader.overflowed() || name.empty() || name.size() > MaxEventNameLength || dataSize > MaxEventDataBytes)
        return;
    const oc::span<const uint8> data = reader.readBytes(dataSize);
    if (reader.overflowed())
        return;

    // AUTHORIZATION (server only): a connected player firing an event it shouldn't is not stopped by
    // identity, only by this. Dropped events are not relayed either, so nothing observes them.
    if (m_role == ENetRole::Server && m_eventFilter
        && !m_eventFilter(senderClientId, name, data, findOwnedEntity(senderNetId, senderClientId)))
    {
        Log::warning("Network: event '" + oc::string(name) + "' from client "
            + oc::to_string(senderClientId) + " refused by the event filter");
        return;
    }

    fireEventAttributed(name, senderClientId, data);
    if (m_role == ENetRole::Server) // re-serialized, not forwarded raw: the id must be ours, not theirs
        for (const NetPeerId other : m_readyPeers)
            if (other != peer)
                sendEventTo(other, name, senderClientId, senderNetId, data);
}

// The netId comes off the wire, so it is a CLAIM. Resolving it without the ownership check would let
// a client pin its events on any entity it likes, including another player's.
Entity* NetworkManager::findEntity(uint32 netId) const
{
    if (netId == 0)
        return nullptr;
    const std::lock_guard<std::mutex> lock(m_entityMutex);
    const auto it = m_entities.find(netId);
    return it != m_entities.end() ? it->second.entity : nullptr;
}

Entity* NetworkManager::findOwnedEntity(uint32 netId, uint32 clientId) const
{
    if (netId == 0 || clientId == 0)
        return nullptr;
    const std::lock_guard<std::mutex> lock(m_entityMutex);
    const auto it = m_entities.find(netId);
    if (it == m_entities.end() || it->second.comp->ownerClientId != clientId)
        return nullptr;
    return it->second.entity;
}

// Fires a named event with `senderClientId` readable for its duration (scripts query it through
// ctx->networkEventSender). Dispatch is synchronous, so a plain scoped set is enough.
void NetworkManager::fireEventAttributed(oc::string_view name, uint32 senderClientId, oc::span<const uint8> data)
{
    if (name == TweakSyncEventName)
    {
        // Engine-reserved tweak sync. Applied only when WE are the client — a hostile client
        // cannot push values to a server — and applySyncedBlob itself ignores every key the
        // receiver did not flag as Synced, clamping to the receiver's own bounds.
        if (m_role == ENetRole::Client)
            TweakRegistry::get().applySyncedBlob(data);
        return;
    }
    const uint32 previousSender = m_currentEventSender;
    const oc::span<const uint8> previousData = m_currentEventData;
    m_currentEventSender = senderClientId;
    m_currentEventData = data;
    Globals::scriptEvents.fireEvent(oc::string(name));
    if (m_onGameEvent)
        m_onGameEvent(name); // C++ listener: sender/data stay readable for the call
    m_currentEventSender = previousSender;
    m_currentEventData = previousData;
}

void NetworkManager::handleSnapshot(NetReader& reader)
{
    const uint32 tick = uint32(reader.readVarUInt());
    const uint8 flags = reader.read<uint8>();
    if (flags & ~SnapshotFlag_Quantized)
    {
        // a future format we don't know how to parse — a per-record size is not recoverable, drop the message
        if (!m_warnedUnknownRecFlags)
        {
            m_warnedUnknownRecFlags = true;
            Log::warning("Network: snapshot with unknown flags " + oc::to_string(flags) + ", dropping (newer server?)");
        }
        return;
    }
    const bool quantized = (flags & SnapshotFlag_Quantized) != 0;
    float maxVel = s_maxVel;       // only read (and only used) when the payload is quantized
    float maxAngVel = s_maxAngVel;
    if (quantized) // the quantization ranges are live tweaks server-side, so they ride in the header
    {
        // clamped like the claim path: a garbage range decodes every velocity in the message
        maxVel = glm::clamp(reader.read<float>(), MinWireQuantizeRange, MaxWireQuantizeRange);
        maxAngVel = glm::clamp(reader.read<float>(), MinWireQuantizeRange, MaxWireQuantizeRange);
    }
    const uint32 count = reader.read<uint16>();

    const std::lock_guard<std::mutex> lock(m_entityMutex);
    for (uint32 i = 0; i < count; ++i)
    {
        const uint32 netId = uint32(reader.readVarUInt());
        const uint8 recFlags = reader.read<uint8>();
        if (recFlags & ~(NetRecFlag_Physics | NetRecFlag_Asleep | NetRecFlag_Forced | NetRecFlag_Arbitrated | NetRecFlag_ServerPlayer | NetRecFlag_Game))
        {
            if (!m_warnedUnknownRecFlags)
            {
                m_warnedUnknownRecFlags = true;
                Log::warning("Network: snapshot record with unknown flags " + oc::to_string(recFlags) + ", dropping message (newer server?)");
            }
            return; // unknown bits = unknown record size, the rest of the message is unparseable
        }
        const glm::vec3 pos = reader.read<glm::vec3>();
        const glm::quat rot = quantized ? unpackQuat(reader.read<uint32>()) : reader.read<glm::quat>();
        glm::vec3 linVel(0.0f);
        glm::vec3 angVel(0.0f);
        if (recFlags & NetRecFlag_Physics)
        {
            if (quantized)
            {
                for (int c = 0; c < 3; ++c) linVel[c] = reader.readQuantized<uint16>(-maxVel, maxVel);
                for (int c = 0; c < 3; ++c) angVel[c] = reader.readQuantized<uint16>(-maxAngVel, maxAngVel);
            }
            else
            {
                linVel = reader.read<glm::vec3>();
                angVel = reader.read<glm::vec3>();
            }
        }
        uint8 gameBlob[GameBlobBytes] = {};
        if (recFlags & NetRecFlag_Game) // parsed even when the entity resolve below misses — record framing
        {
            for (size_t c = 0; c < GameBlobBytes; ++c)
                gameBlob[c] = reader.read<uint8>();
        }
        if (reader.overflowed())
            return;
        glm::quat sanitizedRot = rot;
        if (!isFinite(pos) || !isFinite(linVel) || !isFinite(angVel) || !sanitizeRotation(sanitizedRot))
            return; // same rule as claims: non-finite state must never reach a body or a transform

        const auto it = m_entities.find(netId);
        if (it == m_entities.end())
            continue; // expected transient: the unreliable snapshot outran the reliable Spawn, or the Despawn already landed
        NetworkComponent* comp = it->second.comp;
        if (comp->state->client.hasTarget && tick <= comp->state->client.serverTick)
            continue; // stale (reordered unreliable packet)
        // remote-owned entities also record into their interpolation ring (another player's entity:
        // the observer plays this history back a couple of ticks behind — see NetSnapshotRing).
        // The SERVER's own player rides the same path (flagged — it is server-owned by id).
        if ((recFlags & NetRecFlag_Physics)
            && ((recFlags & NetRecFlag_ServerPlayer)
                || (comp->ownerClientId != 0 && comp->ownerClientId != m_localClientId)))
        {
            oc::unique_ptr<NetSnapshotRing>& ring = m_remoteBuffers[netId];
            if (!ring)
                ring = oc::make_unique<NetSnapshotRing>();
            ring->records[tick % NetSnapshotRing::Capacity] = { tick, pos, sanitizedRot, linVel, angVel };
            ring->newestTick = glm::max(ring->newestTick, tick);
            ring->count = glm::min(ring->count + 1, NetSnapshotRing::Capacity);
            comp->state->client.remoteBuffer = ring.get();
        }
        comp->state->client.targetPos = pos;
        comp->state->client.targetRot = sanitizedRot;
        comp->state->client.targetLinVel = linVel;
        comp->state->client.targetAngVel = angVel;
        comp->state->client.targetFlags = recFlags;
        comp->state->client.serverTick = tick;
        comp->state->client.timeSinceSnapshot = 0.0f;
        comp->state->client.hasTarget = true;
        // game state applies for everyone; the entity's OWNER keeps its self-computed shield and
        // takes only the server-authoritative materials
        if (recFlags & NetRecFlag_Game)
            applyGameStateBlob(it->second.entity, gameBlob,
                /*applyShield*/ comp->ownerClientId != m_localClientId || comp->ownerClientId == 0,
                /*applyMaterials*/ true, /*applyTeam*/ true);
    }
}

void NetworkManager::send(double deltaSec)
{
    if (m_role == ENetRole::None)
        return;
    ProfileScope scope("NetworkManager::send", EProfileCategory::Network);

    // Synced tweaks: whenever any flagged value changed, re-broadcast the full set (small and
    // idempotent — no per-var dirty bookkeeping). Queued here, drained with the other events below.
    if (m_role == ENetRole::Server && TweakRegistry::get().syncGeneration() != m_sentTweakGeneration)
    {
        m_sentTweakGeneration = TweakRegistry::get().syncGeneration();
        oc::vector<oc::vector<uint8>> chunks;
        TweakRegistry::get().packSynced(chunks);
        for (const oc::vector<uint8>& chunk : chunks)
            fireNetworkEvent(TweakSyncEventName, chunk);
    }

    // outgoing events queued this frame (worker-thread script thunks can't touch NetHost themselves)
    oc::vector<PendingEvent> pendingEvents;
    {
        const std::lock_guard<std::mutex> lock(m_eventMutex);
        pendingEvents.swap(m_pendingOutgoingEvents);
    }
    for (const PendingEvent& event : pendingEvents)
    {
        if (m_role == ENetRole::Server)
        {
            for (const NetPeerId peer : m_readyPeers)
                sendEventTo(peer, event.name, 0, event.senderNetId, event.data); // 0 = from the server
        }
        else if (m_serverPeer != InvalidNetPeerId && m_host.isConnected(m_serverPeer))
            // client id unused here: our identity is the connection we send on
            sendEventTo(m_serverPeer, event.name, 0, event.senderNetId, event.data);
    }

    if (m_role == ENetRole::Server)
    {
        updateOwnershipTransfers(deltaSec); // proximity handover of touched objects

        // announce this frame's runtime spawns/despawns to every ready client (reliable, session channel)
        for (const uint32 baseId : m_pendingSpawnAnnounce)
            if (const auto it = m_dynamicSpawns.find(baseId); it != m_dynamicSpawns.end())
                for (const NetPeerId peer : m_readyPeers)
                    sendSpawnTo(peer, it->second);
        m_pendingSpawnAnnounce.clear();
        for (const uint32 baseId : m_pendingDespawn)
        {
            uint8 buffer[16];
            NetWriter writer(buffer);
            writer.write<uint8>(uint8(ENetMsg::Despawn));
            writer.writeVarUInt(baseId);
            for (const NetPeerId peer : m_readyPeers)
                m_host.send(peer, writer.data(), ENetDelivery::Reliable, ChannelSession);
        }
        m_pendingDespawn.clear();
        // spawn-call grouping keys are raw entity pointers, only meaningful within this frame's
        // synchronous spawns — purge before the allocator can recycle an address into a false match
        m_dynamicRootIds.clear();

        const double interval = 1.0 / double(glm::clamp(s_snapshotHz, 1.0f, 240.0f));
        m_snapshotAccum = glm::min(m_snapshotAccum + deltaSec, interval * 4.0); // don't spiral after a hitch
        while (m_snapshotAccum >= interval)
        {
            m_snapshotAccum -= interval;
            ++m_serverTick;
            if (m_host.getConnectedCount() > 0)
                sendSnapshotTick();
        }
    }
    else if (m_serverPeer != InvalidNetPeerId && m_host.isConnected(m_serverPeer) && m_localClientId != 0)
    {
        // Claims are PHASE-LOCKED to the physics step: one claim per step, which is exactly the
        // information the sim produces. A free-running clock at the same nominal Hz beats against
        // the step clock instead, duplicating one step pose and skipping another. Physics paused =
        // no steps = no claims.
        // Thinning counted in STEPS, never wall clock: a step boundary falls partway through a
        // frame while m_netTime advances in whole ones, so a time comparison at the step rate
        // measures just-under-interval and drops that step's claim entirely. Achievable rates are
        // the step rate over a whole number. A frame taking several steps claims only the newest
        // pose (accepted — it is already running below the physics rate).
        const uint32 stepCount = Globals::physics.getStepCount();
        const int stepHz = glm::max(1, Globals::physics.getStepHz());
        const uint32 stepsPerClaim = uint32(glm::max(1.0f,
            glm::round(float(stepHz) / glm::clamp(s_maxUpdateHz, 1.0f, 240.0f))));
        if (stepCount - m_lastClaimStep >= stepsPerClaim)
        {
            m_lastClaimStep = stepCount;
            sendClaims();
        }
    }
    m_host.update(0.0); // flush everything queued this frame (send() only queues until the next update)
}

void NetworkManager::sendSnapshotTick()
{
    uint8 buffer[1400];
    const size_t capacity = size_t(glm::clamp(s_snapshotMaxBytes, 128, 1400));
    NetWriter writer(oc::span<uint8>(buffer, capacity));
    size_t countOffset = 0;
    uint16 count = 0;

    const float maxVel = glm::max(1.0f, s_maxVel);
    const float maxAngVel = glm::max(1.0f, s_maxAngVel);
    const auto beginMessage = [&]
    {
        writer.reset();
        writer.write<uint8>(uint8(ENetMsg::Snapshot));
        writer.writeVarUInt(m_serverTick);
        writer.write<uint8>(s_quantize ? SnapshotFlag_Quantized : 0);
        if (s_quantize) // decode needs the (live-tweakable) velocity quantization ranges
        {
            writer.write<float>(maxVel);
            writer.write<float>(maxAngVel);
        }
        countOffset = writer.size();
        writer.write<uint16>(0); // patched by flushMessage
        count = 0;
    };
    const auto flushMessage = [&]
    {
        if (count == 0)
            return;
        writer.writeAt(countOffset, count);
        assert(!writer.overflowed());
        m_host.sendToAll(writer.data(), ENetDelivery::Unreliable, ChannelSnapshot);
    };

    const std::lock_guard<std::mutex> lock(m_entityMutex);
    if (m_entities.empty())
        return;
    beginMessage();

    const uint32 keyframeTicks = uint32(glm::max(1, s_keyframeEveryTicks));
    const float posEpsilonSq = s_sendPosEpsilon * s_sendPosEpsilon;
    auto it = m_entities.lower_bound(m_roundRobinCursor);
    if (it == m_entities.end())
        it = m_entities.begin();
    const size_t total = m_entities.size();
    int sent = 0;
    for (size_t visited = 0; visited < total && sent < s_maxEntitiesPerTick; ++visited)
    {
        const uint32 netId = it->first;
        Entity* entity = it->second.entity;
        NetworkComponent* comp = it->second.comp;
        ++it;
        if (it == m_entities.end())
            it = m_entities.begin();

        const bool keyframe = (m_serverTick % keyframeTicks) == (netId % keyframeTicks);
        const PhysicsComponent* physics = getComponent<PhysicsComponent>(entity);
        const bool physicsBody = physics && physics->bodyType == EPhysicsBodyType::Dynamic
            && physics->body.isValid() && !physics->suspended && physics->enabled;

        // owner must accept corrections while its claims are being rejected (non-owners ignore both bits)
        uint8 forcedFlag = (comp->ownerClientId != 0 && m_serverTick < comp->state->server.forcedUntilTick)
            ? NetRecFlag_Forced : uint8(0);
        if (comp->ownerClientId != 0 && m_serverTick < comp->state->server.arbitratedUntilTick)
            forcedFlag |= NetRecFlag_Arbitrated; // owner: soft-correct toward the solver-owned pose while steering

        // game state rides the same record; a blob change forces the record out even when the pose
        // alone would have been skipped (asleep body / unmoved entity)
        uint8 gameBlob[GameBlobBytes];
        const bool hasGame = packGameStateBlob(entity, gameBlob);
        const bool gameChanged = hasGame
            && std::memcmp(gameBlob, comp->state->server.lastSentGameBlob, GameBlobBytes) != 0;

        glm::vec3 pos;
        glm::quat rot;
        uint8 recFlags = forcedFlag | (hasGame ? NetRecFlag_Game : uint8(0));
        glm::vec3 linVel(0.0f);
        glm::vec3 angVel(0.0f);
        if (physicsBody)
        {
            // the BODY's world pose is authoritative (and what the client teleports its body to);
            // awake bodies send every tick, then ONE final record on the awake->asleep edge so the
            // client hard-syncs and sleeps too — after that only the keyframe rotation refreshes
            const bool asleep = !physics->body.isAwake();
            if (!asleep)
                comp->state->sleepDirty = true;
            else if (comp->state->sleepDirty)
                comp->state->sleepDirty = false;
            else if (!keyframe && !gameChanged)
                continue;
            recFlags |= NetRecFlag_Physics | (asleep ? NetRecFlag_Asleep : 0)
                | (comp->state->server.serverPrimary ? NetRecFlag_ServerPlayer : uint8(0));
            pos = physics->body.getPosition();
            rot = physics->body.getRotation();
            linVel = physics->body.getLinearVelocity();
            angVel = physics->body.getAngularVelocity();
            // CLAIM PASSTHROUGH: re-emit the owner's accepted claim rather than sampling the twin,
            // whose pose age wobbles ±1 tick as the owner/network/server clocks drift (remote
            // clients replay that wobble as speed pulsing). Ticks with no fresh claim extrapolate by
            // its velocity. Forced/arbitrated/asleep keep the twin — the authoritative state then.
            if (comp->ownerClientId != 0 && comp->state->server.claimStreamSeq != 0 && !asleep && forcedFlag == 0)
            {
                // snapshots may outpace claims, so the cap must cover that steady-state gap plus a
                // lost packet, or routine loss pops the stream back to the twin mid-gap
                const uint8 extrapCap = uint8(glm::clamp(s_snapshotHz / glm::max(1.0f, s_maxUpdateHz), 1.0f, 8.0f)) + 3;
                if (comp->state->server.claimStreamSentSeq != comp->state->server.claimStreamSeq)
                {
                    comp->state->server.claimStreamSentSeq = comp->state->server.claimStreamSeq;
                    comp->state->server.claimStreamExtrapTicks = 0;
                }
                else if (comp->state->server.claimStreamExtrapTicks < extrapCap)
                {
                    ++comp->state->server.claimStreamExtrapTicks;
                    const float dt = 1.0f / glm::clamp(s_snapshotHz, 1.0f, 240.0f);
                    comp->state->server.claimStreamPos += comp->state->server.claimStreamLinVel * dt; // chains across gap ticks
                    const float angSpeed = glm::length(comp->state->server.claimStreamAngVel);
                    if (angSpeed > 1e-4f)
                        comp->state->server.claimStreamRot = glm::normalize(
                            glm::angleAxis(angSpeed * dt, comp->state->server.claimStreamAngVel / angSpeed) * comp->state->server.claimStreamRot);
                }
                else
                    comp->state->server.claimStreamSeq = 0; // stream went stale: back to twin until claims resume
                if (comp->state->server.claimStreamSeq != 0)
                {
                    pos = comp->state->server.claimStreamPos;
                    rot = comp->state->server.claimStreamRot;
                    linVel = comp->state->server.claimStreamLinVel;
                    angVel = comp->state->server.claimStreamAngVel;
                    // at emit only, never accumulated into claimStream state
                    if (s_ownerPredictTicks > 0.0f)
                    {
                        const float lead = s_ownerPredictTicks / glm::clamp(s_snapshotHz, 1.0f, 240.0f);
                        pos += linVel * lead;
                        const float angSpeed = glm::length(angVel);
                        if (angSpeed > 1e-4f)
                            rot = glm::normalize(glm::angleAxis(angSpeed * lead, angVel / angSpeed) * rot);
                    }
                }
            }
        }
        else
        {
            // entity-LOCAL transform: moved-since-last-send + the keyframe rotation for convergence
            const glm::vec3 posDelta = entity->pos - comp->state->server.lastSentPos;
            const bool moved = glm::dot(posDelta, posDelta) > posEpsilonSq
                || quatAngleDeg(entity->rot, comp->state->server.lastSentRot) > s_sendRotEpsilonDeg;
            if (!keyframe && !moved && !gameChanged)
                continue;
            pos = entity->pos;
            rot = entity->rot;
            comp->state->server.lastSentPos = pos;
            comp->state->server.lastSentRot = rot;
        }

        constexpr size_t MaxRecordBytes = 5 + 1 + 12 + 16 + 24 + GameBlobBytes; // varint id + flags + pos + raw quat + raw velocities + game blob
        if (writer.size() + MaxRecordBytes > writer.capacity())
        {
            flushMessage();
            beginMessage();
        }
        writer.writeVarUInt(netId);
        writer.write<uint8>(recFlags);
        writer.write(pos);
        if (s_quantize)
            writer.write<uint32>(packQuat(rot));
        else
            writer.write(rot);
        if (recFlags & NetRecFlag_Physics)
        {
            if (s_quantize)
            {
                for (int c = 0; c < 3; ++c) writer.writeQuantized<uint16>(linVel[c], -maxVel, maxVel);
                for (int c = 0; c < 3; ++c) writer.writeQuantized<uint16>(angVel[c], -maxAngVel, maxAngVel);
            }
            else
            {
                writer.write(linVel);
                writer.write(angVel);
            }
        }
        if (recFlags & NetRecFlag_Game)
        {
            for (size_t c = 0; c < GameBlobBytes; ++c)
                writer.write<uint8>(gameBlob[c]);
            std::memcpy(comp->state->server.lastSentGameBlob, gameBlob, GameBlobBytes);
        }
        ++count;
        ++sent;
    }
    m_roundRobinCursor = it != m_entities.end() ? it->first : 0;
    flushMessage();
}

void NetworkManager::fireNetworkEvent(oc::string_view name, oc::span<const uint8> data, Entity* sender)
{
    // the same caps the receiver enforces — otherwise an event that is valid to send is rejected on
    // arrival, which is a far more confusing failure than refusing it here
    if (name.empty() || name.size() > MaxEventNameLength || data.size() > MaxEventDataBytes)
    {
        if (!name.empty())
            Log::warning("Network: event '" + oc::string(name.substr(0, MaxEventNameLength))
                + "' exceeds the name (" + oc::to_string(MaxEventNameLength) + "B) or payload ("
                + oc::to_string(MaxEventDataBytes) + "B) cap, dropped");
        return;
    }
    // locally originated: we are the sender (0 on a server, our clientId on a client)
    fireEventAttributed(name, m_localClientId, data); // same worker-safety contract as thunk_sendEvent
    if (m_role == ENetRole::None)
        return;
    uint32 senderNetId = 0;
    if (sender)
        if (const NetworkComponent* comp = getComponent<NetworkComponent>(sender))
            senderNetId = comp->netId; // assigned at spawn and never changes — safe to read off-thread
    // NetHost is single-threaded and this is reachable from worker-thread script thunks during the
    // parallel entity pass — queue, send() drains on the main thread the same frame
    const std::lock_guard<std::mutex> lock(m_eventMutex);
    m_pendingOutgoingEvents.push_back({ oc::string(name), { data.begin(), data.end() }, senderNetId });
}

// The CLIENT id is server->client only: a client's identity is implied by its connection, and an
// ignored client-writable identity field is a refactor away from being trusted. The NET id names the
// firing entity, which no connection implies, so it travels both ways — and is checked for ownership.
void NetworkManager::sendEventTo(NetPeerId peer, oc::string_view name, uint32 senderClientId,
    uint32 senderNetId, oc::span<const uint8> data)
{
    uint8 buffer[MaxEventMessageBytes];
    NetWriter writer(buffer);
    writer.write<uint8>(uint8(ENetMsg::Event));
    if (m_role == ENetRole::Server)
        writer.writeVarUInt(senderClientId); // who it originated from — the connection can't say
    writer.writeVarUInt(senderNetId);
    writer.writeString(name);
    writer.writeVarUInt(data.size());
    writer.writeBytes(data);
    if (writer.overflowed())
    {
        Log::warning("Network: event name too long, not sent: " + oc::string(name.substr(0, 64)));
        return;
    }
    m_host.send(peer, writer.data(), ENetDelivery::Reliable, ChannelEvent);
}

uint32 NetworkManager::registerEntity(Entity& entity, NetworkComponent* comp)
{
    uint32 netId = 0;
    if (m_role == ENetRole::Client && m_incomingSpawnCount != 0)
    {
        // executing a replicated Spawn: take the server's ids in tree spawn order (deterministic DFS)
        if (m_incomingSpawnCursor < m_incomingSpawnCount)
            netId = m_incomingSpawnBase + m_incomingSpawnCursor;
        ++m_incomingSpawnCursor; // counted past the limit too, so the mismatch warning has the real number
        if (netId == 0)
            return 0; // prefab has more NetworkComponents than the server announced: extras stay local-inert
        comp->ownerClientId = m_incomingSpawnOwner; // whole-tree ownership from the Spawn message
    }
    else if (m_role == ENetRole::Server)
    {
        // the server is the sole id authority — scene load and runtime spawns alike mint an id and
        // create/extend the ROOT's spawn record, which is how the entity reaches every client
        // (announced when ready peers exist, replayed to late joiners; registration is synchronous
        // DFS on the main thread, so a tree's ids stay contiguous from the base)
        const Entity* root = &entity;
        while (root->parent)
            root = root->parent;
        netId = m_nextNetId++;
        if (const auto rootIt = m_dynamicRootIds.find(root); rootIt != m_dynamicRootIds.end())
        {
            ++m_dynamicSpawns[rootIt->second].componentCount;
        }
        else
        {
            m_dynamicRootIds.emplace(root, netId);
            DynamicSpawn record;
            record.baseId = netId;
            record.componentCount = 1;
            if (root->spawnTemplate)
                record.path = !root->spawnTemplate->sourceFile.empty() ? root->spawnTemplate->sourceFile
                                                                       : root->spawnTemplate->prefabName;
            record.spawnPos = root->pos;
            record.spawnRot = root->rot;
            record.spawnScale = root->scale;
            if (record.path.empty())
                Log::warning("Network: networked entity '" + oc::string(root->getName())
                    + "' has no prefab source, clients cannot spawn it");
            else
                m_pendingSpawnAnnounce.push_back(netId);
            m_dynamicSpawns.emplace(netId, oc::move(record));
        }
    }
    else
        return 0; // client-local content or single player: LOCAL-INERT, no id, not in the registry

    const std::lock_guard<std::mutex> lock(m_entityMutex);
    const auto [it, inserted] = m_entities.insert_or_assign(netId, Replicated{ &entity, comp });
    if (!inserted)
        // the Entity Editor respawning this entity: the new twin spawns (and re-derives the same
        // name-path hash) while the old one is still registered — replace, pointer-checked unregister
        // keeps the replacement safe when the old entity dies
        Log::warning("Network: netId " + oc::to_string(netId) + " re-registered by entity '"
            + entity.getName() + "' (editor respawn?)");
    return netId;
}

void NetworkManager::unregisterEntity(uint32 netId, const NetworkComponent* comp)
{
    if (netId == 0)
        return; // local-inert component, was never registered
    {
        const std::lock_guard<std::mutex> lock(m_entityMutex);
        const auto it = m_entities.find(netId);
        if (it == m_entities.end() || it->second.comp != comp) // a replaced (stale) twin must not erase its successor
            return;
        m_entities.erase(it);
    }
    m_claimRings.erase(netId);    // owner-side redundancy ring, if this was a locally-owned entity
    m_remoteBuffers.erase(netId); // observer-side interpolation history (component dies with the entity, so no dangling reader)
    // a BASE id dying on the server despawns the whole replicated tree client-side (child ids of a
    // partial destruction are deliberately ignored — despawn is all-or-nothing at the root)
    if (m_role == ENetRole::Server)
        if (const auto it = m_dynamicSpawns.find(netId); it != m_dynamicSpawns.end())
        {
            m_dynamicSpawns.erase(it);
            if (oc::erase(m_pendingSpawnAnnounce, netId) == 0) // spawned + destroyed the same frame: announce nothing
                m_pendingDespawn.push_back(netId);
        }
}

oc::string NetworkManager::getStatusText() const
{
    if (m_role == ENetRole::None || !s_showStats)
        return {};
    const NetHostStats& stats = m_host.getStats();
    const auto kbs = [](uint32 bytesPerSec) { return oc::to_string((bytesPerSec * 10) / 1024 / 10) + "." + oc::to_string((bytesPerSec * 10 / 1024) % 10); };
    if (m_role == ENetRole::Server)
        return "SERVER " + oc::to_string(m_host.getConnectedCount()) + " peers | out " + kbs(stats.bytesSentPerSec)
            + " KB/s in " + kbs(stats.bytesReceivedPerSec) + " KB/s"
            // sustained nonzero = someone is flooding us (or the limits are set too tight for the
            // configured rates) — worth seeing without opening a panel
            + (stats.packetsDroppedPerSec != 0 ? " | DROPPED " + oc::to_string(stats.packetsDroppedPerSec) + "/s" : "");
    if (m_serverPeer == InvalidNetPeerId || !m_host.isConnected(m_serverPeer))
        return "CLIENT connecting...";
    return "CLIENT rtt " + oc::to_string(int(m_host.getPeerRttMs(m_serverPeer))) + " ms loss "
        + oc::to_string(int(m_host.getPeerPacketLoss(m_serverPeer) * 100.0f)) + "% | in " + kbs(stats.bytesReceivedPerSec)
        + " KB/s out " + kbs(stats.bytesSentPerSec) + " KB/s";
}
