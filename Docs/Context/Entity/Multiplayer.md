# Multiplayer

> Part of the Entity documentation ([`Docs/Context/Entity/CONTEXT.md`](CONTEXT.md)) — read that first.

`Entity:NetworkManager` (`Globals::networkManager`) plus `NetworkComponent` (ID 8). The full wire format is the
comment at the top of `NetworkManager.ixx`.

## The ownership model

**Every networked entity is SERVER-OWNED, and the server mints every netId.**

A component registering on the server — scene load or runtime alike — gets an id plus a spawn record
replicated to clients. **Late joiners get every live record replayed after their Welcome, and that
replay IS how the networked world arrives.**

A client registering OUTSIDE an incoming Spawn stays **LOCAL-INERT** (netId 0, never synced). So
client-only and server-only entities are just entities the other side does not have, **the two scenes
can differ arbitrarily, and id conflicts are structurally impossible.**

> **Only STATE is networked.** Static scenery that never moves needs no NetworkComponent.

`NetworkComponent::authority()` derives Local / ServerOwned / LocalOwner / RemoteOwner (`ENetAuthority`,
worker-safe). `localClientId()` is 0 on the server and before the Welcome, **which is what makes
`ownerClientId == localClientId()` false everywhere except on the actual owner.**

## State layout

The inline component is **identity only** — `netId`, `ownerClientId`, and a `unique_ptr<NetEntityState>`.
All sync state lives in `NetEntityState`, allocated only inside a session.

**Role-exclusive state is a UNION of `ServerState` / `ClientState`** — a process is one role for life,
and both are trivially destructible (static_asserted). Only `input`, `transferredOwnership` and `sleepDirty` are
dual-role. **Cross-role reads are bugs.**

The manager never null-checks `state`; the component's own `update()` and outside readers
(EntityEditor, InputControls) must.

## Roles and the thread contract

```
--server [--port N] [--headless] [--tickrate N]
--connect <ip[:port]>          --no-encrypt
(no flags)  = single player (ENetRole::None; fireNetworkEvent degrades to a local fireEvent)
```

The "Network" tweaks are `NetworkSettings` = `Globals::settings.network` (`Settings.Network`, with
`NetSyncParams` as its `correction` member), registered in `Code/Settings` for every role.
NetworkManager.cpp binds them as file-scope `s_*` references. `initialize()` attaches the link-simulation
listeners ("Network/Link sim"), which copy `simPacketLoss` / `simLatencyMs` / `simJitterMs` into the NetHost
config (also after every `open()`, which resets that config). Hard cap `MaxClients` 32.

**Thread contract:** every NetHost call is main-thread — `receive()` before the entity and physics
updates, `send()` after `world.update`. Snapshot decode writes component target state on the main
thread too, **so `NetworkComponent::update` only ever reads it from workers.** Client physics
corrections go through the body-command queue.

## Headless server

`headlessServer` branching inside main() — **one init sequence and ONE main loop for all modes.** No
window, renderer, UI or input.

**Loop:** entity-change drains → receive → scriptContext → physics → `commitFrame` → game update → contacts →
`world.update` → send → Sleep-based tick limiter (`--tickrate`). Ctrl+C is a clean shutdown; there is a 5 s status
line.

> Component thunks do NOT null-check their handle. **`//@@require`'s live `requirementsMet` gate is
> the guarantee** that a script never reaches an absent component, so a script using a stripped
> component simply never runs its entry points headless. Cross-entity thunks still
> `getComponent`-check, and `Renderer::isInitialized()` gates the few thunks that touch renderer
> state.

**Deliberately skipped headless:** terrain, ocean, trees, rocks, ground clutter and the terrain collider
(camera-centered streaming — the test scene carries its own ground), nav, buoyancy, and audio. Spatial entries DO
register.

## The wire protocol

