module Network;

import Core;
import Core.Log;
import :Address;
import :Socket;
import :Serialization;
import :Crypto;
import :Reliable;

enum : uint8
{
    PktConnectRequest = 1,
    PktChallenge = 2,
    PktChallengeResponse = 3,
    PktConnectAccept = 4,
    PktConnectDeny = 5,
    PktPayload = 6,
};
constexpr uint8 PktTypeMask = 0x7f;
constexpr uint8 PktFlagHasAck = 0x80; // the ack fields in the body are meaningful

enum : uint8
{
    MsgUnreliable = 0,
    MsgUnreliableSeq = 1,
    MsgReliable = 2,
    MsgFragment = 3,
    MsgDisconnect = 4,
};
constexpr uint8 MsgKindMask = 0x07;
constexpr uint32 MsgChannelShift = 3; // channel index in kind byte bits 3-5

constexpr uint8 HandshakeFlagEncrypt = 0x01;
constexpr uint32 HandshakeRequestPad = 16; // keeps Request >= Challenge (no amplification)
constexpr double HandshakeResendSec = 0.25;

// serial-number arithmetic with wraparound (message-level u16 seqs)
static bool seqGreater(uint16 a, uint16 b) { return int16(uint16(a - b)) > 0; }

// recover the full 64-bit packet counter closest to reference from its low 16 wire bits
static uint64 reconstructSeq(uint16 wire, uint64 reference)
{
    uint64 candidate = (reference & ~uint64(0xffff)) | wire;
    if (candidate + 0x8000 < reference)
        candidate += 0x10000;
    else if (candidate > reference + 0x8000 && candidate >= 0x10000)
        candidate -= 0x10000;
    return candidate;
}

bool NetHost::open(uint16 port, const NetHostConfig& config)
{
    close();
    m_config = config;
    if (m_config.maxPacketSize < 128)
        m_config.maxPacketSize = 128;
    if (m_config.maxPacketSize > 1400)
        m_config.maxPacketSize = 1400;
    if (!cryptoRandom(m_hostSecret))
    {
        Log::error("NetHost: system RNG unavailable");
        return false;
    }
    if (m_config.encrypt && !cryptoGenerateKeyPair(m_keyPair))
        return false;
    if (!m_socket.open(port))
        return false;
    m_time = 0.0;
    m_statsWindowStart = 0.0;
    m_stats = {};
    m_accumBytesSent = m_accumBytesReceived = 0;
    m_accumPacketsSent = m_accumPacketsReceived = m_accumPacketsDropped = 0;
    for (RateBucket& bucket : m_rateBuckets)
        bucket = {};
    uint8 seed[8];
    cryptoRandom(seed);
    memcpy(&m_rngState, seed, sizeof(m_rngState));
    return true;
}

void NetHost::close()
{
    if (!isOpen())
        return;
    for (NetPeer& peer : m_peers)
        if (peer.state == NetPeer::EState::Connected)
            sendDisconnectPackets(peer, true);
    m_socket.close();
    m_keyPair.destroy();
    m_peers.clear();
    m_peerByAddress.clear();
    m_events.clear();
    m_delayedSends.clear();
}

NetPeerId NetHost::connect(const NetAddress& address)
{
    if (!isOpen() || !address.isValid())
        return InvalidNetPeerId;
    if (const auto it = m_peerByAddress.find(address.key()); it != m_peerByAddress.end())
        return it->second;
    const NetPeerId id = allocPeer(address);
    if (id == InvalidNetPeerId)
        return InvalidNetPeerId;
    NetPeer& peer = m_peers[id];
    peer.state = NetPeer::EState::Connecting;
    cryptoRandom(oc::span(reinterpret_cast<uint8*>(&peer.clientSalt), sizeof(peer.clientSalt)));
    sendConnectRequest(peer);
    peer.lastHandshakeSendTime = m_time;
    return id;
}

void NetHost::disconnect(NetPeerId id)
{
    if (id >= m_peers.size() || m_peers[id].state == NetPeer::EState::Free)
        return;
    NetPeer& peer = m_peers[id];
    if (peer.state == NetPeer::EState::Connected)
        sendDisconnectPackets(peer, false);
    emitDisconnected(id, ENetDisconnectReason::Local);
    freePeer(id);
}

