export module Network:Reliable;

import Core;
import :Address;
import :Serialization;
import :Socket;
import :Crypto;

// Reliable-UDP game protocol over a single UdpSocket. A NetHost is symmetric: it accepts incoming
// connections (server), connects out (client), or both at once — two hosts that connect() to each
// other resolve the simultaneous handshake automatically (p2p, tie-broken on connect salts).
//
// Handshake (4-way, anti-spoofing: the responder stores no state and allocates nothing until the
// challenge round-trips through the initiator's claimed address):
//   Request [protocolId][flags][clientSalt][pad][pubkey?] ->
//   Challenge [clientSalt][serverSalt=HMAC(hostSecret, addr|salt|flags)] ->
//   Response [flags][clientSalt][serverSalt][pubkey?] ->  (responder verifies HMAC, allocates peer)
//   Accept [clientSalt][pubkey?]
// With NetHostConfig::encrypt both ends exchange ephemeral ECDH P-256 keys in the handshake and
// every payload packet is AES-128-GCM sealed (+16B tag): ack fields + messages encrypted, the
// 3-byte header authenticated as AAD, nonce = sender role + implicit 64-bit packet counter
// (reconstructed from the 16-bit wire seq). Unauthenticated key exchange: safe against spoofing,
// eavesdropping and tampering, not against an active MITM.
//
// Payload packet: [type u8][seq u16] + { [ack u16][ackBits u32] + messages } (sealed when encrypted):
//   Unreliable:          [kind u8][len varint][bytes]
//   UnreliableSequenced: [kind u8][seq u16][len varint][bytes]      stale ones dropped on arrival
//   Reliable:            [kind u8][seq u16][len varint][bytes]      ordered, resent until acked
//   Fragment:            [kind u8][seq u16][idx u16][cnt u16][len varint][bytes]
//   Disconnect:          [kind u8]
// The kind byte carries the channel index (bits 3-5): up to 8 independent reliable streams, each
// ordered within itself, so a bulk transfer on one channel never head-of-line-blocks another.
// Acks are packet-level (latest received seq + 32-packet history bitfield, piggybacked on every
// packet); reliable messages retransmit when every packet that carried them times out (~2x RTT).
// Empty payload packets double as acks/keepalives.

export enum class ENetDelivery : uint8
{
    Unreliable,          // fire and forget, must fit in one packet
    UnreliableSequenced, // like Unreliable, but out-of-date messages are dropped on arrival (per channel)
    Reliable,            // ordered + guaranteed within its channel, fragments transparently
};

export enum class ENetEventType : uint8
{
    Connected,
    Disconnected,
    Message,
};

export enum class ENetDisconnectReason : uint8
{
    None,
    Local,         // we called disconnect()
    Remote,        // the peer disconnected
    Timeout,       // nothing received for timeoutSec
    ConnectFailed, // handshake ran out of time
    Denied,        // remote host is full, refuses incoming, or the encrypt setting mismatches
    Overflow,      // peer stopped acking while we kept queueing reliable messages for it
};

export using NetPeerId = uint16;
export constexpr NetPeerId InvalidNetPeerId = 0xffff;
export constexpr uint32 NetMaxChannels = 8;

export struct NetEvent
{
    ENetEventType type = ENetEventType::Message;
    NetPeerId peer = InvalidNetPeerId;
    ENetDelivery delivery = ENetDelivery::Unreliable;         // Message events
    uint8 channel = 0;                                        // Message events
    ENetDisconnectReason reason = ENetDisconnectReason::None; // Disconnected events
    oc::vector<uint8> data;                                  // Message events
};

export struct NetHostConfig
{
    uint32 protocolId = 0x4F435331;    // "OCS1" — must match on both ends
    uint16 maxPeers = 32;              // total connection slots (incoming + outgoing)
    bool acceptIncoming = true;        // false = pure client (p2p simultaneous connect still works)
    bool encrypt = false;              // ECDH handshake + AES-128-GCM per packet (+16B); must match on both ends
    uint16 maxPacketSize = 1200;       // conservative MTU payload; fixed after open()
    uint32 maxMessageSize = 1024 * 1024; // reliable messages fragment up to this
    double connectTimeoutSec = 5.0;
    double timeoutSec = 5.0;
    double keepAliveSec = 0.1;         // empty ack packet cadence when idle
    double resendMinSec = 0.05;        // reliable retransmit clamp
    double resendMaxSec = 0.5;

    // ---- abuse limits (hostile senders; all enforced before any per-packet work) ----
    // Packet rate is what costs CPU, so it is budgeted per SOURCE ADDRESS in a fixed-size table
    // (a per-address map would itself be the memory attack). Generous vs real traffic: one peer at
    // 60 Hz snapshots + claims + acks runs well under 200/s.
    uint32 maxPacketsPerSecPerAddress = 400;
    uint32 packetBurstPerAddress = 200;   // bucket cap: absorbs legitimate bursts after a stall
    uint32 maxPacketsPerUpdate = 8192;    // global work ceiling per update() — a flood cannot make
                                          // one frame unbounded no matter how many addresses it uses
    uint16 maxPeersPerIp = 4;             // one machine must not be able to eat every connection slot
    uint32 maxQueuedReliablePerChannel = 1024; // a peer that stops acking while we keep queueing is
                                               // an unbounded memory sink — disconnect it instead

