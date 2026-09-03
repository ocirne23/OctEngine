# App

> Documentation for `Code/App` — the testbed executable: `main.cpp`, `App.Lobby`, `App.Chat`,
> `InputControls`, `App.ProfileDump`.
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
| 3 | **`joinPostUpdateJobs()`** | Last frame's widget pass and Nav field steps. Windowed and headless alike. |
| 4 | `ui.flushMainThreadWork()` + `renderer.updateImGuiTextures()` | Deferred tweak callbacks, container imports, and the glyphs the pass baked — **the ImGui context is quiescent from the join until `ui.update()`.** |
| 5 | `forceSystem.joinMerge()` | Last frame's merge job — **before input or drains can touch emitters.** |
| 6 | Menu / lobby / chat servicing | Only while the main menu is active. See below. |
| 7 | `TweakRegistry::update(dt)` | Saved/Synced change detection. |
| 8 | `input.update` → **`ui.prepare()`** → `controls.update` | The panel prepare jobs overlap everything down to `ui.update`. |
| 9 | Escape menu | |
| 10 | Camera: `game->updateWindowed` **or** VR **or** fly camera + `applyPlayerCamera` | |
| 11 | Script reload requests, then the UI and script `EntityChange` drains | |
| 12 | `simDeltaSec = time.getSimDeltaSec()` | **Read HERE, after the input dispatch and tweak poll, so a pause toggle applies to this very frame.** |
| 13 | `networkManager.receive(dt)` | Snapshot targets and events land before the sim reads them. |
| 14 | `game->updatePlayer(simDt)` | ONLY the player-body writes (pre-physics). |
| 15 | `scriptContext.update(...)` | |
| 16 | **KICKS: `getCullView` → `spatialIndex.kickUpdateJob` → `renderer.kickBeginFrameJob`** | Under a `"Frame kicks"` scope, **which attributes the submit + wake cost that used to read as a gap.** |
| 17 | `physics.update(simDt)` | ≤ 1 step; contact events stay buffered. |
| 18 | `audio.update` → `navSystem.update` → **`joinBeginFrameJob` → `joinUpdateJob`** | Headless instead calls `spatialIndex.commitFrame()`. |
| 19 | `game->update(simDt)` **or** `world.setSimLodFocus(&camera.position, 1)` | The rest of the game tick; also publishes the SIM LOD focus. |
| 20 | `physics.dispatchContactEvents(...)` | **AFTER the joins** — contact scripts query the index and can touch renderer state. |
| 21 | **`world.update(renderer, simDt)`** | The parallel entity pass. |
| 22 | `networkManager.send(dt)` | Server: snapshot entities at their POST-update poses. |
| 23 | terrain → collider → ocean → scatter → particles → force | |
| 24 | `ui.drawGizmoEntity` → **`ui.update(...)`** | |
| 25 | **`kickPostUpdateJobs()` → `renderer.present()`** | Headless kicks too — **an unkicked queue only fills up.** |
| 26 | `mainLoopScope.stop()` → `profiler.endFrame()` | **The scope stops BEFORE the frame mark, so the record stays inside this frame's window.** |
| 27 | Headless: Sleep-based tick limiter | |

After the loop: `joinPostUpdateJobs()` and `forceSystem.joinMerge()` for the final in-flight batches.

> **The quiescent window (16 → 18) is the whole point.** The drains, net receive and `game.updatePlayer`
> above it are the last registers and container loads, and `physics.update` no longer fires contact
> scripts — so the cull and begin-frame jobs overlap the physics step, audio and the nav publish.
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
| `--quit-after <sec>` / `--no-vsync` | `--no-vsync` is just `setOverride("Time/VSync=0")`. |
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

**The start is SPLIT in two:**

* **`startNetworkFor(mode, startGame, address)`** — host/join plus the server join hooks. **Game modes
  wire lobby and GameMatch together, lobby FIRST.**
* **`startWorldAndGame(startGame, startCoop)`** — content, or `oc::optional<GameMatch>` emplace.

---

# Lobby (`App.Lobby`)

`LobbySystem` is a plain stack local in main — **plain state, no entity handles.** The UI is the
MainMenu's lobby page, with `LobbyView` / `LobbyAction` snapshots polled exactly like the menu action.

**A MULTIPLAYER co-op or PvP pick enters the lobby instead of starting.** Offline picks and the
sandbox start immediately, and command-line runs never lobby — **a CLI game server calls
`lobby.markStarted` so menu clients joining it still get the go signal.**

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

# Chat (`App.Chat`)

`ChatSystem`, a plain stack local next to the lobby. The UI is `UI:ChatPanel`: main pushes a `ChatView`
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

The overlay also carries the **"Debug panels"** checkbox that drives the game layout's side panel.

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
| P | Toggle the GI probe debug cubes |
| O | Cycle its mode (irradiance ↔ cellSize/LOD colour) |
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

# `App.ProfileDump`

`writeProfileReport(path, options)` — `Profiler::buildReport` writes through FileSystem, **because Core
does no IO.** Wired to F7, the Profiler panel's Dump button, and the `--profile-after` timer. See
**Profiling** in [`Code/Core/CONTEXT.md`](../Core/CONTEXT.md).