bool NetHost::send(NetPeerId id, oc::span<const uint8> data, ENetDelivery delivery, uint8 channel)
{
    if (id >= m_peers.size() || m_peers[id].state != NetPeer::EState::Connected || channel >= NetMaxChannels)
        return false;
    NetPeer& peer = m_peers[id];
    if (peer.sendChannels.size() <= channel)
        peer.sendChannels.resize(channel + 1);
    NetPeer::SendChannel& sendChannel = peer.sendChannels[channel];

    // max bytes one message blob may occupy inside a packet body (after header/ack/GCM tag)
    const uint32 bodyBudget = m_config.maxPacketSize - NetPacketHeaderSize - NetPacketAckSize
        - (m_config.encrypt ? NetCryptoTagSize : 0);
    uint8 scratch[NetMaxRawPacketSize];

    if (delivery == ENetDelivery::Unreliable || delivery == ENetDelivery::UnreliableSequenced)
    {
        NetWriter writer(scratch);
        if (delivery == ENetDelivery::Unreliable)
        {
            writer.write<uint8>(uint8(MsgUnreliable | (channel << MsgChannelShift)));
        }
        else
        {
            writer.write<uint8>(uint8(MsgUnreliableSeq | (channel << MsgChannelShift)));
            writer.write<uint16>(sendChannel.nextUnreliableSeq++);
        }
        writer.writeVarUInt(data.size());
        writer.writeBytes(data);
        if (writer.overflowed() || writer.size() > bodyBudget)
        {
            Log::warning("NetHost: unreliable message too large (" + oc::to_string(data.size()) + " bytes), dropped");
            return false;
        }
        peer.unreliableQueue.emplace_back(writer.data().begin(), writer.data().end());
        return true;
    }

    // a peer that stays connected but stops ACKING would grow this window forever
    if (sendChannel.window.size() >= m_config.maxQueuedReliablePerChannel)
    {
        if (!peer.sendWindowOverflow)
        {
            peer.sendWindowOverflow = true; // acted on in update(): freeing a peer here would break
                                            // sendToAll's loop
            Log::warning("NetHost: peer " + peer.address.toString() + " reliable backlog over "
                + oc::to_string(m_config.maxQueuedReliablePerChannel) + " on channel "
                + oc::to_string(channel) + ", disconnecting");
        }
        return false;
    }

    // Reliable: single message when it fits, otherwise a run of fragments on consecutive seqs
    const auto pushReliable = [&](uint8 kind, uint16 fragIndex, uint16 fragCount, oc::span<const uint8> chunk)
    {
        NetPeer::ReliableMsg msg;
        msg.seq = sendChannel.nextReliableSeq++;
        NetWriter writer(scratch);
        writer.write<uint8>(uint8(kind | (channel << MsgChannelShift)));
        writer.write<uint16>(msg.seq);
        if (kind == MsgFragment)
        {
            writer.write<uint16>(fragIndex);
            writer.write<uint16>(fragCount);
        }
        writer.writeVarUInt(chunk.size());
        writer.writeBytes(chunk);
        assert(!writer.overflowed() && writer.size() <= bodyBudget);
        msg.encoded.assign(writer.data().begin(), writer.data().end());
        sendChannel.window.push_back(oc::move(msg));
    };

    const uint32 singleMax = bodyBudget - 6;   // kind + seq + varint len
    if (data.size() <= singleMax)
    {
        pushReliable(MsgReliable, 0, 0, data);
        return true;
    }
    const uint32 fragmentMax = bodyBudget - 10; // kind + seq + idx + cnt + varint len
    const uint64 fragmentCount = (data.size() + fragmentMax - 1) / fragmentMax;
    if (data.size() > m_config.maxMessageSize || fragmentCount > 0xffff)
    {
        Log::warning("NetHost: reliable message too large (" + oc::to_string(data.size()) + " bytes), dropped");
        return false;
    }
    for (uint64 i = 0; i < fragmentCount; ++i)
    {
        const uint64 offset = i * fragmentMax;
        const uint64 size = data.size() - offset < fragmentMax ? data.size() - offset : fragmentMax;
        pushReliable(MsgFragment, uint16(i), uint16(fragmentCount), data.subspan(offset, size));
    }
    return true;
}

void NetHost::sendToAll(oc::span<const uint8> data, ENetDelivery delivery, uint8 channel)
{
    for (NetPeerId id = 0; id < m_peers.size(); ++id)
        if (m_peers[id].state == NetPeer::EState::Connected)
            send(id, data, delivery, channel);
}

void NetHost::update(double deltaSec)
{
    if (!isOpen())
        return;
    m_time += deltaSec;

    uint8 buffer[NetMaxRawPacketSize];
    NetAddress from;
    m_packetsThisUpdate = 0;
    while (true)
    {
        const int size = m_socket.receiveFrom(buffer, from);
        if (size < 0)
            break;
        ++m_accumPacketsReceived;
        m_accumBytesReceived += size;
        // global ceiling: per-address limits alone still let one update() run unbounded, since the
        // socket keeps yielding. Excess stays in the socket buffer for the OS to drop.
        if (++m_packetsThisUpdate > m_config.maxPacketsPerUpdate)
        {
            ++m_accumPacketsDropped;
            continue;
        }
        if (size > 0 && rateLimitAllows(from))
            handlePacket(from, oc::span<const uint8>(buffer, size));
        else if (size > 0)
            ++m_accumPacketsDropped;
    }

    for (NetPeerId id = 0; id < m_peers.size(); ++id)
    {
        NetPeer& peer = m_peers[id];
        if (peer.state == NetPeer::EState::Connecting)
        {
            if (m_time - peer.connectStartTime > m_config.connectTimeoutSec)
            {
                emitDisconnected(id, ENetDisconnectReason::ConnectFailed);
                freePeer(id);
            }
            else if (!peer.isServerRole && m_time - peer.lastHandshakeSendTime >= HandshakeResendSec)
            {
                if (peer.hasServerSalt)
                    sendChallengeResponse(peer);
                else
                    sendConnectRequest(peer);
                peer.lastHandshakeSendTime = m_time;
            }
        }
        else if (peer.state == NetPeer::EState::Connected)
        {
            if (peer.sendWindowOverflow)
            {
                sendDisconnectPackets(peer, false);
                emitDisconnected(id, ENetDisconnectReason::Overflow);
                freePeer(id);
            }
            else if (m_time - peer.lastReceiveTime > m_config.timeoutSec)
            {
                sendDisconnectPackets(peer, false);
                emitDisconnected(id, ENetDisconnectReason::Timeout);
                freePeer(id);
            }
            else
            {
                flushPeer(peer);
            }
        }
    }

    for (size_t i = 0; i < m_delayedSends.size();)
    {
        if (m_time >= m_delayedSends[i].releaseTime)
        {
            m_socket.sendTo(m_delayedSends[i].to, m_delayedSends[i].data);
            if (i != m_delayedSends.size() - 1)
                m_delayedSends[i] = oc::move(m_delayedSends.back());
            m_delayedSends.pop_back();
        }
        else
        {
            ++i;
        }
    }

    if (m_time - m_statsWindowStart >= 1.0)
    {
        const double window = m_time - m_statsWindowStart;
        m_stats.packetsSentPerSec = uint32(m_accumPacketsSent / window);
        m_stats.packetsReceivedPerSec = uint32(m_accumPacketsReceived / window);
        m_stats.bytesSentPerSec = uint32(m_accumBytesSent / window);
        m_stats.bytesReceivedPerSec = uint32(m_accumBytesReceived / window);
        m_stats.packetsDroppedPerSec = uint32(m_accumPacketsDropped / window);
        m_accumPacketsSent = m_accumPacketsReceived = m_accumPacketsDropped = 0;
        m_accumBytesSent = m_accumBytesReceived = 0;
        m_statsWindowStart = m_time;
    }
}

