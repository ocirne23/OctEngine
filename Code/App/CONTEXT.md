# App

> Documentation for `Code/App` — the testbed executable: `main.cpp` (ONLY `main()`: the init
> sequence, the timers, the frame loop), `App.Session`, `InputControls`, `App.UnattendedRun`. (The
> lobby and chat MODELS, `LobbySystem` / `ChatSystem`, are `Game:Lobby` /
> `Game:Chat` — plain stack locals in main, which services them; the profile report writer is
> `Profiler::writeReport` with main's injected file writer.)
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction.
>
> The frame-loop ORDERING rules (kick/join windows, post-update jobs) are documented in the owning
> libraries: RendererVK, Spatial, Threading, UI, Entity.

App links everything, plus Game and AppScripts.

---

# The frame loop

ONE loop and ONE init sequence for every mode; `headlessServer` branches inside it. In order
([main.cpp:542](main.cpp#L542) onward):

| # | Step | Notes |
|---|---|---|
| 1 | **`Time::beginFrame(...)`** | The whole frame boundary: fence wait, frame-rate limit, event-pump kick, next frame's clock. **BEFORE the "main loop" scope opens and before input is sampled** — see RendererVK. Headless calls plain `Time::update()`. |
| 2 | `ProfileScope("main loop")` opens | |
| 3 | **`joinPostUpdateJobs()`** (the FRAME batch) | Last frame's widget pass only. The SIM batch — Nav field steps, the game's nav feed (gather + the Nav setters) + ambient wander — joins later, at row 10, so it also overlaps input, prepare and the camera. Windowed and headless alike. |
| 4 | `ui.flushMainThreadWork()` + `renderer.updateImGuiTextures()` | Deferred tweak callbacks, container imports, and the glyphs the pass baked — **the ImGui context is quiescent from the join until `ui.update()`.** Also promotes the pass's PENDING draw-data snapshot to the one present records (never earlier: the pass can finish before present). |
| 5 | `forceSystem.joinMerge()` | Last frame's merge job — **before input or drains can touch emitters.** |
| 6 | Menu / lobby / chat servicing | Only while the main menu is active. See below. |
| 7 | `TweakRegistry::update(dt)` | Saved/Synced change detection. |
| 8 | `input.update` → **`ui.prepare()`** → `controls.update` | The panel prepare jobs overlap everything down to `ui.update`. |
| 9 | Escape menu | |
| 10 | **`joinPostUpdateJobs(Sim)`**, then camera: `game->updateWindowed` (fly camera first when `game->cameraDetached()` — the "Game/Player/Detach camera" tweak, seeded via `setPose` on the flip) **or** VR **or** fly camera + `applyPlayerCamera` | The Sim join sits before the frame's first main-thread write to units/rosters (unit orders in the windowed tick, the entity-change drains at row 11); headless joins it before the script drain instead. |
| 11 | Script reload requests, then the UI and script `EntityChange` drains | |
| 12 | `simDeltaSec = time.getSimDeltaSec()` | **Read HERE, after the input dispatch and tweak poll, so a pause toggle applies to this very frame.** |
| 13 | `networkManager.receive(dt)` | Snapshot targets and events land before the sim reads them. |
| 14 | `game->updatePlayer(simDt)` | ONLY the player-body writes (pre-physics). |
| 15 | `scriptContext.update(...)` | |
| 16 | **`world.joinSelection()`**, then **KICKS: `getCullView` → `spatialIndex.kickUpdateJob` → `renderer.kickBeginFrameJob`** | The join: last frame's SIM LOD selection query (fired at the end of `world.update`, it had the whole frame) must be done before the commit inside the spatial kick — normally a no-op. Kicks under a `"Frame kicks"` scope, **which attributes the submit + wake cost that used to read as a gap.** |
| 17 | `physics.update(simDt)` | ≤ 1 step; contact events stay buffered. |
| 18 | `audio.update` → **`joinBeginFrameJob` → `joinUpdateJob`** | Headless instead calls `spatialIndex.commitFrame()`. |
| 19 | `game->update(simDt)` **or** `world.setSimLodFocus(&camera.position, 1)` | The rest of the game tick; also publishes the SIM LOD focus. |
| 20 | `physics.dispatchContactEvents(...)` | **AFTER the joins** — contact scripts query the index and can touch renderer state. |
| 21 | **`world.update(renderer, simDt)`** | The parallel entity pass. |
| 22 | `networkManager.send(dt)` | Server: snapshot entities at their POST-update poses. |
| 23 | terrain → collider → ocean → scatter → particles → force → **`renderer.kickGridBuilds()`** | The force update is the frame's LAST light source and the last writer of the force emitter slots, so the light grid's merge job and the force compaction + grid build job kick right after it (see RendererVK: the per-light work already ran inline in every `addLightInfo`). **Any new light source or emitter writer must run before this kick.** |
| 24 | `ui.drawGizmoEntity` → `game->joinWorldLabels()` → **`ui.update(...)`** | The game's world-labels job (kicked at the end of its tick, row 19, overlapping the entity pass) feeds the widget pass's HUD overlay, so it joins right before the pass is queued. |
| 25 | **`navSystem.update`** → **`kickPostUpdateJobs()` → `terrain.joinRender()` → `ocean.joinRender()` → `renderer.present()`** | The nav publish + its Low-priority build kicks go LAST before the post-update kick, so the build slices run through present and the fence wait instead of next to the cull chunks (they used to kick at row 18, right after the spatial kick, and delay it). It also queues the field-steps job that rides this kick. A finished build publishes one frame later. Headless kicks too — **an unkicked queue only fills up.** `terrain.joinRender()` / `ocean.joinRender()` join the streamer's chunk render-push job and the ocean's sector job (kicked in their `update`s) so their `renderNode` pushes, and the ocean's wave-extent / camera-surface stores, land before present. |
| 26 | `mainLoopScope.stop()` → `profiler.endFrame()` | **The scope stops BEFORE the frame mark, so the record stays inside this frame's window.** |
| 27 | Headless: Sleep-based tick limiter | |

After the loop: `joinPostUpdateJobs()` and `forceSystem.joinMerge()` for the final in-flight batches.

> **The quiescent window (16 → 18) is the whole point.** The drains, net receive and `game.updatePlayer`
> above it are the last registers and container loads, and `physics.update` no longer fires contact
> scripts — so the cull and begin-frame jobs overlap the physics step and audio.
> **Nothing added between the kicks and the joins may touch the spatial index or renderer frame
> state.**

## Init order

Window (**spawns the WINDOW THREAD**) → `input` → **`jobSystem`** → `registerExternalHelper` +
`setIdleWork` on the window thread → camera → renderer → UI → world (+ `setHeadless` **BEFORE any
spawn**) → physics → audio → spatialIndex → occlusionBuffer → particleSystem + forceSystem (**before
any world spawn** — components register on spawn) → networkManager → scriptEvents +
`registerScriptDslBindings` → terrain / terrainCollider / scatter / ocean → the ocean↔terrain wiring
and `physics.setWaterSurface`.

---

# Command line

| Flag | Effect |
|---|---|
| `--game` | The whitebox game instead of the testbed scene. **Refused with `--headless`** — GPU field readbacks drive the authority sim. |
| `--coop` | PvE. **Needs `--game`**; clients pass it too, since the layout is local. |
| `--server` / `--connect <ip>` / `--port N` / `--tickrate N` / `--headless` / `--no-encrypt` | |
| `--scenario <save\|default>` / `--scenario-at <sec>` | |
| `--profile-after <sec>` / `--profile-frames W` / `--profile-out path` / `--profile-workers` | |
| `--quit-after <sec>` / `--no-vsync` | `--no-vsync` is just `setOverride("Time/VSync=0")`. **Either unattended flag also installs `App.UnattendedRun`'s failure handling**: no modal dialogs (assert / abort / OS fault box — the run FAILS instead of hanging on a button), assert text to stderr, and an unhandled-exception filter that prints the faulting thread's PDB-symbolized stack (file:line) to stderr. Interactive runs keep the dialogs and the debugger break. |
| `--tweak "Cat/Name=v"` / `--tweaks <file>` | |

**The MAIN MENU boots when none of these apply**: no mode flags, not a client or server, not
`--game`, not an unattended run (`--profile-after` or `--quit-after`), and no `--scenario`.

---

# Main menu

`UI:MainMenu`, a fullscreen start screen. **The engine runs fully underneath** — renderer, UI, world,
terrain as background — and nothing mode-specific starts until a selection arrives.

Contents: co-op / PvP / sandbox buttons; a **"Host a server" checkbox** or an address field
(`ip[:port]`, empty = offline); Quit; and a **SETTINGS page built from the tweak registry** — curated
per-section category-prefix tables in MainMenu.cpp, with dev branches (Debug / Stress / Stats / Cheats
/ Emitter / V3, plus Time/Paused) filtered out, rows drawn by the exported `drawTweakVar` shared with
the TweakPanel, and `onChange` deferred through the TweakPanel's list.

**The host endpoint display** is seeded with `netGetLocalAddress()` + the launch port, then upgraded to
the EXTERNAL IP once `netGetExternalAddress()` lands — **run on a detached thread with a shared_ptr
result block, polled in the loop's menu block.** The LAN address then moves to a dim note line; a
failure keeps the LAN address with a note.

**Flow:** the menu only RECORDS a `MainMenuAction`. main polls `takeMainMenuAction()` **after the
post-update join** and runs the start in the **pre-kick window**, where main-thread spawns and the
network start are legal, **under a one-shot `AllowMainThreadIO`** (the F10 pattern).

A failed host or join leaves the menu up with the reason in the log. During the menu phase the testbed
keys are muted (`setGameMode(true)`) and free flight is paused; the mode start restores both.

**The mode flow lives in `App.Session`** (`Session.ixx`, which also holds `LaunchOptions` /
`parseCommandLine`, `installFileHooks` and the `g_running` flag) — a stack local in `main()` (it owns the
`oc::optional<GameMatch>`, the `LobbySystem` and the `ChatSystem`, so it must stay one: see the
`GameMatch` rule in [`Code/Game/CONTEXT.md`](../Game/CONTEXT.md)). Its methods are the phases the
loop calls in order: `serviceServerLost` / `serviceMainMenu` / `serviceChat` (menu servicing, row 6),
`serviceEscapeMenu` and `updateCamera` (rows 9–10), plus the network event dispatch it installs
(`installNetworkCallbacks`). `main()` itself keeps the command line (`parseCommandLine` →
`LaunchOptions`), the engine init sequence INLINE (its order is the init-order documentation), the
timers and the frame loop.

**The start is SPLIT in two (both `Session` methods):**

* **`startNetworkFor(mode, startGame, address)`** — host/join plus the server join hooks. **Game modes
  wire lobby and GameMatch together, lobby FIRST.**
* **`startWorldAndGame(startGame, startCoop)`** — content, or `oc::optional<GameMatch>` emplace.

---

# Lobby (`Game:Lobby`, serviced here)

`LobbySystem` is a plain stack local in main — **plain state, no entity handles.** The UI is the
MainMenu's lobby page, with `LobbyView` / `LobbyAction` snapshots polled exactly like the menu action.

**EVERY co-op or PvP pick enters the lobby instead of starting** — multiplayer to gather players,
offline as a LOCAL lobby (see below). The sandbox starts immediately, and command-line runs never
lobby — **a CLI game server calls `lobby.markStarted` so menu clients joining it still get the go
signal.**

## The local lobby and the world block

An offline pick calls `lobby.enter(host, coop, local = true)`: no network, no event filter, the host
is the only player, nothing is broadcast, and **Start launches at once** (`takeServerStart`, no ready
check or countdown). The page hides the roster, the ready button, the endpoint block and the chat, and
adds a second column, **the WORLD block** (`LobbyView::local` + `LobbyWorldView`, filled by
`Session::fillWorldView`):

* **Terrain seed + "Generate preview"** — `LobbyAction::GeneratePreview` → the session's
  `Procedural::TerrainPreview` (a `Session` member: it owns a job and a generator) samples the seed's
  COARSE stage into a 256² overview map (see the Procedural CONTEXT). The image crosses to the UI's
  `LobbyPreviewImage` ONCE per generation (UI cannot import Procedural) and the page uploads it into
  its own ImGui-managed texture. **A new preview first UNSEEDS a seeded world** — the diffusion
  runtime holds one seed process-wide.
* **Click the map, pick a playable-area size, then "Seed world"** — `LobbyAction::SeedWorld`
  carries the pick (0..1) and the area's side in metres; `serviceWorldAction` turns it into tweak
  OVERRIDES: `Terrain/Seed`, `Terrain/Origin X (m)` / `Origin Z (m)` (from
  `TerrainPreview::worldOffsetAt`, so the pick becomes the engine's origin), **`Terrain/Range
  (chunks)` sized to the area** (the previous radius is remembered and restored on unseed),
  `Terrain/Enabled=1` and `Ocean/Enabled=1` — the sandbox's switch. It then starts the session's
  `Procedural::TerrainSeeder`, which **pre-generates the area's full-detail diffusion TILES,
  nearest-first, loading any already in the disk cache instead of regenerating**; the terrain MESH
  streams live over them (the streamer's pumps park on the same per-tile events). **The page's bar is
  the tile count and Start stays disabled until the seeder is done**; the map draws the generated
  tiles' footprint as a square and estimates the tile count for the chosen size. "Re-seed" moves the
  origin or changes the size; "Unseed" (`UnseedWorld`) drops the overrides again.
  The seeder's tile-aligned coverage also becomes the streamer's **generated bounds**
  (`setGeneratedBounds`): the ring never requests a chunk outside it and the generator answers every
  full-detail sample past it from the coarse stage, so nothing outside the playable area costs a
  cold tile.
* **Start** first calls `TerrainGenV3::unloadModels()` for a seeded world (every tile it needs is on
  disk; the caches keep serving and nothing infers in game), then runs
  `startWorldAndGame(true, coop)` unchanged: the game spawns over the streamed terrain (its flat ground plane and grid still sit at y = 0 — the match does not yet read the
  terrain height), and `exitToMenu` unseeds, resets the preview and switches the terrain off.

The multiplayer lobby has no world block: a client would not get the seed or the origin (they would
have to ride the game's GMp event), so the seeded world is single-player only for now.

**The SANDBOX content** (`startWorldAndGame`, the `!startGame` branch): sponza + the skysphere
prefab, plus the procedural world — `Terrain/Enabled` and `Ocean/Enabled` are switched ON through
tweak OVERRIDES (applied at once, never written back to tweaks.cfg) and switched off again in
`exitToMenu`, so every other mode keeps its own default of off.

## Events (server-authoritative, reliable)

| Event | Meaning |
|---|---|
| `LbR` | Client ready toggle. |
| `LbG` | Start request — **the server re-validates that all are ready.** |
| `LbS` | Full state broadcast on every change. |
| `LbT` | Client PvP team pick. |
| `LbX` | Countdown done — launch. |

`LbS` carries the coop / countdown / started flags, the host's **co-op map settings** (seed, fill,
lanes), the PvP `numTeams` and `pvpMap`, and the roster with ready bits AND team picks.

**Clients ignore relayed `LbR` / `LbT` / `LbG` through `sender != 0`.**

## The map and match blocks

**Co-op** — the lobby page's "Map" block holds seed / fill / lanes as host-editable widgets committing
on release as a `SetMapSettings` action, read-only on clients. At launch main hands them to the host's
GameMatch through `setMapSettings` before `spawnWorld`. **Clients still generate ONLY from the game's
GMp event — the lobby copy is display.**

**PvP** — the host's MAP combo (`EPvpMap`; **the names reach the UI through
`LobbyView::pvpMapNames`, so UI never imports Game**) and a "Number of teams" slider (2..8, commit on
release). Every player picks a team through the combo on their OWN row of the player list.

**A joiner lands on the least-populated team, lowering the count re-seats anyone above it, and any map
or team-layout change mid-countdown cancels it.** At launch main hands count + picks + arena to
GameMatch (`setLobbyTeams`, `setPvpMap`) before `spawnWorld`.

## Ready check and launch

Ready check per player; anyone may Start once ALL are ready → a 3 s countdown on the **real clock**.
**ANY un-ready, or a mid-countdown join, cancels it.**

**The HOST's mode is authoritative**: a client's GameMatch coop flag comes from the first `LbS`, and
**that client CONSTRUCTS its GameMatch inside the dispatch** — the world is visible behind the lobby,
and `updateWindowed` is gated on menu-inactive so game input, camera and HUD stay off.

At countdown end the server spawns its world and calls `game->onClientJoined` for every lobby client.
**A client joining a RUNNING game gets `LbS(started)` BEFORE the replay**, through the join-hook order.

## Leaving

* The lobby page's **Leave** button — also on the "Connecting..." page, **the way out of a dead
  address** — becomes `exitToMenu`. Host: the server closes and every client sees a disconnect.
  Client: just its disconnect, and the server's `onClientLeft` hooks drop it.
* **A CLIENT whose server goes away** gets `NetworkManager::setOnServerLost`, fired inside `receive()`
  before the auto-reconnect. **main only sets a flag there** and runs `exitToMenu` at the next loop top
  (pre-kick window; the shutdown ends the reconnect), with a "Disconnected from the server" status
  line. **Command-line clients keep the auto-reconnect.**

**Known limit:** a CLI `--game --connect` client in a menu-hosted lobby never readies — its lobby is
inert, so the host cannot start.

---

# Chat (`Game:Chat`, serviced here)

`ChatSystem` (Game's `Chat.ixx`), a plain stack local next to the lobby. The UI is `UI:ChatPanel`: main pushes a `ChatView`
snapshot **only when `chat.generation()` changed**, and polls `takeChatOutgoing()` every frame.

Drawn EMBEDDED in the lobby page and as a translucent OVERLAY in the game layout's bottom-right corner
(co-op and PvP; single player fires locally only).

**ONE event, `"ChM"` [string ≤ 200 chars], reliable.** A sent line is `fireNetworkEvent`'d — the local
fire is our own log entry, and **the server relays to the other clients re-attributed to the real
sender.** main's dispatcher routes "ChM" to `chat.handleNetEvent()` BEFORE the Lb*/game split.

Both server-side event filters admit it: `ChatSystem::allowsEvent` in the lobby filter, and **the Game
layer's `Gq*` filter repeats the 256-byte cap by hand, because Game cannot import App.**

Names are "Host" / "Player N", like the lobby roster. The log survives lobby → match and resets on
exit-to-menu; 100 messages kept.

**Keys:** Enter with no active widget opens the input line — **read through ImGui, so it works while
the game viewport owns the keyboard**; Enter sends; Esc cancels. In-game both refocus `##GameViewport`
so the viewport-focus hotkey gate re-arms — **while typing, the grid hotkeys are off because the
viewport is not focused.**

---

# The escape menu

Esc opens it in **every running mode plus the LOBBY page**, never over the main menu's front or
settings pages.

**In game mode the game's own Esc cancel chain keeps first claim**: the overlay opens only once
`GameMatch::escWouldCancel()` is false — no pending two-click flow, no armed item, no open hotbar page,
and the mode is Select.

Actions: Resume / Pause / ExitToMenu / Quit (+ Unpause from the paused box). **The teardown runs in
the pre-kick window**, where main-thread entity destruction and the network shutdown are legal.

**The SHARED GAME PAUSE.** Opening the escape menu does NOT pause the sim (a co-op peer's game must
keep running under your menu). "Pause game" (offered over a running game only) calls
`GameMatch::requestPause(true)`: the authority flips `Time::setPaused` and broadcasts **GPz**; a
client sends a **GqZ** request instead, which the server applies and re-broadcasts — so **any
player may pause, and any player may resume**. main mirrors `game->isPaused()` into
`UI::setGamePaused` every frame: while set, `MainMenu::renderPausedBox` draws a centered PAUSED box
(under the escape menu, no dim) with a Resume button (`EscapeMenuAction::Unpause` →
`requestPause(false)`). The transport keeps running on the real clock, so the resume event still
arrives while the sim clock stands still; the join replay sends GPz to a late joiner; `~GameMatch`
clears the pause so exit-to-menu never strands the clock.

While the overlay is over a RUNNING game **the frame camera is left untouched** — neither the game's
follow cam nor the fly camera runs, because the fly-camera branch would overwrite it with the testbed
camera's pose (*the view dropped to that pose every time the overlay opened*).

The overlay also carries the **"Debug panels"** checkbox, and next to it **"Pause profiler"** —
`Profiler::setPaused` without the Profiler panel's own UI in the frame (the panel adopts the state when
it next shows). Both show in the game layout AND the editor layout (sandbox, command-line testbed), not
in the lobby. "Debug panels" has one state per layout: in game it drives the side panel (default off),
in the editor it shows or hides every docked panel, leaving only the viewport (default on). See the UI
CONTEXT.

---

# Testbed keys (`InputControls`)

`InputControls` is headless-inert — update and key handling never run there.

**In game mode `setGameMode(true)` mutes the spawn and possess keys.** Script event fires, hotbar
routing, F5/F6 and T/R/G stay. And **while the game HUD's hotbar is ACTIVE, keys 1..9 and 0 select
hotbar slots** and the testbed meanings are skipped.

## Always

| Key | Action |
|---|---|
| F5 | Reload shaders |
| F6 | Recompile the Script Editor's current script |
| F7 | Write the profiler text report (to `--profile-out`) |
| Pause/Break | Toggle the global simulation pause — see Core's two clocks; **works in game mode too** |
| T / R / G | Gizmo Translate / Rotate / Scale (editor only) |
| Esc | The escape menu (see above) |

**Script events fired on every press and release:** W/A/S/D, LShift, Space → `"W Down"` / `"W Up"` etc.

## Lights

| Key | Spawns at the camera |
|---|---|
| 2 | Point light, range 25, intensity 50 |
| 3 | Point light, range ~15, intensity 30 |
| 4 | Spot, cone 25°, edge softness 0.25 |
| 5 | Area quad, 1×1, emitting along the camera direction |
| 6 | Tube, radius 0.1, length 1, axis = camera up |
| 7 | Burst of 100 random point lights |

Values live on the component and are **deliberately not serialized**.

## Physics and force

| Key | Action |
|---|---|
| 8 / 9 | Spawn + throw a `physicsCube` / `physicsSphere` |
| N | Spawn a force emitter from the `Force/Emitter` tweak values |
| M | Re-apply those tweaks to the LAST spawned emitter |
| H | Forcefield stress burst — **500 random emitters** (random team, reach, focus) in a 150 m disc; press repeatedly to stack |
| F | Throw a physics sphere carrying a small team-1 forcefield — **enemy bubbles knock it back through the GPU force readback** |
| J | Drop an 11×11 territory-query grid 10 m ahead (`Force/Debug/Draw queries` colours each point by owning team) |
| B | **Clear** the spawned emitters, queries and force balls |

## Rendering debug

| Key | Action |
|---|---|
| P | Toggle the GI probe debug cubes (also the "GI/Debug probes" tweak — the game modes have no P) |
| O | Cycle its mode (irradiance ↔ cellSize/LOD colour; also "GI/Debug probe colour") |
| L | Aim the sun down the camera forward |

## Player control

**`C` toggles player control**, and **`V` toggles first/third person** while it is on (third pulls in on
a wall raycast).

* **On a network client** it drives the locally-owned capsule (see Multiplayer) with the same camera
  follow.
* **Single player or server** it spawns and possesses a local upright `playerCapsule.pre`
  (`LockRotation`): camera-relative WASD velocity steering, Space jump off a ground raycast, LShift
  sprint.
* The camera follows the interpolated body pose. `Player` tweaks cover eye height, third distance and
  sprint; the move params are shared with `Network/Player`.
* **`InputControls::applyPlayerCamera` overrides the frame camera right after the fly camera** in main.

## Network testing

| Key | Action |
|---|---|
| U (server) | Throw `Entities/Debug/netPhysCube.pre` |
| C (client) | Toggle player control |
| K | Fire the `"NetPing"` network event |

---

# The profile report writer

`Globals::profiler.writeReport(path, options)` (Core) writes `buildReport` through the writer main
injects with `setReportWriter` at startup — **because Core does no IO**, the same pattern as the tweak
registry's `setFileIo`: the lambda in main creates the directory and writes through FileSystem with
main-thread IO allowed explicitly (a one-shot). Wired to F7, the Profiler panel's Dump button, and the
`--profile-after` timer. See **Profiling** in [`Code/Core/CONTEXT.md`](../Core/CONTEXT.md).
