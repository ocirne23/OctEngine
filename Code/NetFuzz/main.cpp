// Protocol fuzzer for Code/Network and the game protocol on top of it. Run after ANY wire-format
// change. A crash is the finding; every mode prints its seed, so a failure reproduces exactly.
//
//   NetFuzz reader [iterations] [seed]   NetReader primitives vs random buffers (invariant-checked)
//   NetFuzz host   [iterations] [seed]   raw hostile packets at an in-process NetHost server
//   NetFuzz game   <ip[:port]> [iter]    hostile client vs a live `App --server --headless` — the
//                                        only mode reaching NetworkManager's handlers
//   NetFuzz all    [iterations] [seed]   reader + host
//
import Core;
import Network;

// ---- deterministic RNG (never time-seeded: a finding must reproduce) ----------------------------

struct Rng
{
    uint64 state = 0;

    explicit Rng(uint64 seed) : state(seed ? seed : 0x9e3779b97f4a7c15ull) {}

    uint64 next()
    {
        state += 0x9e3779b97f4a7c15ull;
        uint64 z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    uint32 below(uint32 bound) { return bound ? uint32(next() % bound) : 0; }
    bool chance(uint32 percent) { return below(100) < percent; }
    uint8 byte() { return uint8(next()); }
};

// pure noise rarely reaches deep parser states: corrupt mostly-valid data instead
static void fillFuzzed(Rng& rng, oc::vector<uint8>& out, size_t size)
{
    out.resize(size);
    for (uint8& b : out)
        b = rng.byte();
    if (rng.chance(40) && size >= 10)
    {
        // a varint decoding to ~2^64: random bytes hit the length-vs-capacity arithmetic rarely
        const size_t at = rng.below(uint32(size - 10));
        for (int i = 0; i < 9; ++i)
            out[at + i] = uint8(0xff);
        out[at + 9] = 0x01;
    }
}

// ---- mode: NetReader primitives -----------------------------------------------------------------

static int fuzzReader(uint32 iterations, uint64 seed)
{
    printf("[reader] %u iterations, seed %llu\n", iterations, (unsigned long long)seed);
    Rng rng(seed);
    oc::vector<uint8> buffer;
    uint64 overflows = 0;

    for (uint32 i = 0; i < iterations; ++i)
    {
        fillFuzzed(rng, buffer, 1 + rng.below(1400));
        const oc::span<const uint8> input(buffer);
        NetReader reader(input);

        // drive a random sequence of reads until the reader gives up
        const uint32 ops = 1 + rng.below(24);
        for (uint32 op = 0; op < ops && !reader.overflowed(); ++op)
        {
            switch (rng.below(8))
            {
            case 0: reader.read<uint8>(); break;
            case 1: reader.read<uint16>(); break;
            case 2: reader.read<uint32>(); break;
            case 3: reader.readVarUInt(); break;
            case 4: reader.readVarInt(); break;
            case 5: reader.readQuantized<uint16>(-50.0f, 50.0f); break;
            case 6:
            {
                // the invariant: a returned view must lie inside the input buffer. Catches a
                // wrapped bounds check here rather than at a later OOB dereference.
                const oc::string_view text = reader.readString();
                if (!text.empty())
                {
                    const uint8* begin = reinterpret_cast<const uint8*>(text.data());
                    if (begin < input.data() || text.size() > size_t(input.data() + input.size() - begin))
                    {
                        printf("[reader] FAIL iteration %u: readString escaped the buffer (size %zu)\n", i, text.size());
                        return 1;
                    }
                }
                break;
            }
            default:
            {
                const oc::span<const uint8> bytes = reader.readBytes(size_t(reader.readVarUInt()));
                if (!bytes.empty()
                    && (bytes.data() < input.data()
                        || bytes.size() > size_t(input.data() + input.size() - bytes.data())))
                {
                    printf("[reader] FAIL iteration %u: readBytes escaped the buffer (size %zu)\n", i, bytes.size());
                    return 1;
                }
                break;
            }
            }
            if (reader.position() > input.size())
            {
                printf("[reader] FAIL iteration %u: cursor %zu past end %zu\n", i, reader.position(), input.size());
                return 1;
            }
        }
        overflows += reader.overflowed() ? 1 : 0;
    }
    printf("[reader] ok (%llu/%u runs ended in a clean overflow)\n", (unsigned long long)overflows, iterations);
    return 0;
}

// ---- transport packet construction --------------------------------------------------------------
// Mirrors Reliable.cpp's private constants on purpose: sharing the sender's helpers could only ever
// produce well-formed packets.

namespace Pkt
{
    constexpr uint8 ConnectRequest = 1;
    constexpr uint8 Challenge = 2;
    constexpr uint8 ChallengeResponse = 3;
    constexpr uint8 ConnectAccept = 4;
    constexpr uint8 Payload = 6;
    constexpr uint8 FlagHasAck = 0x80;
    constexpr uint32 RequestPad = 16;
}

// Completes the real 4-way handshake: the payload/message parsers only run for a CONNECTED peer.
// Encryption off — sealed packets would only fail to open. `pump` advances an in-process server
// between polls (it has no thread of its own); omit it for a real one.
static bool handshake(UdpSocket& socket, const NetAddress& server, uint32 protocolId, uint64 clientSalt,
    const oc::function<void()>& pump = {})
{
    // drain: after a fuzz phase this queue holds thousands of replies to earlier garbage (random
    // packets parse as valid ConnectRequests), which would eat the poll budget below
    {
        uint8 stale[1500];
        NetAddress staleFrom;
        while (socket.receiveFrom(stale, staleFrom) >= 0)
            ;
    }

    uint8 out[64];
    NetWriter request(out);
    request.write<uint8>(Pkt::ConnectRequest);
    request.write<uint32>(protocolId);
    request.write<uint8>(0); // flags: no encryption
    request.write<uint64>(clientSalt);
    for (uint32 i = 0; i < Pkt::RequestPad; ++i)
        request.write<uint8>(0);
    socket.sendTo(server, request.data());

    uint8 in[1500];
    NetAddress from;
    for (int attempt = 0; attempt < 400; ++attempt)
    {
        const int size = socket.receiveFrom(in, from);
        if (size <= 0)
        {
            if (pump)
                pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        NetReader reader(oc::span<const uint8>(in, size));
        const uint8 type = reader.read<uint8>() & 0x7f;
        if (type != Pkt::Challenge || reader.read<uint32>() != protocolId)
            continue;
        if (reader.read<uint64>() != clientSalt)
            continue;
        const uint64 serverSalt = reader.read<uint64>();
        if (reader.overflowed())
            continue;

        NetWriter response(out);
        response.write<uint8>(Pkt::ChallengeResponse);
        response.write<uint32>(protocolId);
        response.write<uint8>(0);
        response.write<uint64>(clientSalt);
        response.write<uint64>(serverSalt);
        socket.sendTo(server, response.data());

        for (int accept = 0; accept < 400; ++accept)
        {
            const int acceptSize = socket.receiveFrom(in, from);
            if (acceptSize <= 0)
            {
                if (pump)
                    pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if ((in[0] & 0x7f) == Pkt::ConnectAccept)
                return true;
        }
        return false;
    }
    return false;
}

// `seq` must advance or the receiver dedups the packet and the fuzzed bytes are never parsed
static void sendPayload(UdpSocket& socket, const NetAddress& to, uint16 seq, oc::span<const uint8> body)
{
    uint8 packet[1500];
    NetWriter writer(packet);
    writer.write<uint8>(Pkt::Payload | Pkt::FlagHasAck);
    writer.write<uint16>(seq);
    writer.write<uint16>(0); // ack
    writer.write<uint32>(0); // ackBits
    writer.writeBytes(body);
    if (!writer.overflowed())
        socket.sendTo(to, writer.data());
}

// ---- mode: raw packets at an in-process NetHost --------------------------------------------------

static int fuzzHost(uint32 iterations, uint64 seed)
{
    printf("[host] %u iterations, seed %llu\n", iterations, (unsigned long long)seed);
    Rng rng(seed);

    NetHostConfig config;
    config.protocolId = 0x46555a5a; // "FUZZ" — isolated from a real server on the same machine
    NetHost server;
    if (!server.open(0, config))
    {
        printf("[host] FAIL: could not open server socket\n");
        return 1;
    }
    const NetAddress serverAddress = NetAddress::loopback(server.getLocalPort());

    UdpSocket attacker;
    if (!attacker.open(0))
    {
        printf("[host] FAIL: could not open attacker socket\n");
        return 1;
    }

    // Smoke test for the ENCRYPTED path (the game's default): two real hosts must complete the ECDH
    // handshake and exchange a sealed payload. The fuzz phases below run plaintext because forging
    // packets requires the key, so without this the encrypted path would go untested here entirely.
    {
        NetHostConfig secureConfig = config;
        secureConfig.encrypt = true;
        secureConfig.protocolId = config.protocolId + 1; // isolated from the plaintext server below
        NetHost secureServer, secureClient;
        if (!secureServer.open(0, secureConfig) || !secureClient.open(0, secureConfig))
        {
            printf("[host] FAIL: could not open encrypted hosts\n");
            return 1;
        }
        const NetPeerId peer = secureClient.connect(NetAddress::loopback(secureServer.getLocalPort()));
        bool connected = false, delivered = false;
        for (int i = 0; i < 400 && !delivered; ++i)
        {
            secureServer.update(0.005);
            secureClient.update(0.005);
            for (const NetEvent& evt : secureServer.takeEvents())
                if (evt.type == ENetEventType::Message)
                    delivered = true;
            for (const NetEvent& evt : secureClient.takeEvents())
                if (evt.type == ENetEventType::Connected)
                {
                    connected = true;
                    const uint8 payload[] = { 1, 2, 3, 4 };
                    secureClient.send(peer, payload, ENetDelivery::Reliable, 2);
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!connected || !delivered)
        {
            printf("[host] FAIL: encrypted handshake/payload did not complete (connected=%d delivered=%d)\n",
                int(connected), int(delivered));
            return 1;
        }
        printf("[host] encrypted handshake + sealed payload ok\n");
    }

    // regression: a brand-new address must connect on its FIRST packet, at the default limits
    {
        UdpSocket early;
        if (!early.open(0) || !handshake(early, serverAddress, config.protocolId, rng.next(),
                [&] { server.update(0.002); }))
        {
            printf("[host] FAIL: fresh address could not connect immediately (rate limiter?)\n");
            return 1;
        }
        for (int i = 0; i < 4; ++i)
            server.update(0.002);
        server.takeEvents();
        printf("[host] cold-start connect ok\n");
    }

    // limits off from here: engaged, they discard the corpus at the door and the run becomes a
    // test of the limiter instead of the parsers behind it
    server.config().maxPacketsPerSecPerAddress = 100000000;
    server.config().packetBurstPerAddress = 100000000;
    server.config().maxPacketsPerUpdate = 100000000;

    // phase 1: everything an unauthenticated sender can reach
    oc::vector<uint8> buffer;
    for (uint32 i = 0; i < iterations / 2; ++i)
    {
        fillFuzzed(rng, buffer, 1 + rng.below(1400));
        if (rng.chance(60) && buffer.size() >= 5)
        {
            buffer[0] = 1 + rng.byte() % 6;            // a real packet type
            const uint32 id = config.protocolId;        // ...and usually the real protocol id, or
            memcpy(buffer.data() + 1, &id, sizeof(id)); // everything is rejected in one line
        }
        attacker.sendTo(serverAddress, buffer);
        if ((i % 16) == 0)
            server.update(0.001);
    }
    server.update(0.001);
    server.takeEvents();
    printf("[host] pre-auth phase survived\n");

    // phase 2: the authenticated surface
    // fresh socket: clean peer slot and receive queue
    UdpSocket client;
    if (!client.open(0))
    {
        printf("[host] FAIL: could not open client socket\n");
        return 1;
    }
    if (!handshake(client, serverAddress, config.protocolId, rng.next(), [&] { server.update(0.002); }))
    {
        printf("[host] FAIL: handshake did not complete (server rejected a well-formed request?)\n");
        return 1;
    }
    // the server needs to see the response and allocate the peer
    for (int i = 0; i < 8; ++i)
        server.update(0.002);
    printf("[host] handshake complete, fuzzing authenticated payloads\n");

    uint16 seq = 1;
    for (uint32 i = iterations / 2; i < iterations; ++i)
    {
        fillFuzzed(rng, buffer, 1 + rng.below(1200));
        if (rng.chance(70) && !buffer.empty())
        {
            // steer the first byte to a valid message kind + channel so the parser walks into the
            // per-kind bodies (length varints, fragment indices) instead of bailing on kind 7
            const uint8 kind = uint8(rng.below(5));
            const uint8 channel = uint8(rng.below(8));
            buffer[0] = uint8(kind | (channel << 3));
        }
        sendPayload(client, serverAddress, seq++, buffer);
        if ((i % 8) == 0)
        {
            server.update(0.001);
            server.takeEvents(); // drain, or a long run just accumulates delivered garbage
        }
    }
    server.update(0.001);
    server.takeEvents();

    const NetHostStats& stats = server.getStats();
    printf("[host] ok (received %u pkt/s, dropped %u pkt/s at the rate limiter)\n",
        stats.packetsReceivedPerSec, stats.packetsDroppedPerSec);
    return 0;
}

// ---- mode: hostile client vs a live game server --------------------------------------------------

static int fuzzGame(const NetAddress& target, uint32 iterations, uint64 seed)
{
    printf("[game] target %s, %u iterations, seed %llu\n", target.toString().c_str(), iterations,
        (unsigned long long)seed);
    // --no-encrypt is required: this mode speaks the handshake by hand and does not implement ECDH,
    // and an encrypted server denies a plaintext one. The parsers behind it are identical either way.
    printf("[game] run a server first:  App.exe --server --headless --no-encrypt\n");
    Rng rng(seed);

    UdpSocket socket;
    if (!socket.open(0))
    {
        printf("[game] FAIL: could not open socket\n");
        return 1;
    }
    // must match GameProtocolId in NetworkManager.cpp — bumped on every wire change, which is the
    // signal to re-run this mode
    constexpr uint32 GameProtocolId = 0x4F435346;
    if (!handshake(socket, target, GameProtocolId, rng.next()))
    {
        printf("[game] FAIL: no handshake — is the server running, and is GameProtocolId current?\n");
        return 1;
    }
    printf("[game] connected, sending malformed game messages\n");

    // Message ids from NetworkManager.cpp's ENetMsg. Hello(1) first so the server mints a client id
    // and the owner-gated handlers (Claim) are reachable at all.
    oc::vector<uint8> body;
    uint16 seq = 1;
    {
        uint8 hello[8];
        NetWriter writer(hello);
        writer.write<uint8>(uint8(2 | (2 << 3))); // MsgReliable on the session channel
        writer.write<uint16>(0);                  // message seq
        writer.writeVarUInt(3);
        writer.write<uint8>(1);      // ENetMsg::Hello
        writer.write<uint16>(14);    // GameNetVersion
        sendPayload(socket, target, seq++, writer.data());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for (uint32 i = 0; i < iterations; ++i)
    {
        const size_t payloadSize = 1 + rng.below(400);
        fillFuzzed(rng, body, payloadSize);
        body[0] = uint8(1 + rng.below(9)); // an ENetMsg id

        uint8 message[1500];
        NetWriter writer(message);
        const uint8 channel = uint8(rng.below(4));
        const bool reliable = rng.chance(50);
        writer.write<uint8>(uint8((reliable ? 2 : 0) | (channel << 3)));
        if (reliable)
            writer.write<uint16>(uint16(i + 1)); // in-order, or the receiver window drops them
        writer.writeVarUInt(body.size());
        writer.writeBytes(body);
        if (!writer.overflowed())
            sendPayload(socket, target, seq++, writer.data());

        // stay under the transport rate limiter (400/s per address) — tripping it would mean
        // fuzzing the limiter instead of the parsers
        if ((i % 32) == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("[game] ok — server still up (check its log for warnings, and that it kept simulating)\n");
    return 0;
}

// -------------------------------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    const oc::string mode = argc > 1 ? argv[1] : "all";
    // fixed default seed: an unreproducible fuzzer finding is nearly worthless
    const uint64 seed = argc > 3 ? strtoull(argv[3], nullptr, 10) : 0x1234;

    if (mode == "reader")
        return fuzzReader(argc > 2 ? uint32(atoi(argv[2])) : 200000, seed);
    if (mode == "host")
        return fuzzHost(argc > 2 ? uint32(atoi(argv[2])) : 20000, seed);
    if (mode == "game")
    {
        if (argc < 3)
        {
            printf("usage: NetFuzz game <ip[:port]> [iterations] [seed]\n");
            return 2;
        }
        NetAddress target = NetAddress::fromString(argv[2]);
        if (target.port == 0)
            target.port = 27888; // the App's default server port
        return fuzzGame(target, argc > 3 ? uint32(atoi(argv[3])) : 5000, argc > 4 ? strtoull(argv[4], nullptr, 10) : 0x1234);
    }
    if (mode == "all")
    {
        const uint32 iterations = argc > 2 ? uint32(atoi(argv[2])) : 0;
        if (const int result = fuzzReader(iterations ? iterations : 200000, seed))
            return result;
        if (const int result = fuzzHost(iterations ? iterations : 20000, seed))
            return result;
        printf("all modes passed\n");
        return 0;
    }
    printf("usage: NetFuzz reader|host|game|all [...]\n");
    return 2;
}