oc::vector<NetEvent> NetHost::takeEvents()
{
    oc::vector<NetEvent> out = oc::move(m_events);
    m_events.clear();
    return out;
}

bool NetHost::isConnected(NetPeerId id) const
{
    return id < m_peers.size() && m_peers[id].state == NetPeer::EState::Connected;
}

uint32 NetHost::getConnectedCount() const
{
    uint32 count = 0;
    for (const NetPeer& peer : m_peers)
        count += peer.state == NetPeer::EState::Connected;
    return count;
}

NetAddress NetHost::getPeerAddress(NetPeerId id) const
{
    return id < m_peers.size() && m_peers[id].state != NetPeer::EState::Free ? m_peers[id].address : NetAddress{};
}

NetPeerId NetHost::findPeer(const NetAddress& address) const
{
    const auto it = m_peerByAddress.find(address.key());
    return it != m_peerByAddress.end() ? it->second : InvalidNetPeerId;
}

float NetHost::getPeerRttMs(NetPeerId id) const
{
    return id < m_peers.size() && m_peers[id].hasRtt ? m_peers[id].smoothedRttSec * 1000.0f : 0.0f;
}

float NetHost::getPeerPacketLoss(NetPeerId id) const
{
    return id < m_peers.size() ? m_peers[id].packetLoss : 0.0f;
}

// ---- handshake ----------------------------------------------------------------------------------

uint64 NetHost::challengeSaltFor(const NetAddress& address, uint64 clientSalt, uint8 flags) const
{
    uint8 input[15];
    NetWriter writer(input);
    writer.write<uint32>(address.ip);
    writer.write<uint16>(address.port);
    writer.write<uint64>(clientSalt);
    writer.write<uint8>(flags);
    return cryptoStatelessMac(m_hostSecret, writer.data());
}

bool NetHost::derivePeerKey(NetPeer& peer, oc::span<const uint8> peerPublicKey)
{
    uint8 context[20];
    NetWriter writer(context);
    writer.write<uint32>(m_config.protocolId);
    writer.write<uint64>(peer.clientSalt);
    writer.write<uint64>(peer.serverSalt);
    return cryptoDeriveAead(m_keyPair, peerPublicKey, writer.data(), peer.key);
}

void NetHost::sendConnectRequest(NetPeer& peer)
{
    uint8 buffer[NetPacketHeaderSize + 128];
    NetWriter writer(buffer);
    writer.write<uint8>(PktConnectRequest);
    writer.write<uint32>(m_config.protocolId);
    writer.write<uint8>(m_config.encrypt ? HandshakeFlagEncrypt : 0);
    writer.write<uint64>(peer.clientSalt);
    const uint8 pad[HandshakeRequestPad] = {};
    writer.writeBytes(pad);
    if (m_config.encrypt)
        writer.writeBytes(m_keyPair.publicKey);
    sendRaw(peer.address, writer.data());
}

void NetHost::sendChallengeResponse(NetPeer& peer)
{
    uint8 buffer[128];
    NetWriter writer(buffer);
    writer.write<uint8>(PktChallengeResponse);
    writer.write<uint32>(m_config.protocolId);
    writer.write<uint8>(m_config.encrypt ? HandshakeFlagEncrypt : 0);
    writer.write<uint64>(peer.clientSalt);
    writer.write<uint64>(peer.serverSalt);
    if (m_config.encrypt)
        writer.writeBytes(m_keyPair.publicKey);
    sendRaw(peer.address, writer.data());
}