    // debug link simulation, applied to outgoing packets (live-editable)
    float simPacketLoss = 0.0f;        // 0..1 chance to drop
    float simLatencyMs = 0.0f;
    float simJitterMs = 0.0f;
};

export struct NetHostStats
{
    uint32 packetsSentPerSec = 0;
    uint32 packetsReceivedPerSec = 0;
    uint32 bytesSentPerSec = 0;
    uint32 bytesReceivedPerSec = 0;
    uint32 packetsDroppedPerSec = 0; // rate-limited away — sustained nonzero means a flood (or limits set too tight)
};

// protocol internals shared between interface and implementation. The framing sizes are exported so
// callers can derive their own message caps from them (see netMaxSinglePacketMessage).
export constexpr uint32 NetPacketHeaderSize = 1 + 2; // type + wire seq; ack fields live in the (sealed) body
export constexpr uint32 NetPacketAckSize = 2 + 4;
export constexpr uint32 NetReliableMsgHeaderSize = 6; // kind + seq + varint length
constexpr uint32 NetMaxRawPacketSize = 1500;
constexpr uint32 NetReliableWindow = 256;          // per channel: messages in flight / receive reorder window
constexpr uint32 NetSentPacketRing = 1024;

// Largest message that still rides in ONE packet. Bigger RELIABLE messages fragment transparently
// (correct, just more packets that must all arrive); bigger UNRELIABLE ones are DROPPED, as there is
// nothing to reassemble them with. Derive limits from this rather than hardcoding a margin, which
// silently rots the moment any header here grows.
export constexpr uint32 netMaxSinglePacketMessage(uint32 maxPacketSize, bool encrypted)
{
    return maxPacketSize - NetPacketHeaderSize - NetPacketAckSize
        - (encrypted ? NetCryptoTagSize : 0) - NetReliableMsgHeaderSize;
}

struct NetPeer
{
    enum class EState : uint8 { Free, Connecting, Connected };

    EState state = EState::Free;
    bool isServerRole = false; // accepted side of the handshake (drives the crypto nonce direction)
    NetAddress address;
    uint64 clientSalt = 0;
    uint64 serverSalt = 0;
    bool hasServerSalt = false; // client role: challenge received, resend Response instead of Request
    NetAeadKey key;

    double connectStartTime = 0.0;
    double lastHandshakeSendTime = -1.0;
    double lastReceiveTime = 0.0;
    double lastSendTime = 0.0;
    bool ackDirty = false; // received a payload packet since our last send -> ack promptly
    bool sendWindowOverflow = false; // queued reliable backlog blew past the cap: drop the peer in update()

    uint64 localSeq = 0;      // outgoing payload packet counter; low 16 bits go on the wire
    uint64 remoteSeq = 0;     // highest received packet counter (reconstructed)
    uint32 remoteSeqBits = 0; // received-history bitfield below remoteSeq
    bool hasRemoteSeq = false;

    float smoothedRttSec = 0.0f;
    bool hasRtt = false;
    float packetLoss = 0.0f; // EMA sampled as the sent ring recycles

    struct SentPacket
    {
        uint64 seq = 0;
        bool used = false;
        bool acked = false;
        double sendTime = 0.0;
        oc::vector<uint32> reliableIds; // (channel << 16) | messageSeq carried by this packet
    };
    oc::vector<SentPacket> sentPackets; // ring indexed seq % NetSentPacketRing

    struct ReliableMsg
    {
        uint16 seq = 0;
        bool acked = false;
        double lastSendTime = -1.0;
        oc::vector<uint8> encoded; // complete message blob, dropped into packets as-is
    };
    struct SendChannel
    {
        uint16 nextReliableSeq = 0;
        uint16 nextUnreliableSeq = 0;
        oc::deque<ReliableMsg> window; // front = oldest unacked; first NetReliableWindow are eligible
    };
    oc::vector<SendChannel> sendChannels; // grown on first use per channel

    oc::vector<oc::vector<uint8>> unreliableQueue; // encoded (channel in the kind byte), flushed every update

    struct RecvSlot
    {
        bool valid = false;
        uint8 kind = 0;
        uint16 fragIndex = 0;
        uint16 fragCount = 0;
        oc::vector<uint8> data;
    };
    struct RecvChannel
    {
        uint16 expectedSeq = 0;
        uint16 lastUnreliableSeq = 0;
        bool hasUnreliableSeq = false;
        uint16 fragmentsReceived = 0;
        oc::vector<RecvSlot> slots; // ring indexed seq % NetReliableWindow
        oc::vector<uint8> fragmentAssembly;
    };
    oc::vector<RecvChannel> recvChannels; // grown on first use per channel
};

