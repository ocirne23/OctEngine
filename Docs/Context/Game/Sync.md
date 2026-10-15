# Multiplayer sync

> Part of the Game documentation ([`Docs/Context/Game/CONTEXT.md`](CONTEXT.md)) — read that first.

## Multiplayer sync

The server runs ALL sim. Sync is three-layered.

#### 1 — Player-structure MIRROR over game events

**GPl / GRm** reliable on change, plus a join replay. The `mirror*` appliers in StructureSystem are
idempotent, notified through its `onStructure*` hooks.

* A completed blueprint **RE-SENDS GPl** through `onStructureBuilt`, and `mirrorPlace` on a known id
  applies the built flag idempotently. **Cable segments sit outside GSt, so this IS their build
  notification.**
* Their build PROGRESS mirrors through **GCb**, sent right after every GSt: blueprint cable and
  crossing segments only, `[u16 count]{u32 id, u8 healthFrac}` over the segment's own `healthMax`, up
  to 190 records with `m_cableSyncCursor` rotating past that.
* **There is NO cable wire at all** — links derive locally on both sides.

Plus **GSt at 5 Hz**: u8-quantized health / charge / fuel / minerals / outputFrac / flow-utilization /
powered for the first 79 NON-CABLE structures, plus resource totals. **Mirrored emitters drive their
LOCAL ForceComponent from the synced outputFrac**, so client-side fields are real and the client
player's shield readbacks work.

Other broadcasts: **GRt** routes, **GBu** barracks unit type, **GLt** strike beams (kind byte:
turret lightning / melee hit, then from + to), **GWv** wave
index, **GDm** damage flush, **GMp** map, **GPz** the shared pause state (u8; the client request is
**GqZ** — any seated player may pause or resume; see the escape menu in `Docs/Context/App/CONTEXT.md`).

#### 2 — Units and shots as network entities

`Component Network` in those prefabs plus `player.pre`.

#### 3 — Client capsule adoption

Each client ADOPTS its server-spawned capsule (`GamePlayer::clientAdopt`, LocalOwner scan) and
owner-simulates through the claim stream, computing shield and health locally.

### The game blob

Replaced the old GSh / GqE shield-mirror events.

**Every snapshot or claim record for an entity WITH a `GameUnitComponent` carries `NetRecFlag_Game`
plus 5 bytes**: healthFrac and energyFrac as u8, emitter output over a fixed 0..8 range,
materialsFrac, and flags = `collapsed | team<<4`.

* Server-side it is CHANGE-DETECTED and **forces the record out even when the pose alone would skip**
  (asleep body, unmoved entity); a collapse edge flushes on the next snapshot tick.
* Receivers write the entity's GameUnitComponent and drive its LOCAL ForceComponent output and team,
  **so remote bubbles render true drain/collapse state on the right team** and the overhead labels read
  components directly on every instance.
* **The entity's OWNER keeps its self-computed shield** (`applyShield` off) and takes only
  server-authoritative materials. **Claims apply shield-only** — never materials or team, which are
  client-forgeable.

### Player capsules are puppets

`Puppet true` in `player.pre`: `update()` is inert, GamePlayer stamps health / energy / collapsed /
team each tick and reads materials back as LocalOwner. **Puppets are excluded from turret targeting
and from `NpcSystem` clear/save.**

**Carried MATERIALS live on the twin**: `materialsFrac` IS the server-authoritative store — there is
no `m_clientMaterials` map, and the state dies with the capsule. `m_clientPlayers` is the only
per-client container left.

### Client intents

`Gq*` request events — **GqP** place (cables included), **GqD** demolish, **GqW** routes, **GqU** unit
type — gated by a `setEventFilter` (only `Gq*` ≤ 64 B from clients, installed by `spawnWorld`; during
the LOBBY phase the LobbySystem's `Lb*`-only filter stands instead) and validated by the same
authority seams as local input.

> **The C++ dispatch hook `NetworkManager::setOnGameEvent` is owned by MAIN.CPP**, installed ONCE and
> never replaced: it routes "Lb*" to the LobbySystem and everything else to
> `GameMatch::handleNetEvent`. **GameMatch must NOT install its own hook** — a lobby client's
> GameMatch is constructed INSIDE a dispatch, and reassigning the `oc::function` there would destroy
> the executing lambda.

`GameMatch::onClientJoined` / `onClientLeft` (registered from main when game + server) spawn and
remove per-client players and replay world state.

**Settings:** every Game tweak is a member of `Globals::settings.game` (`GameSettings`, module
`Settings.Game` in `Code/Settings`): `match` (GameMatch), `camera`, `player`, `structures`
(StructureSystem), `npc` (NpcSystem), plus the component baselines `unitParams` / `structureParams`
(`GameUnitComponent::params` / `GameStructureComponent::params` are references to them).
`Settings::registerGame` registers them ONCE at startup, so the values persist across matches. The
systems read the settings directly (`m_settings` reference members in GameMatch / StructureSystem);
a reaction to a change is a listener (`Tweak::onChange`) the system attaches and drops itself
(StructureSystem: the transport tick rate / substeps / spread re-dirty the links).

Gameplay tweaks sync automatically through the `Settings.Tweaks` `Synced` flag — all `Game/*` except
`Game/Camera` and `Game/Player/Detach camera (free fly)` / `Detach focus point`.

**Shadow preset:** the `GameMatch` ctor writes the top-down "Shadows" values into the renderer's live
tweak block through `Renderer::setShadowParams` (Max distance 250 m from the player, Split lambda 0.5,
Caster pad 500 m, both biases 0) and the dtor restores whatever the sandbox had. They remain editable
in the tweak panel during the match.

**No volumetric clouds in a match:** the ctor calls `Renderer::setCloudsSuppressed(true)` and the dtor
clears it. The clouds are a sandbox feature; the top-down camera sits under them.

**Off-screen casters** are not a game concern: the spatial `Shadow` pass (the view frustum swept
toward the sun by "Spatial/Culling/Shadow reach (m)") keeps walls and units outside the view casting
into it — see the Spatial CONTEXT.

**Scene focus:** `updateWindowed` calls `Renderer::setSceneFocus` every frame with the player's X/Z and
Y pinned at 1 m (a jump must not scroll the GI clipmap or slide the cascades), so
every distance-based quality falloff measures from the PLAYER: the sun cascades are nested spheres
around it ("Shadows/Max distance" is metres from the player) and the RTAO fade/early-out use the same
origin — the follow camera hanging in empty sky plays no part. With the camera detached the focus stays
on the player unless the local "Game/Player/Detach focus point" tweak is on too (`clearSceneFocus`, the
fly camera takes it); `~GameMatch` clears it. See "The scene focus" in the RendererVK CONTEXT.

**`Game/Player/Detach camera (free fly)`** (local, `GamePlayer::cameraDetached`): main runs the
testbed `FreeFlyCameraController` INSTEAD of the follow camera (seeded from the follow view on the
flip via `setPose`, WASD handed over), and `updateWindowed` skips `GameCamera::apply` and the whole
input half (`updateGameInput`: grid hotkeys, hotbar/popup clicks, mode clicks, the RMB move order)
— LMB-look and WASD belong to the fly camera. The debug draws, labels and HUD keep running; the
capsule keeps simulating (a standing move order still completes) but takes no new orders.