void NetHost::sendAccept(NetPeer& peer)
{
    uint8 buffer[128];
    NetWriter writer(buffer);
    writer.write<uint8>(PktConnectAccept);
    writer.write<uint32>(m_config.protocolId);
    writer.write<uint64>(peer.clientSalt);
    if (m_config.encrypt)
        writer.writeBytes(m_keyPair.publicKey);
    sendRaw(peer.address, writer.data());
}

void NetHost::sendDeny(const NetAddress& to, uint64 clientSalt)
{
    uint8 buffer[16];
    NetWriter writer(buffer);
    writer.write<uint8>(PktConnectDeny);
    writer.write<uint32>(m_config.protocolId);
    writer.write<uint64>(clientSalt);
    sendRaw(to, writer.data());
}

void NetHost::handleConnectRequest(const NetAddress& from, NetPeerId id, NetReader& reader)
{
    const uint8 flags = reader.read<uint8>();
    const uint64 clientSalt = reader.read<uint64>();
    reader.skip(HandshakeRequestPad);
    if (reader.overflowed())
        return;
    if (flags != (m_config.encrypt ? HandshakeFlagEncrypt : 0))
    {
        sendDeny(from, clientSalt);
        return;
    }
    if (id != InvalidNetPeerId)
    {
        NetPeer& peer = m_peers[id];
        if (peer.state == NetPeer::EState::Connecting && !peer.isServerRole)
        {
            if (peer.clientSalt > clientSalt)
                return; // simultaneous connect: we win as client, they will serve our request
            peer.isServerRole = true; // we lose: serve theirs on this slot, stop our own resends
        }
        // Connected: late duplicate request (or a restart) — the stateless challenge is harmless
    }
    else if (!m_config.acceptIncoming)
    {
        sendDeny(from, clientSalt);
        return;
    }
    uint8 buffer[32]; // stateless reply: nothing allocated until the challenge round-trips
    NetWriter writer(buffer);
    writer.write<uint8>(PktChallenge);
    writer.write<uint32>(m_config.protocolId);
    writer.write<uint64>(clientSalt);
    writer.write<uint64>(challengeSaltFor(from, clientSalt, flags));
    sendRaw(from, writer.data());
}

void NetHost::handleChallengeResponse(const NetAddress& from, NetPeerId id, NetReader& reader)
{
    const uint8 flags = reader.read<uint8>();
    const uint64 clientSalt = reader.read<uint64>();
    const uint64 serverSalt = reader.read<uint64>();
    uint8 peerPublicKey[NetCryptoPublicKeySize];
    if (m_config.encrypt)
    {
        const oc::span<const uint8> key = reader.readBytes(NetCryptoPublicKeySize);
        if (!reader.overflowed())
            memcpy(peerPublicKey, key.data(), NetCryptoPublicKeySize);
    }
    if (reader.overflowed())
        return;
    if (flags != (m_config.encrypt ? HandshakeFlagEncrypt : 0))
        return;
    if (serverSalt != challengeSaltFor(from, clientSalt, flags))
        return; // not our challenge (spoofed source address or stale)

    if (id != InvalidNetPeerId && m_peers[id].state == NetPeer::EState::Connected)
    {
        NetPeer& peer = m_peers[id];
        if (peer.clientSalt == clientSalt && peer.serverSalt == serverSalt)
        {
            sendAccept(peer); // duplicate response, our accept was lost
            return;
        }
        // same address, fresh salts: the remote end restarted — replace the stale connection
        emitDisconnected(id, ENetDisconnectReason::Remote);
        freePeer(id);
        id = InvalidNetPeerId;
    }
    if (id == InvalidNetPeerId)
    {
        // the challenge proves reachability, not scarcity: one host can handshake from many ports
        if (m_config.maxPeersPerIp != 0 && countPeersOnIp(from.ip) >= m_config.maxPeersPerIp)
        {
            sendDeny(from, clientSalt);
            return;
        }
        id = allocPeer(from);
        if (id == InvalidNetPeerId)
        {
            sendDeny(from, clientSalt);
            return;
        }
    }
    NetPeer& peer = m_peers[id];
    peer.isServerRole = true;
    peer.clientSalt = clientSalt;
    peer.serverSalt = serverSalt;
    if (m_config.encrypt && !derivePeerKey(peer, peerPublicKey))
    {
        freePeer(id);
        sendDeny(from, clientSalt);
        return;
    }
    peer.state = NetPeer::EState::Connected;
    peer.lastReceiveTime = m_time;
    sendAccept(peer);
    emitConnected(id);
}

// Per-source token bucket, before any parsing/decryption/allocation. Unconnected senders included:
// handshake requests are the cheapest thing to flood. Fixed table — a per-address map would grow
// with every forged sender.
bool NetHost::rateLimitAllows(const NetAddress& from)
{
    const uint64 key = from.key();
    // finalizer: raw address bits cluster by subnet/port and would pile a range into one bucket
    uint64 hash = key * 0x9e3779b97f4a7c15ull;
    hash ^= hash >> 29;
    hash *= 0xbf58476d1ce4e5b9ull;
    hash ^= hash >> 32;
    RateBucket& bucket = m_rateBuckets[hash % RateBucketCount];

    const float burst = float(oc::max(1u, m_config.packetBurstPerAddress));
    if (bucket.addressKey != key)
    {
        // unused slots claim immediately (an empty bucket never "refills", so testing it would
        // reject every new address); occupied ones only yield once their owner has gone quiet long
        // enough to have refilled, or a flooder could evict an active peer and reset its own budget
        if (bucket.addressKey != 0)
        {
            const float refilled = float(m_time - bucket.lastRefill) * float(m_config.maxPacketsPerSecPerAddress);
            if (bucket.tokens + refilled < burst)
                return false;
        }
        bucket.addressKey = key;
        bucket.tokens = burst;
        bucket.lastRefill = m_time;
    }
    else
    {
        bucket.tokens = oc::min(burst,
            bucket.tokens + float(m_time - bucket.lastRefill) * float(m_config.maxPacketsPerSecPerAddress));
        bucket.lastRefill = m_time;
    }
    if (bucket.tokens < 1.0f)
        return false;
    bucket.tokens -= 1.0f;
    return true;
}