`GameProtocolId` (`0x4F43534B`) bumps on ANY format change — **the transport handshake denies
mismatched ids, which IS the version gate.** `GameNetVersion` (15) rides in Hello/Welcome.
[NetworkManager.cpp:15](../../../Code/Entity/Private/NetworkManager.cpp#L15). NetFuzz mirrors both by hand.

| Channel | Messages |
|---|---|
| **ch0 Unreliable** | Snapshot |
| **ch1 Reliable** | Event |
| **ch2 Reliable** | Hello / Welcome / Deny / Spawn / Despawn / OwnerChange |
| **ch3 Unreliable** | Claim |

**Spawn and Despawn ride the session channel so they order after Welcome.**

**Event** carries `senderClientId` **server→client ONLY**: a client sends no client id, because *its
identity is the connection, and an ignored client-writable one is a refactor away from being
trusted*. The NET id names the firing entity and travels both ways; being client-supplied, the server
resolves it only after an ownership check (`findOwnedEntity`).

## Snapshots (server → clients, per peer)

Records carry the entity's LOCAL pos/rot, or **the BODY's world pose** when `NetRecFlag_Physics`.
Rotation is smallest-three quantized behind a live `Quantize` toggle; velocities are 3×u16 over the live
"Max vel (quantize m/s)" / "Max ang vel (quantize rad/s)" ranges (50 each). **Those ranges are live tweaks, so
every message states them** and the decoder is told per message.

Chunked under `Snapshot max bytes` (1105 — static_asserted to the transport's single-packet budget minus 64:
an oversized UNRELIABLE message is dropped); **every chunk repeats the tick and staleness is dropped PER ENTITY**,
so chunk reordering is harmless — UnreliableSequenced would drop sibling chunks instead.

### Two phases per tick

**1 — OBSERVE** (`observeSnapshotTick`, every entity once). Sample the wire record — pose, velocities,
flags, game blob, with the claim passthrough advancing here — into `ServerState::obs*` and stamp
`changedTick` on any difference.

* An AWAKE body changes every tick; a sleeping one only on the sleep edge or a blob/flag change; a
  non-physics entity when it moved past the epsilon tweaks (`Send pos epsilon` 0.001,
  `Send rot epsilon (deg)` 0.1).
* It also rebuilds the flat `m_tickEntities` array and the players' positions by clientId.

**2 — SEND** (`sendSnapshotTo`, per ready peer). Each peer has a SLOT, and every entity is TIERED by
distance **to THAT peer's own player(s)** ("Network/Relevance"):

| Tier | Radius | Cadence |
|---|---|---|
| NEAR | < `Near radius` 40 | every tick |
| MID | < `Mid radius` 100 | every `Mid every ticks` 2 |
| FAR | beyond | every `Far every ticks` 4 |

A peer with no player yet sees everything as far.

An entity is DUE when `changedTick > sentTick[slot]` and its tier's cadence is up, **or on its
KEYFRAME rotation** (`serverTick % K == netId % K`): near/mid `Keyframe every ticks` 20, far
`Far keyframe every ticks` 200, and a SLEEPING body at least `Asleep keyframe every ticks` 100.
**Keyframes are the loss repair — nothing else re-sends an unchanged entity.**

Each tier is walked round-robin from the peer's own cursor, near first under `Max entities per tick`
300, with far additionally capped by `Far max per tick` 100. `Snapshot Hz` is 20 — **matching the
physics fixed step.**

Far units DO move (the Game's far tick walks a wave in from the spawn ring); the client only needs
them roughly right until they cross its player's radius.

### The client's unselected-root path

An UNSELECTED root — beyond every player's outer SIM LOD radius, so never visited and
`NetworkComponent::update` never runs — gets its snapshot pose applied **DIRECTLY in `handleSnapshot`** (main
thread, pre-pass), under the teleport contract (body teleport + `snapPose`, entity pose, `placeSpatialEntry`).
Never for our own claim-driven entities.

> The LOD selects by the entity's LOCAL position, so without this a far unit sits at its spawn point forever
> while the server's walks into range.

## The spawn stream

**Spawn and Despawn are NEVER sent directly.** Every ready peer has a `PeerStream` of OWED baseIds —
the join replay queued at Hello, plus each frame's announces and despawns.

`drainSpawnStreams()` packs them **many per message** (`[u16 count]` + records, one message per
reliable window slot), only while `getQueuedReliable(peer, ch2)` is under "Network/Spawn stream/Queue target" 64
and at most "Records per frame" 128 per peer per frame.

> The transport DISCONNECTS a peer whose reliable queue passes 1024 (`maxQueuedReliablePerChannel`); a wave or a
> 20k-record join replay sent unthrottled hits that at once.

Ids never recycle, **so an owed spawn whose record is gone was despawned unsent and is skipped**;
despawns drain before spawns, so a despawn for an unsent spawn is ignored client-side and a spawn can
never follow its own despawn. Ownership that diverged from the record rides as OwnerChange right
behind the batch. The server status text shows `spawn backlog N`.

**The Spawn message carries the root body's live velocities**, sampled at send — i.e. AFTER same-frame
launch impulses — and the receiver caps and applies them, **so a replicated projectile's local twin
flies instead of dropping from rest.** Corrections near the receiver's own capsule are
grace-suppressed, so that initial velocity is the only thing moving it there.

Clients adopt server ids **in tree-DFS order**. `setOwner(root, clientId)` must run in the spawn's own
frame, before `send()` announces; later transfers use OwnerChange. **Never store NetPeerIds on
entities** — they recycle; clients get a stable `clientId` at Hello/Welcome.

An occupied id is REPLACED with a warning (the Entity Editor respawns before destroying, so a
collision there is the stale twin), and **unregister erases only when the component pointer still
matches** — otherwise that stale twin's destroy would take the new registration down with it.
Despawn is all-or-nothing at the root.

## Claims (owner → server)

The owner simulates freely and streams input plus claimed pose and velocities, **PHASE-LOCKED to the
physics step**: `send()` fires them only on frames where `getStepCount()` changed.

"Network/Player/Max update Hz" (20) is only a thinning limit, counted in **WHOLE STEPS**
(`stepCount - m_lastClaimStep >= round(stepHz / maxUpdateHz)`), **never against the wall clock**: a step boundary
falls partway through a frame, so a time comparison at the step rate measures just-under-interval and drops that
step's claim, which the passthrough extrapolates over and double-jumps. Paused physics = no claims, correct.

Each packet carries the last "Claim redundancy" claims (default 4, ring cap 8) — **redundancy beats a reliable
channel under loss: no head-of-line stall** — and the server dedups by seq. **The record count comes
from `validCount`, never seq**, so a re-acquired entity cannot replay a previous owner's poses.

One trailing `look` vector applies to every record in the window, since it barely changes across it.
Claims follow the sleep policy: every awake tick, one rest-pose claim on the sleep edge, then silence.

## Validation ("Network/Validation")

* A movement token bucket in metres, refilled at `Max speed` (60) per second of **SERVER WALL
  CLOCK** — never per claim, since the packet rate is attacker-controlled.
* `Max velocity` / `Max ang velocity` (50 each) on **EVERY accept path**.
* A trajectory raycast (`Path raycast`; `castRayClosest` with `ignoreBody` + `staticOnly` — see Physics).
* A hard `Teleport cap` (10 m) regardless of elapsed time.
* The first claim seeds the anchor but is still checked.
* **`Re-anchor radius` (2 m): a claim this close to the twin's CURRENT state ALWAYS accepts.**
  That is the guaranteed recovery from a rejection spiral.
* Every float off the wire is finite-checked and quaternions re-normalized — **NaN passes comparisons
  and poisons the solver.**

**Accepted** → the twin FOLLOWS the claim through the solver ("Network/Twin follow gain" 10, a bounded
`NudgeVelocity` = claimed velocity plus a corrective velocity toward the claimed pose); a hard
teleport only past "Twin resync (m)" 2.

> It is deliberately NOT teleport-pinned per claim. That collapses the render interpolation (prev and curr both
> become the claim pose) and, since claims are sampled on the OWNER's step clock while the server steps on its
> own, **yanks the twin backwards whenever two server steps fall between two claims** — pulsing on the SERVER's own
> view. Other clients are unaffected either way: the passthrough sends them the owner's stream, not the twin.

`lastAcceptedClaimPos` anchors the next budget and the re-emit through **CLAIM PASSTHROUGH**: the
snapshot re-emits the newest accepted claim state instead of sampling the wobbling twin, extrapolating
by claim velocity for a bounded gap. "Owner predict (ticks)" (0.5) adds a constant forward shift.

**Rejected** → `violations`++ plus `NetRecFlag_Forced` for `Forced ticks` (30). The owner ignores
corrections UNLESS Forced, an accepted claim clears it immediately, and `updatePlayerControl` yields
input while Forced — **steering against the correction is a tug-of-war.**

## Client correction (`NetSyncParams`, "Network/Correction")

**The DEFAULT for dynamic bodies is the PHYSICAL PUSH**: error × gain becomes corrective velocity on
top of the server's, delivered as bounded impulses, **so contacts and gameplay impulses COMPOSE.** No
teleport on this path, and bodies still sleep inside the deadzones.

| Band | Behaviour |
|---|---|
| Inside `Pos deadzone` (0.05) / `Rot deadzone (deg)` (0.5) | Local state free-runs — **but the velocity still settles onto the server's** (a NudgeVelocity toward the target velocities whenever they differ, never for a body already matching, so rest stays asleep). |
| Past `Pos snap threshold` (2 m) / `Rot snap threshold (deg)` (45) | CATCH-UP: gains and caps × `Push catch-up boost` 4, **still through the sim** — teleporting into an occupied space would depenetration-fling both bodies into fresh desync. |
| Past `Pos teleport threshold` (10 m) | The hard `teleportBody` resync — **position error only**, since a wrong orientation cannot materialize inside anything. |

> Why the deadzone still settles velocity: the push leaves its corrective term in the body, and a
> **FRICTIONLESS body (units) keeps it forever**, coasting out the far side of the deadzone and back.

**MASS SCALING.** Gains, velocity caps and acceleration limits — the deadzone settle included —
multiply by `clamp(mass / Push mass reference (kg) 30, Push mass scale min 0.15, 1)`.

> A light body's velocity answers every contact impulse strongly, so in a packed crowd its correction
> and its neighbours' pushes compound into swinging (the swarm). Bodies at or above the reference
> correct at full strength.

Gains: `Push pos gain` 0.5 /s per metre, `Push rot gain` 5 /s per radian, `Push max vel` 10 m/s,
`Push max ang vel` 10 rad/s, `Push accel limit` 10 m/s² (**keep it above gravity**),
`Push ang accel limit` 60 rad/s².

**NON-physics entities** blend the entity transform at `Blend rate` 10, with optional `Extrapolate`
(dead-reckon by `linVel × timeSinceSnapshot`).

**INTERACTION GRACE** (`Interaction radius` 1.5, `Interaction linger` 0.5 s): a server-owned body near one of OUR
claim-driven bodies suspends its corrections — **they would fight the shove the player is applying
with the server's RTT-old pre-push state.** The twin gets the same push an RTT later. Proxy positions
are published in `receive()` (`localOwnedBodyPositions`) and read from workers.

**ARBITRATED OWNER** (player-vs-player contact) uses `Arbitrate deadzone` 0.4 and halved gains:
*the local feel comes from the local contact with the opponent's replica, so the server correction
should only reconcile REAL divergence* — a tight deadzone would micro-correct the pipeline lag and
drag against the player's input for the whole window.

## Remote-owned entities

Other players skip the chase entirely and use **BUFFERED SNAPSHOT INTERPOLATION**: a `NetSnapshotRing`
per entity (manager-owned, main-write / worker-read) replays the owner's recorded trajectory at
`newestTick − Remote interp (ticks)` **in the SERVER's tick units** (`serverSnapshotHz` comes from the
Welcome).

> Minimum and default 2 — **the cursor clamps to `newest - 1`, so 1 leaves no headroom and playback
> advances only on snapshot arrival. Buy smoothness with snapshot RATE, not depth.** Loss gaps fall
> through to the push for a frame.

Teleport-following with matched velocities and prev/curr stomps. Asleep records hard-sync once then
queue `SetAwake 0`. Kinematic and static bodies use the entity-transform path.

## The teleport contract

**EVERY teleport must also call `PhysicsComponent::snapPose(entity, pos[, rot])`.**

The component stores no pose: the body IS the pose. But a teleport is queued and applies at the next
`physics.update`, so `body.getPosition()` still holds the OLD pose (for network playback: LAST
frame's teleport) when `PhysicsComponent::update` runs. `snapPose` writes the world pose into the
entity at once and sets `poseHeld`: the next update leaves the entity pose alone. Without it the
entity shows the stale body pose for a frame — **for a body that teleports every frame that is visible
pulsing.** A teleport outside the visit also re-places the spatial entry (`Entity::placeSpatialEntry`).

## Transfer, steal and arbitration ("Network/Ownership")

`Transfer enabled` is the kill switch, mirrored by `setOwnershipTransfers`.

* Awake server-owned dynamics within `Transfer radius` (2.5) of a client's PRIMARY hand over through a
  per-entity reliable OwnerChange; the server resets that entity's claim and validation state. They
  revert after `Release delay` (1 s) outside `Release radius` (4).
* **Primaries only as SOURCES** — chained transfer through a pushed cube would deadlock release on
  piles; the client-side interaction grace covers second-order contact instead.
* **The SERVER's own player body registers as a primary too** (`setServerPrimary`, clientId 0; GameMatch calls
  it). It is never handed to a client, and it RE-CLAIMS transferred objects by proximity — only ever transferred
  ones, never a client's primary. Its snapshot records carry `NetRecFlag_ServerPlayer`, so observers
  route it into the remote-player INTERP ring and **skip the interaction grace**: it is an
  actively-steered body, and grace-freeing it for local shoves only fabricates divergence.
* **Contact STEALING** (`stealOwnershipOnContact`, wired as the primary's `onContact` in the App's
  `Session.ixx`; needs `ContactEvents true`): **last collider owns**, even inside the previous owner's transfer
  bubble — but never another client's PRIMARY.
* **Primary-vs-primary contact ARBITRATES** both for `Arbitrate window` (1 s, refreshed per contact):
  twins go solver-owned, claims apply as bounded NudgeVelocity, records carry `NetRecFlag_Arbitrated`,
  and input keeps steering. An object touched by two DISTINCT clients within `Contest window` (1.5 s)
  is **CONTESTED → server-owned** until the window decays.
* Late joiners get OwnerChange replays after the Spawn replay; a disconnect reverts transfers before
  tearing down primaries.

> #### The transfer pass NEVER walks `m_entities`
>
> On a co-op map every unit is a networked entity, so a per-frame walk is thousands of map nodes
> and body reads. `updateOwnershipTransfers` instead works from three small sets: the tracked
> **primaries** (`m_primaryIds`, every id `setOwner` / `setServerPrimary` touched — the sources),
> a **SpatialIndex sphere query** of `Transfer radius` around each client primary (the hand-over
> candidates; `send()` runs after the frame's join, where read-only queries are legal), and the
> tracked **transferred objects** (`m_transferredIds`, maintained by `transferOwnership` — the
> reclaim / release candidates). `unregisterEntity` prunes both lists; the disconnect revert and the
> arbitration of the toucher's primaries use the same lists. Contest history is time-STAMPED
> (`contestTimes`, net time), not aged per frame. **It also returns at once with no client
> connected.** Consequence: an entity with no spatial entry (`CullMode None`, or covered by a RootOnly root) is
> never a hand-over candidate.

`setOnClientJoined` / `setOnClientLeft` (App `Session.ixx`; the testbed spawns and tears down
`Entities/Debug/netPlayerCapsule.pre` per client). **`joined` runs AFTER the Welcome and world replay are queued**,
so anything spawned inside arrives in the same session stream.

`setOnServerLost` fires on the main thread inside `receive()`, **BEFORE the automatic reconnect
attempt**. The App decides what a lost server means. **Do not shut the host down from inside it** —
`receive()` is still walking its events.

## Events

`fireNetworkEvent(name, data, sender)` = a local fire plus Reliable ch1; the server relays to other
clients. Role None = local fire only.

* **Callable from job workers** (script thunks tick in the parallel pass): the network half queues
  under a mutex and `send()` drains it on the main thread, since NetHost is single-threaded. The
  payload is copied.
* `MaxEventDataBytes` 1024. The whole max-size event message (`MaxEventMessageBytes`, with the longest name) is
  **static_asserted against the transport's `netMaxSinglePacketMessage()`**, so it still rides in ONE packet.
* `currentEventSender()` / `currentEventData()` are readable during dispatch. **The sender clientId is
  server-stamped from the receiving peer**, so a client cannot claim to be someone else. The data is a
  view into the receive buffer — copy anything you keep.
* **`setEventFilter`** gates client-originated events: false drops it, neither fired nor relayed.
  `sender` is non-null ONLY when that client really owns the supplied netId. Server-originated events
  bypass. **No filter = everything allowed, so install one before any event grants state** (GameMatch and the
  Lobby do).
* **`setOnGameEvent`** is the C++ listener for every dispatched event, local and received — the
  counterpart of the script listeners. It runs synchronously inside the dispatch. **Main thread for
  received events, but a locally fired event dispatches on the FIRING thread** (worker script thunks
  included), so keep the handler cheap and thread-tolerant.

## Session

The 4-way transport handshake gates on `GameProtocolId`, then Hello → Welcome on ch2 marks the peer
ready. **Per-peer state MUST clear on Disconnected.**

**Events (ch1) have NO ordering against the ch2 Welcome**, so a client PARKS events that arrive before
its Welcome is processed (`m_preWelcomeEvents`, at most 256, replayed in order right after).

> The server's join-time burst — game world replay, tweak sync — is fired in the same frame as the
> Welcome; without the parking it is **silently lost** (already acked = never resent). **Anything
> join-critical sent as an event relies on this parking.**

Encryption is on by default at this layer (`NetHostConfig::encrypt`'s own default is false). It
**cannot change on a live host**, so call `NetworkManager::setEncryption()` before start; `--no-encrypt` drives
it, and both ends must match or the handshake denies — a clean failure. Without it a peer is identified only
by source address, trivially forged on a LAN.

## Testing

Server key U throws `Entities/Debug/netPhysCube.pre`; client key C toggles player control; key K fires
"NetPing". Demo `Entities/Debug/networkTest.pre` spawns at a **FIXED transform — never
camera-relative, since both worlds must match.** Link-sim tweaks bind into `m_host.config()`, and the
window title shows role plus RTT / loss / KB/s (`getStatusText`).

## Player control

Key C on a client, `InputControls::updatePlayerControl`. WASD and Space velocity-steer every
locally-owned dynamic body camera-relative ("Network/Player" tweaks — **keep Move speed under
Validation's `Max speed`**), with the jump gated on a downward raycast and fly-camera movement
released while active.

The owning client simulates its body directly — main thread, before `physics.update`, where direct
setters are sanctioned — writes intent into `comp->input`, and the claim stream carries the result.
