# NetFuzz

> Documentation for `Code/NetFuzz`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

A console executable (Core + Network only; VS target `run-netfuzz`). The protocol fuzzer, and **the
regression gate for wire-format changes — run it after ANY change to the wire.**

**A crash is the finding.** Every mode prints its seed, and the default seed is fixed (`0x1234`, not
time-based), so a failure reproduces exactly.

```
NetFuzz reader [iterations] [seed]      default 200000
NetFuzz host   [iterations] [seed]      default 20000
NetFuzz game   <ip[:port]> [iter] [seed]  default 5000, port 27888
NetFuzz all    [iterations] [seed]      reader + host
```

## The corpus

Pure noise rarely reaches deep parser states, so `fillFuzzed` corrupts **mostly-valid** data:

* Random bytes, then with 40 % probability it plants **a varint that decodes to ~2^64** — random
  bytes hit the length-vs-capacity arithmetic far too rarely, and that arithmetic is exactly where a
  wrapped bounds check hides.
* Each mode then steers specific bytes toward valid values (below), because otherwise everything is
  rejected in one line.

## `reader` — `NetReader` primitives

Drives a random sequence of `read<T>` / `readVarUInt` / `readVarInt` / `readQuantized` / `readString`
/ `readBytes` against a fuzzed buffer until the reader gives up.

**The invariant:** any view `NetReader` returns must lie **inside the input buffer**, and the cursor
must never pass the end. Checking it here catches a wrapped bounds check at the source, rather than
at a later out-of-bounds dereference somewhere else.

## `host` — raw packets at an in-process `NetHost`

Uses `protocolId` "FUZZ" so it is isolated from a real server on the same machine. Four stages:

1. **Encrypted smoke test.** Two real hosts complete the ECDH handshake and exchange a sealed
   payload. Needed because the fuzz phases run plaintext — forging sealed packets requires the key —
   so without this the encrypted path would go untested here entirely.
2. **Cold-start regression.** A brand-new address must connect on its FIRST packet at the DEFAULT
   limits. This is what catches a rate limiter that refuses to hand out an unused bucket.
3. **Pre-auth phase** (half the iterations). Everything an unauthenticated sender can reach. 60 % of
   packets get a real packet type byte and the real protocol id planted in them.
4. **Authenticated phase** (the other half). A real handshake first — the payload and message parsers
   only run for a CONNECTED peer — then fuzzed payload bodies where 70 % get a valid message
   kind + channel in the first byte, so the parser walks into the per-kind bodies (length varints,
   fragment indices) instead of bailing on an unknown kind.

## `game` — hostile client vs a live server

**The only mode that reaches NetworkManager's handlers.** It needs a real server:

```bash
App.exe --server --headless --no-encrypt
```

`--no-encrypt` is required: this mode speaks the handshake by hand and does not implement ECDH, and
an encrypted server denies a plaintext peer. The parsers behind it are identical either way.

It sends `ENetMsg::Hello` first so the server mints a client id and the owner-gated handlers (Claim)
are reachable at all, then fuzzed game messages across 4 channels, half reliable.

> **Two constants are mirrored by hand from
> [NetworkManager.cpp:14](../Entity/Private/NetworkManager.cpp#L14):** `GameProtocolId`
> (`0x4F43534B`) and `GameNetVersion` (15). Bumping either — which any wire change must do — is
> exactly the signal to re-run this mode, and to update
> [NetFuzz/main.cpp:389](main.cpp#L389) and [main.cpp:408](main.cpp#L408).

## Harness constraints

Break any of these and the fuzzer silently stops testing anything:

| Constraint | Why |
|---|---|
| **Pump the in-process server between handshake polls.** | It has no thread of its own; `handshake()` takes a `pump` callback for exactly this. |
| **Drain the socket before a handshake.** | After a fuzz phase the queue holds thousands of replies to earlier garbage — random packets do parse as valid ConnectRequests — which would eat the poll budget. |
| **Raise the rate limits for the fuzz phases.** | Engaged, they discard the corpus at the door and the run becomes a test of the limiter instead of the parsers behind it. `host` sets them to 100000000 after stage 2; `game` instead sleeps every 32 messages to stay under the real 400/s. |
| **Advance the packet `seq`.** | Otherwise the receiver dedups the packet and the fuzzed bytes are never parsed. Reliable messages must also be in-order, or the receive window drops them. |
| **Drain `takeEvents()` periodically.** | A long run otherwise just accumulates delivered garbage. |

The packet-type and message-kind constants in `namespace Pkt` **mirror Reliable.cpp's private
constants on purpose**: sharing the sender's helpers could only ever produce well-formed packets.