uint32 NetHost::countPeersOnIp(uint32 ip) const
{
    uint32 count = 0;
    for (const NetPeer& peer : m_peers)
        if (peer.state != NetPeer::EState::Free && peer.address.ip == ip)
            ++count;
    return count;
}

void NetHost::handlePacket(const NetAddress& from, oc::span<const uint8> bytes)
{
    NetReader reader(bytes);
    const uint8 type = reader.read<uint8>() & PktTypeMask;
    const auto it = m_peerByAddress.find(from.key());
    const NetPeerId id = it != m_peerByAddress.end() ? it->second : InvalidNetPeerId;

    if (type == PktPayload)
    {
        if (id != InvalidNetPeerId && m_peers[id].state == NetPeer::EState::Connected)
            handlePayload(m_peers[id], id, bytes);
        return;
    }

    if (reader.read<uint32>() != m_config.protocolId || reader.overflowed())
        return;

    switch (type)
    {
    case PktConnectRequest:
        handleConnectRequest(from, id, reader);
        return;
    case PktChallenge:
    {
        const uint64 clientSalt = reader.read<uint64>();
        const uint64 serverSalt = reader.read<uint64>();
        if (reader.overflowed() || id == InvalidNetPeerId)
            return;
        NetPeer& peer = m_peers[id];
        if (peer.state != NetPeer::EState::Connecting || peer.isServerRole || peer.clientSalt != clientSalt)
            return;
        peer.serverSalt = serverSalt;
        peer.hasServerSalt = true;
        sendChallengeResponse(peer);
        peer.lastHandshakeSendTime = m_time;
        return;
    }
    case PktChallengeResponse:
        handleChallengeResponse(from, id, reader);
        return;
    case PktConnectAccept:
    {
        const uint64 clientSalt = reader.read<uint64>();
        if (reader.overflowed() || id == InvalidNetPeerId)
            return;
        NetPeer& peer = m_peers[id];
        if (peer.state != NetPeer::EState::Connecting || peer.isServerRole || !peer.hasServerSalt || peer.clientSalt != clientSalt)
            return;
        if (m_config.encrypt)
        {
            const oc::span<const uint8> key = reader.readBytes(NetCryptoPublicKeySize);
            if (reader.overflowed() || !derivePeerKey(peer, key))
                return;
        }
        peer.state = NetPeer::EState::Connected;
        peer.lastReceiveTime = m_time;
        emitConnected(id);
        return;
    }
    case PktConnectDeny:
    {
        const uint64 clientSalt = reader.read<uint64>();
        if (reader.overflowed() || id == InvalidNetPeerId)
            return;
        if (m_peers[id].state != NetPeer::EState::Connecting || m_peers[id].clientSalt != clientSalt)
            return;
        emitDisconnected(id, ENetDisconnectReason::Denied);
        freePeer(id);
        return;
    }
    default:
        return;
    }
}

// ---- payload ------------------------------------------------------------------------------------

