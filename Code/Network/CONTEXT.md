# Network

> Library documentation for `Code/Network`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction.
>
> This is the TRANSPORT only. The game-level replication layer (NetworkManager / NetworkComponent) is
> in [`Code/Entity/CONTEXT.md`](../Entity/CONTEXT.md) under **Multiplayer**; the fuzzer that gates
> wire changes is [`Code/NetFuzz/CONTEXT.md`](../NetFuzz/CONTEXT.md).

Winsock UDP/TCP plus a reliable-UDP game protocol. Core-only; `Ws2_32` and `Bcrypt` are PRIVATE.

Partitions `:Address`, `:Serialization`, `:Socket`, `:Crypto` (nothing exported), `:Reliable`,
re-exported from `Public/Network.ixx`.

## Hard constraints

* **IPv4 only.**
* **Raw native little-endian wire format: no tags, no padding, no versioning.** Both ends must agree
  on the layout, gated only by `NetHostConfig::protocolId` (default `0x4F435331` = "OCS1").
  Any wire change means bumping the id and re-running NetFuzz.
* **`NetHost` is single-threaded and main-thread-only.**

## `NetHost` — the reliable-UDP protocol

[Reliable.ixx:211](Private/Reliable.ixx#L211). Symmetric: one host accepts incoming (server),
connects out (client), or both at once. Two hosts that `connect()` to each other resolve the
simultaneous handshake automatically (p2p, tie-broken on the connect salts).

### Poll model

```cpp
open(port = 0, NetHostConfig{})     // port 0 = ephemeral (client/p2p)
connect(address) -> NetPeerId       // returns the existing peer if already known
send(peer, bytes, ENetDelivery, channel = 0)   /  sendToAll(...)
update(deltaSec)                    // pump socket, handshakes, timeouts, retransmits, flush packets
takeEvents() -> vector<NetEvent>    // Connected / Disconnected / Message
```

Queries: `isConnected`, `getConnectedCount`, `getPeerAddress`, `findPeer`, `getPeerRttMs`,
`getPeerPacketLoss`, `getQueuedReliable`, `getStats`, and `config()` for live edits.

> `config()` may be edited live for **timeouts and the `sim*` fields only**. `protocolId`,
> `maxPacketSize` and `encrypt` must not change after `open()`.

### Deliveries and channels

| Delivery | Behaviour |
|---|---|
| `Unreliable` | Fire and forget. **Must fit in one packet — a bigger one is DROPPED**, since there is nothing to reassemble it with. |
| `UnreliableSequenced` | Same, but out-of-date messages are dropped on arrival, per channel. |
| `Reliable` | Ordered and guaranteed within its channel; **fragments transparently** up to `maxMessageSize` (1 MB). |

`NetMaxChannels` is 8. **The channel index rides bits 3–5 of the message kind byte, so it costs
nothing on the wire** — and each channel is ordered only within itself, so a bulk transfer on one
never head-of-line-blocks another.

`netMaxSinglePacketMessage(maxPacketSize, encrypted)`
([Reliable.ixx:130](Private/Reliable.ixx#L130)) is the exported way to derive your own message cap.
**Use it instead of hardcoding a margin**, which silently rots the moment any header grows. Entity's
`MaxEventDataBytes` is static_asserted against it.

### Packet layout

```
[type u8][seq u16] + { [ack u16][ackBits u32] + messages }        (the braces are sealed when encrypted)
  Unreliable:          [kind u8][len varint][bytes]
  UnreliableSequenced: [kind u8][seq u16][len varint][bytes]
  Reliable:            [kind u8][seq u16][len varint][bytes]
  Fragment:            [kind u8][seq u16][idx u16][cnt u16][len varint][bytes]
  Disconnect:          [kind u8]
```

* **Acks are packet-level** — the latest received seq plus a 32-packet history bitfield, piggybacked
  on every packet.
* **A reliable message retransmits when every packet that carried it has timed out** (~2× RTT,
  clamped to `resendMinSec` 0.05 .. `resendMaxSec` 0.5).
* Empty payload packets double as acks and keepalives (`keepAliveSec` 0.1).
* Per peer: a 1024-entry sent-packet ring, and a 256-message reliable window per channel that doubles
  as the receive reorder window.

### Handshake

4-way, anti-spoofing. **The responder stores no state and allocates nothing until the challenge
round-trips through the initiator's claimed address:**

```
Request   [protocolId][flags][clientSalt][pad][pubkey?]  ->
Challenge [clientSalt][serverSalt = HMAC(hostSecret, addr|salt|flags)]  ->
Response  [flags][clientSalt][serverSalt][pubkey?]       ->   (responder verifies the HMAC, allocates the peer)
Accept    [clientSalt][pubkey?]
```

`ENetDisconnectReason`: `Local`, `Remote`, `Timeout`, `ConnectFailed`, `Denied` (host full, refuses
incoming, **or the encrypt setting mismatches**), `Overflow`.

### Encryption (`NetHostConfig::encrypt`)

Both ends exchange **ephemeral ECDH P-256** keys in the handshake, and every payload packet is
**AES-128-GCM** sealed (+16 B tag). Ack fields and messages are encrypted; the 3-byte header is
authenticated as AAD. The nonce is the sender role plus an implicit 64-bit packet counter,
reconstructed from the 16-bit wire seq.

All of it is Windows CNG (hardware AES) behind `:Crypto`, which exports nothing —
`NetHostConfig::encrypt` is the entire public surface.

> **Unauthenticated key exchange: safe against spoofing, eavesdropping and tampering — NOT against
> an active MITM.**

## Abuse limits

All in `NetHostConfig` ([Reliable.ixx:90](Private/Reliable.ixx#L90)), and **all enforced before any
parsing or allocation**.

| Limit | Default | Why |
|---|---|---|
| `maxPacketsPerSecPerAddress` | 400 | Packet rate is what costs CPU. Generous against real traffic: one peer at 60 Hz snapshots + claims + acks runs well under 200/s. |
| `packetBurstPerAddress` | 200 | Bucket cap; absorbs legitimate bursts after a stall. |
| `maxPacketsPerUpdate` | 8192 | Global work ceiling per `update()`. A flood cannot make one frame unbounded no matter how many addresses it uses. |
| `maxPeersPerIp` | 4 | One machine must not eat every connection slot. |
| `maxQueuedReliablePerChannel` | 1024 | A peer that stops acking while we keep queueing is an unbounded memory sink. |

**The token bucket lives in a FIXED 512-slot table** (`RateBucketCount`,
[Reliable.ixx:299](Private/Reliable.ixx#L299)), keyed by source address. Fixed because the thing
being defended against is an attacker who varies its source address — a per-address map would itself
be the memory attack. Two addresses colliding on a slot merely share a (generous) budget.

> **`maxQueuedReliablePerChannel` is a limit for peers that STOP ACKING, not a buffer to fill.**
> A caller streaming many reliable messages — the Entity layer's spawn replay — must pace itself with
> `getQueuedReliable(peer, channel)` and stay well under it. Overflow is latched
> (`NetPeer::sendWindowOverflow`) and acted on in `update()`; **never free a peer inside `send()`**.

`NetHostStats::packetsDroppedPerSec` surfaces rate limiting — sustained nonzero means a flood, or
limits set too tight. The server title bar shows it.

## `:Serialization`

`NetWriter` ([Serialization.ixx:9](Private/Serialization.ixx#L9)) and `NetReader`
([Serialization.ixx:96](Private/Serialization.ixx#L96)) work over caller-provided spans.

* Trivially-copyable `write<T>` / `read<T>`, plus `writeAt` to patch a count written before its
  items.
* **Varint** — LEB128, with zigzag for signed. Strings are varint length + bytes.
* **Quantization** — `writeQuantized<UInt>(v, min, max)` over the full range of the integer type,
  plus `unorm8/16` and `snorm8/16` helpers.
* **Overflow-safe**: check `overflowed()` after a batch. Nothing ever touches memory out of bounds.
* **Reads are zero-copy views** (`readBytes`, `readString`), valid only while the underlying buffer
  lives. This is exactly the invariant NetFuzz's `reader` mode asserts.

> The bounds check is written as `numBytes > size - pos`, **not** `pos + numBytes > size`: `numBytes`
> is a wire-supplied length, and near 2^64 that addition wraps past the check
> ([Serialization.ixx:165](Private/Serialization.ixx#L165)).

## `:Socket`

Non-blocking, RAII movable handles. Winsock initializes lazily on first use.

| Type / function | Notes |
|---|---|
| `UdpSocket` | 512 KB `SO_RCVBUF` and `SO_SNDBUF`, optional `SO_BROADCAST`, and **`SIO_UDP_CONNRESET` disabled** — otherwise an ICMP port-unreachable from one peer fails subsequent `recvfrom` calls. |
| `TcpSocket` / `TcpListener` | `TCP_NODELAY` set; `poll()` reports `Closed` / `Connecting` / `Connected` / `Failed`. |
| `TcpMessageStream` ([Socket.ixx:144](Private/Socket.ixx#L144)) | Length-prefixed framing (`uint32` size + payload) with buffering for partial non-blocking sends and receives. `MaxMessageSize` 16 MB; a bigger declared size closes the socket, since there is no way to resync a hostile stream. Call `flushSend()` every frame while data is pending. |
| `netResolveHost(host, port)` | Blocking DNS, first IPv4 result. |
| `netGetLocalAddress()` ([Socket.ixx:17](Private/Socket.ixx#L17)) | The LAN IPv4 other machines dial. Route selection through a **connected UDP socket — no packet is sent**. Loopback when there is no route. |
| `netGetExternalAddress(timeoutMs = 3000)` ([Socket.ixx:24](Private/Socket.ixx#L24)) | The PUBLIC IPv4. A plain-HTTP GET to checkip.amazonaws.com and fallbacks — **the engine has no TLS, so HTTPS-only services are out**. **BLOCKING: background thread only.** The router must still forward the port for the endpoint to be reachable. |

Both address helpers return port 0; callers fill in their own. They feed the main menu's host display
— see [`Code/App/CONTEXT.md`](../App/CONTEXT.md).

## Link simulation

`simPacketLoss` (0..1 drop chance), `simLatencyMs`, `simJitterMs` apply to OUTGOING packets and are
live-editable. Delayed sends are held in `m_delayedSends` and released by `update()`. Disconnect
packets bypass the sim so teardown is not delayed.
