# Network

> Library documentation for `Code/Network`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.
>
> The game-level replication layer (NetworkManager / NetworkComponent) is in
> [`Code/Entity/CONTEXT.md`](../Entity/CONTEXT.md) under **Multiplayer**; the fuzzer in
> [`Code/NetFuzz/CONTEXT.md`](../NetFuzz/CONTEXT.md).

## Overview

Winsock UDP/TCP plus a reliable-UDP game protocol. Core-only; `Ws2_32` and `Bcrypt` are PRIVATE.

* Partitions `:Address`, `:Serialization`, `:Socket`, `:Crypto` (internal), `:Reliable`, re-exported
  from `Public/Network.ixx`.
* IPv4 only. Raw little-endian wire format, no versioning — both ends must match (`protocolId`).
* `NetHost` is strictly single-threaded, main-thread only.

## `:Serialization`

`NetWriter` / `NetReader` work over caller-provided spans.

* Trivially-copyable `write<T>` / `read<T>`, varint (LEB128 + zigzag), strings, unorm/snorm,
  `writeQuantized`.
* Overflow-safe (`overflowed()`); reads are zero-copy views.

## `:Socket`

Non-blocking and RAII throughout.

* `UdpSocket` — big `SO_RCVBUF`, `SIO_UDP_CONNRESET` disabled.
* `TcpSocket` / `TcpListener`, and `TcpMessageStream` (length-framed).
* `netResolveHost`.
* `netGetLocalAddress` — the LAN IPv4 other machines dial. Uses the connected-UDP route-selection
  trick, so no packet is sent.
* `netGetExternalAddress` — the PUBLIC IPv4. Plain-HTTP GET to checkip.amazonaws.com, api.ipify.org
  or icanhazip.com; the engine has no TLS, so HTTPS-only services are out. BLOCKING up to a timeout,
  so background-thread only.
* Both address queries feed the main menu's host display. Winsock initializes lazily.

## `:Reliable` — `NetHost`

Symmetric client / server / p2p over one `UdpSocket`. A simultaneous handshake is tie-broken on the
connect salts.

**Poll model**

* `send(peer, bytes, ENetDelivery, channel)`
* per-frame `update(dt)` — batches into packets of at most `maxPacketSize`
* drain with `takeEvents()`

**Deliveries** — Unreliable, UnreliableSequenced, Reliable (ordered; ack bitfield piggybacked on
every packet, RTT retransmit, transparent fragmentation), over 8 channels. The channel rides the kind
byte, so it costs nothing on the wire and bulk transfers do not head-of-line-block.

**Handshake and encryption**

* 4-way anti-spoof challenge handshake; the responder stays stateless until the challenge
  round-trips.
* `NetHostConfig::encrypt` adds ephemeral ECDH P-256 + AES-128-GCM per payload packet (+16 B;
  Windows CNG, hardware AES). Secure against spoofing, eavesdropping and tampering — **not** against
  an active MITM.

**Also** — keepalives and timeouts, per-peer RTT/loss stats, and a live link simulator
(`simPacketLoss` / `simLatencyMs` / `simJitterMs`).

## Abuse limits

All in `NetHostConfig`, and all enforced before any parsing or allocation.

* **Per-source-address token bucket** in a FIXED 512-slot hash table. A per-address map would itself
  be the memory attack. A foreign slot is only stolen once its owner has refilled — otherwise a
  flooder evicts active peers — and an UNUSED bucket must be claimable on the first packet.
* `maxPacketsPerUpdate` bounds one update globally.
* `maxPeersPerIp`.
* `maxQueuedReliablePerChannel` disconnects peers that never ack
  (`ENetDisconnectReason::Overflow`, latched and acted on in `update()` — never free a peer inside
  `send()`).

> This last one is a limit for peers that STOP ACKING, not a buffer for bulk data. A caller streaming
> many reliable messages (the Entity layer's spawn replay) must pace itself with
> `getQueuedReliable(peer, channel)` and stay well under it.

`NetHostStats::packetsDroppedPerSec` surfaces rate limiting in the server title bar.