void NetHost::handlePayload(NetPeer& peer, NetPeerId id, oc::span<const uint8> bytes)
{
    if (bytes.size() < NetPacketHeaderSize + NetPacketAckSize + (m_config.encrypt ? NetCryptoTagSize : 0))
        return;
    const uint8 rawType = bytes[0];
    uint16 wireSeq;
    memcpy(&wireSeq, bytes.data() + 1, sizeof(wireSeq));
    const uint64 seq = reconstructSeq(wireSeq, peer.hasRemoteSeq ? peer.remoteSeq : wireSeq);

    uint8 plain[NetMaxRawPacketSize];
    oc::span<const uint8> body;
    if (m_config.encrypt)
    {
        uint8 nonce[12] = {};
        nonce[0] = peer.isServerRole ? 0 : 1; // sender's role
        memcpy(nonce + 4, &seq, sizeof(seq));
        const size_t plainSize = bytes.size() - NetPacketHeaderSize - NetCryptoTagSize;
        if (!cryptoOpen(peer.key, nonce, bytes.first(NetPacketHeaderSize), bytes.subspan(NetPacketHeaderSize), oc::span(plain, plainSize)))
            return; // forged, corrupt, or replayed under the wrong counter
        body = oc::span<const uint8>(plain, plainSize);
    }
    else
    {
        body = bytes.subspan(NetPacketHeaderSize);
    }

    NetReader reader(body);
    const uint16 ackWire = reader.read<uint16>();
    const uint32 ackBits = reader.read<uint32>();
    peer.lastReceiveTime = m_time;
    if (rawType & PktFlagHasAck)
        processAcks(peer, ackWire, ackBits);

    // packet-level dedup + ack bookkeeping
    bool isNew;
    if (!peer.hasRemoteSeq)
    {
        peer.hasRemoteSeq = true;
        peer.remoteSeq = seq;
        peer.remoteSeqBits = 0;
        isNew = true;
    }
    else if (seq > peer.remoteSeq)
    {
        const uint64 shift = seq - peer.remoteSeq;
        peer.remoteSeqBits = shift >= 32 ? 0 : (peer.remoteSeqBits << shift);
        if (shift <= 32)
            peer.remoteSeqBits |= 1u << (shift - 1);
        peer.remoteSeq = seq;
        isNew = true;
    }
    else
    {
        const uint64 behind = peer.remoteSeq - seq;
        if (behind == 0 || behind > 32)
        {
            isNew = false; // duplicate of the latest, or too old to track
        }
        else
        {
            const uint32 bit = 1u << (behind - 1);
            isNew = (peer.remoteSeqBits & bit) == 0;
            peer.remoteSeqBits |= bit;
        }
    }
    peer.ackDirty = true;
    if (!isNew)
        return;

    while (!reader.atEnd() && !reader.overflowed())
    {
        const uint8 kindByte = reader.read<uint8>();
        const uint8 kind = kindByte & MsgKindMask;
        const uint8 channel = (kindByte >> MsgChannelShift) & (NetMaxChannels - 1);
        switch (kind)
        {
        case MsgUnreliable:
        {
            const oc::span<const uint8> data = reader.readBytes(size_t(reader.readVarUInt()));
            if (reader.overflowed())
                return;
            NetEvent& event = m_events.emplace_back();
            event.type = ENetEventType::Message;
            event.peer = id;
            event.delivery = ENetDelivery::Unreliable;
            event.channel = channel;
            event.data.assign(data.begin(), data.end());
            break;
        }
        case MsgUnreliableSeq:
        {
            const uint16 msgSeq = reader.read<uint16>();
            const oc::span<const uint8> data = reader.readBytes(size_t(reader.readVarUInt()));
            if (reader.overflowed())
                return;
            if (peer.recvChannels.size() <= channel)
                peer.recvChannels.resize(channel + 1);
            NetPeer::RecvChannel& recvChannel = peer.recvChannels[channel];
            if (recvChannel.hasUnreliableSeq && !seqGreater(msgSeq, recvChannel.lastUnreliableSeq))
                break; // stale
            recvChannel.hasUnreliableSeq = true;
            recvChannel.lastUnreliableSeq = msgSeq;
            NetEvent& event = m_events.emplace_back();
            event.type = ENetEventType::Message;
            event.peer = id;
            event.delivery = ENetDelivery::UnreliableSequenced;
            event.channel = channel;
            event.data.assign(data.begin(), data.end());
            break;
        }
        case MsgReliable:
        case MsgFragment:
        {
            const uint16 msgSeq = reader.read<uint16>();
            uint16 fragIndex = 0, fragCount = 0;
            if (kind == MsgFragment)
            {
                fragIndex = reader.read<uint16>();
                fragCount = reader.read<uint16>();
            }
            const oc::span<const uint8> data = reader.readBytes(size_t(reader.readVarUInt()));
            if (reader.overflowed())
                return;
            if (peer.recvChannels.size() <= channel)
                peer.recvChannels.resize(channel + 1);
            NetPeer::RecvChannel& recvChannel = peer.recvChannels[channel];
            if (recvChannel.slots.empty())
                recvChannel.slots.resize(NetReliableWindow);
            const uint16 offset = uint16(msgSeq - recvChannel.expectedSeq);
            if (offset < NetReliableWindow) // otherwise already delivered (dup) — the packet ack covers it
            {
                NetPeer::RecvSlot& slot = recvChannel.slots[msgSeq % NetReliableWindow];
                if (!slot.valid)
                {
                    slot.valid = true;
                    slot.kind = kind;
                    slot.fragIndex = fragIndex;
                    slot.fragCount = fragCount;
                    slot.data.assign(data.begin(), data.end());
                }
            }
            break;
        }
        case MsgDisconnect:
        {
            emitDisconnected(id, ENetDisconnectReason::Remote);
            freePeer(id);
            return;
        }
        default:
            return; // unknown message kind, can't parse past it
        }
    }
    for (uint8 channel = 0; channel < peer.recvChannels.size(); ++channel)
        deliverReliable(peer, id, channel);
}