export class NetHost final
{
public:

    NetHost() = default;
    NetHost(const NetHost&) = delete;
    NetHost& operator=(const NetHost&) = delete;
    ~NetHost() { close(); }

    bool open(uint16 port = 0, const NetHostConfig& config = {}); // port 0 = ephemeral (client/p2p)
    void close();                                                 // disconnects all peers
    bool isOpen() const { return m_socket.isOpen(); }
    uint16 getLocalPort() const { return m_socket.getLocalPort(); }

    NetPeerId connect(const NetAddress& address); // returns the existing peer if already known
    void disconnect(NetPeerId peer);

    bool send(NetPeerId peer, oc::span<const uint8> data, ENetDelivery delivery, uint8 channel = 0);
    void sendToAll(oc::span<const uint8> data, ENetDelivery delivery, uint8 channel = 0);

    // pump the socket, run handshakes/timeouts/retransmits, flush queued messages as packets
    void update(double deltaSec);
    oc::vector<NetEvent> takeEvents();

    bool isConnected(NetPeerId peer) const;
    uint32 getConnectedCount() const;
    NetAddress getPeerAddress(NetPeerId peer) const;
    NetPeerId findPeer(const NetAddress& address) const;
    float getPeerRttMs(NetPeerId peer) const;
    float getPeerPacketLoss(NetPeerId peer) const; // 0..1 over the recent send window

    const NetHostStats& getStats() const { return m_stats; }
    // timeouts and sim* fields may be edited live; protocol/packet sizing/encrypt must not change after open()
    NetHostConfig& config() { return m_config; }

private:

    bool rateLimitAllows(const NetAddress& from); // per-source token bucket, before any parsing
    uint32 countPeersOnIp(uint32 ip) const;
    void handlePacket(const NetAddress& from, oc::span<const uint8> bytes);
    void handleConnectRequest(const NetAddress& from, NetPeerId id, NetReader& reader);
    void handleChallengeResponse(const NetAddress& from, NetPeerId id, NetReader& reader);
    void handlePayload(NetPeer& peer, NetPeerId id, oc::span<const uint8> bytes);
    void deliverReliable(NetPeer& peer, NetPeerId id, uint8 channel);
    void processAcks(NetPeer& peer, uint16 ackWire, uint32 ackBits);
    void flushPeer(NetPeer& peer);
    void transmitPayload(NetPeer& peer, oc::span<const uint8> body, bool bypassSim); // header + seal + send, advances localSeq
    void sendDisconnectPackets(NetPeer& peer, bool bypassSim);
    void sendConnectRequest(NetPeer& peer);
    void sendChallengeResponse(NetPeer& peer);
    void sendAccept(NetPeer& peer);
    void sendDeny(const NetAddress& to, uint64 clientSalt);
    void sendRaw(const NetAddress& to, oc::span<const uint8> bytes, bool bypassSim = false);
    uint64 challengeSaltFor(const NetAddress& address, uint64 clientSalt, uint8 flags) const;
    bool derivePeerKey(NetPeer& peer, oc::span<const uint8> peerPublicKey);
    void emitConnected(NetPeerId id);
    void emitDisconnected(NetPeerId id, ENetDisconnectReason reason);
    NetPeerId allocPeer(const NetAddress& address);
    void freePeer(NetPeerId id);
    float rand01();

    UdpSocket m_socket;
    NetHostConfig m_config;
    double m_time = 0.0;
    oc::vector<NetPeer> m_peers;
    oc::unordered_map<uint64, NetPeerId> m_peerByAddress;
    oc::vector<NetEvent> m_events;
    oc::vector<uint32> m_scratchReliableIds;

    uint8 m_hostSecret[32] = {}; // keys the stateless challenge HMAC
    NetKeyPair m_keyPair;        // ephemeral ECDH keypair when config.encrypt

    struct DelayedSend
    {
        double releaseTime = 0.0;
        NetAddress to;
        oc::vector<uint8> data;
    };
    oc::vector<DelayedSend> m_delayedSends; // link simulation
    uint64 m_rngState = 0;

    // Fixed-size rate-limit table: FIXED because the thing being defended against is an attacker
    // who varies its source address — a per-address map would grow with every forged sender. Two
    // addresses colliding on a slot merely share a (generous) budget.
    static constexpr uint32 RateBucketCount = 512;
    struct RateBucket
    {
        uint64 addressKey = 0;
        float tokens = 0.0f;
        double lastRefill = 0.0;
    };
    RateBucket m_rateBuckets[RateBucketCount];
    uint32 m_packetsThisUpdate = 0;
    uint32 m_accumPacketsDropped = 0;

    NetHostStats m_stats;
    double m_statsWindowStart = 0.0;
    uint64 m_accumBytesSent = 0;
    uint64 m_accumBytesReceived = 0;
    uint32 m_accumPacketsSent = 0;
    uint32 m_accumPacketsReceived = 0;
};