void NetHost::deliverReliable(NetPeer& peer, NetPeerId id, uint8 channel)
{
    NetPeer::RecvChannel& recvChannel = peer.recvChannels[channel];
    if (recvChannel.slots.empty())
        return;
    while (true)
    {
        NetPeer::RecvSlot& slot = recvChannel.slots[recvChannel.expectedSeq % NetReliableWindow];
        if (!slot.valid)
            break;
        if (slot.kind == MsgReliable)
        {
            NetEvent& event = m_events.emplace_back();
            event.type = ENetEventType::Message;
            event.peer = id;
            event.delivery = ENetDelivery::Reliable;
            event.channel = channel;
            event.data = oc::move(slot.data);
        }
        else
        {
            if (slot.fragIndex == 0)
            {
                recvChannel.fragmentAssembly.clear();
                recvChannel.fragmentsReceived = 0;
            }
            if (slot.fragIndex == recvChannel.fragmentsReceived
                && recvChannel.fragmentAssembly.size() + slot.data.size() <= m_config.maxMessageSize)
            {
                recvChannel.fragmentAssembly.insert(recvChannel.fragmentAssembly.end(), slot.data.begin(), slot.data.end());
                if (++recvChannel.fragmentsReceived == slot.fragCount)
                {
                    NetEvent& event = m_events.emplace_back();
                    event.type = ENetEventType::Message;
                    event.peer = id;
                    event.delivery = ENetDelivery::Reliable;
                    event.channel = channel;
                    event.data = oc::move(recvChannel.fragmentAssembly);
                    recvChannel.fragmentAssembly.clear();
                    recvChannel.fragmentsReceived = 0;
                }
            }
            else // corrupt/hostile fragment stream, drop the partial assembly
            {
                recvChannel.fragmentAssembly.clear();
                recvChannel.fragmentsReceived = 0;
            }
        }
        slot.valid = false;
        slot.data.clear();
        ++recvChannel.expectedSeq;
    }
}

void NetHost::processAcks(NetPeer& peer, uint16 ackWire, uint32 ackBits)
{
    if (peer.localSeq == 0)
        return; // nothing sent yet, nothing they could ack
    const uint64 ack = reconstructSeq(ackWire, peer.localSeq - 1);
    const auto ackPacket = [&](uint64 seq)
    {
        if (seq >= peer.localSeq)
            return;
        NetPeer::SentPacket& info = peer.sentPackets[seq % NetSentPacketRing];
        if (!info.used || info.seq != seq || info.acked)
            return;
        info.acked = true;
        const float rttSample = float(m_time - info.sendTime);
        peer.smoothedRttSec = peer.hasRtt ? peer.smoothedRttSec + 0.125f * (rttSample - peer.smoothedRttSec) : rttSample;
        peer.hasRtt = true;
        for (const uint32 reliableId : info.reliableIds)
        {
            const uint8 channel = uint8(reliableId >> 16);
            if (channel >= peer.sendChannels.size())
                continue;
            NetPeer::SendChannel& sendChannel = peer.sendChannels[channel];
            if (sendChannel.window.empty())
                continue;
            const uint16 offset = uint16(uint16(reliableId) - sendChannel.window.front().seq);
            if (offset < sendChannel.window.size())
                sendChannel.window[offset].acked = true;
        }
    };
    ackPacket(ack);
    for (uint32 i = 0; i < 32; ++i)
        if ((ackBits & (1u << i)) && ack >= i + 1)
            ackPacket(ack - 1 - i);
    for (NetPeer::SendChannel& sendChannel : peer.sendChannels)
        while (!sendChannel.window.empty() && sendChannel.window.front().acked)
            sendChannel.window.pop_front();
}

void NetHost::transmitPayload(NetPeer& peer, oc::span<const uint8> body, bool bypassSim)
{
    uint8 packet[NetMaxRawPacketSize];
    packet[0] = uint8(PktPayload | (peer.hasRemoteSeq ? PktFlagHasAck : 0));
    const uint16 wireSeq = uint16(peer.localSeq);
    memcpy(packet + 1, &wireSeq, sizeof(wireSeq));
    size_t packetSize = NetPacketHeaderSize;
    if (m_config.encrypt)
    {
        uint8 nonce[12] = {};
        nonce[0] = peer.isServerRole ? 1 : 0;
        memcpy(nonce + 4, &peer.localSeq, sizeof(peer.localSeq));
        if (!cryptoSeal(peer.key, nonce, oc::span<const uint8>(packet, NetPacketHeaderSize), body,
            oc::span(packet + NetPacketHeaderSize, body.size() + NetCryptoTagSize)))
            return;
        packetSize += body.size() + NetCryptoTagSize;
    }
    else
    {
        memcpy(packet + NetPacketHeaderSize, body.data(), body.size());
        packetSize += body.size();
    }
    ++peer.localSeq;
    peer.lastSendTime = m_time;
    peer.ackDirty = false;
    sendRaw(peer.address, oc::span<const uint8>(packet, packetSize), bypassSim);
}

void NetHost::sendDisconnectPackets(NetPeer& peer, bool bypassSim)
{
    for (int i = 0; i < 3; ++i) // distinct seqs so packet-level dedup can't eat the retries
    {
        uint8 body[NetPacketAckSize + 1];
        NetWriter writer(body);
        writer.write<uint16>(uint16(peer.remoteSeq));
        writer.write<uint32>(peer.remoteSeqBits);
        writer.write<uint8>(MsgDisconnect);
        transmitPayload(peer, writer.data(), bypassSim);
    }
}

void NetHost::flushPeer(NetPeer& peer)
{
    const uint32 bodyMax = m_config.maxPacketSize - NetPacketHeaderSize - (m_config.encrypt ? NetCryptoTagSize : 0);
    const double rto = peer.hasRtt
        ? oc::min(oc::max(double(peer.smoothedRttSec) * 2.0 + 0.01, m_config.resendMinSec), m_config.resendMaxSec)
        : 0.2;

    uint8 bodyBuffer[NetMaxRawPacketSize];
    NetWriter body(oc::span(bodyBuffer, bodyMax));
    oc::vector<uint32>& reliableIds = m_scratchReliableIds;
    reliableIds.clear();
    bool sentAny = false;

    const auto openBody = [&]
    {
        body.reset();
        body.write<uint16>(uint16(peer.remoteSeq));
        body.write<uint32>(peer.remoteSeqBits);
    };
    const auto sendPacket = [&]
    {
        NetPeer::SentPacket& info = peer.sentPackets[peer.localSeq % NetSentPacketRing];
        if (info.used) // recycling a slot: sample its fate into the loss estimate
            peer.packetLoss += ((info.acked ? 0.0f : 1.0f) - peer.packetLoss) * 0.02f;
        info.seq = peer.localSeq;
        info.used = true;
        info.acked = false;
        info.sendTime = m_time;
        info.reliableIds = reliableIds;
        reliableIds.clear();
        transmitPayload(peer, body.data(), false);
        sentAny = true;
    };

    openBody();
    for (uint8 channel = 0; channel < peer.sendChannels.size(); ++channel)
    {
        NetPeer::SendChannel& sendChannel = peer.sendChannels[channel];
        const size_t eligible = sendChannel.window.size() < NetReliableWindow ? sendChannel.window.size() : NetReliableWindow;
        for (size_t i = 0; i < eligible; ++i)
        {
            NetPeer::ReliableMsg& msg = sendChannel.window[i];
            if (msg.acked || (msg.lastSendTime >= 0.0 && m_time - msg.lastSendTime < rto))
                continue;
            if (body.size() + msg.encoded.size() > bodyMax)
            {
                sendPacket();
                openBody();
            }
            body.writeBytes(msg.encoded);
            msg.lastSendTime = m_time;
            reliableIds.push_back((uint32(channel) << 16) | msg.seq);
        }
    }
    for (const oc::vector<uint8>& blob : peer.unreliableQueue)
    {
        if (body.size() + blob.size() > bodyMax)
        {
            sendPacket();
            openBody();
        }
        body.writeBytes(blob);
    }
    peer.unreliableQueue.clear();

    if (body.size() > NetPacketAckSize)
        sendPacket();
    else if (!sentAny && (peer.ackDirty || m_time - peer.lastSendTime >= m_config.keepAliveSec))
        sendPacket(); // empty payload = ack/keepalive
}

// ---- plumbing -----------------------------------------------------------------------------------

void NetHost::sendRaw(const NetAddress& to, oc::span<const uint8> bytes, bool bypassSim)
{
    ++m_accumPacketsSent;
    m_accumBytesSent += bytes.size();
    if (!bypassSim)
    {
        if (m_config.simPacketLoss > 0.0f && rand01() < m_config.simPacketLoss)
            return;
        if (m_config.simLatencyMs > 0.0f || m_config.simJitterMs > 0.0f)
        {
            DelayedSend& delayed = m_delayedSends.emplace_back();
            delayed.releaseTime = m_time + double(m_config.simLatencyMs + m_config.simJitterMs * rand01()) * 0.001;
            delayed.to = to;
            delayed.data.assign(bytes.begin(), bytes.end());
            return;
        }
    }
    m_socket.sendTo(to, bytes);
}

void NetHost::emitConnected(NetPeerId id)
{
    NetEvent& event = m_events.emplace_back();
    event.type = ENetEventType::Connected;
    event.peer = id;
}

void NetHost::emitDisconnected(NetPeerId id, ENetDisconnectReason reason)
{
    NetEvent& event = m_events.emplace_back();
    event.type = ENetEventType::Disconnected;
    event.peer = id;
    event.reason = reason;
}

NetPeerId NetHost::allocPeer(const NetAddress& address)
{
    NetPeerId id = InvalidNetPeerId;
    for (NetPeerId i = 0; i < m_peers.size(); ++i)
    {
        if (m_peers[i].state == NetPeer::EState::Free)
        {
            id = i;
            break;
        }
    }
    if (id == InvalidNetPeerId)
    {
        if (m_peers.size() >= m_config.maxPeers)
            return InvalidNetPeerId;
        m_peers.emplace_back();
        id = NetPeerId(m_peers.size() - 1);
    }
    NetPeer& peer = m_peers[id];
    peer = NetPeer{};
    peer.sentPackets.resize(NetSentPacketRing);
    peer.address = address;
    peer.connectStartTime = peer.lastReceiveTime = peer.lastSendTime = m_time;
    peer.lastHandshakeSendTime = -1.0;
    m_peerByAddress[address.key()] = id;
    return id;
}

void NetHost::freePeer(NetPeerId id)
{
    NetPeer& peer = m_peers[id];
    m_peerByAddress.erase(peer.address.key());
    peer.state = NetPeer::EState::Free;
    peer.key.destroy();
    peer.sendChannels.clear();
    peer.recvChannels.clear();
    peer.unreliableQueue.clear();
    peer.sentPackets.clear();
    peer.sendWindowOverflow = false; // slots recycle — never inherit the previous occupant's state
}

float NetHost::rand01()
{
    m_rngState += 0x9E3779B97F4A7C15ull;
    uint64 z = m_rngState;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return float(z >> 40) * (1.0f / 16777216.0f);
}
