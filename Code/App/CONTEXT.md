# App

> Documentation for `Code/App` — the testbed executable: main.cpp, `App.Lobby`, `InputControls`,
> `App.ProfileDump`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction.
>
> The frame-loop ordering rules (kick/join windows, post-update jobs) are documented in the owning
> libraries' CONTEXT.md files: RendererVK, Spatial, Threading, UI, Entity.

## Main menu

App.exe with NO mode flags boots into a fullscreen start screen (`UI:MainMenu`).

**Contents**

* Co-op / PvP / sandbox buttons.
* A "Host a server" checkbox, which shows the endpoint other clients dial. It is seeded with
  `netGetLocalAddress()` plus the launch port, then upgraded to the EXTERNAL IP once
  `netGetExternalAddress()` lands — that runs on a detached thread with a shared_ptr result block,
  polled in the loop's menu block. The LAN address then moves to a dim note line; failure keeps the
  LAN address.
* Or an address field (`ip[:port]`; empty = offline).
* A Quit button.
* A SETTINGS page built from the tweak registry: curated per-section category-prefix tables in
  MainMenu.cpp, with dev branches (Debug / Stress / Stats / Cheats / Emitter / V3, plus Time/Paused)
  filtered out. Rows are drawn by the exported `drawTweakVar` shared with the TweakPanel, and
  `onChange` is deferred through the TweakPanel's list.

**Flow.** While the menu is active the widget pass draws ONLY the menu — the viewport rect is the
full window, there are no editor panels and no prepare jobs. The menu just records a
`MainMenuAction`; main() polls `takeMainMenuAction()` after the post-update join and runs `startMode`
in the pre-kick window, under a one-shot `AllowMainThreadIO` (the F10 pattern). `startMode` is a
lambda shared with the command-line path: network host/join, testbed or game world content,
`oc::optional<GameMatch>` emplace.

**Skipping it.** Any of `--game` / `--server` / `--connect` / `--headless`, `--scenario`, or an
unattended flag starts directly. A failed host or join leaves the menu up.

During the menu phase the testbed keys are muted (`setGameMode(true)`) and free flight is paused; the
mode start restores both per the selection.

The old one-shot `startMode` is SPLIT into `startNetworkFor` (host/join plus the server join hooks —
game modes wire lobby and GameMatch together, lobby FIRST) and `startWorldAndGame` (content plus the
`oc::optional<GameMatch>` emplace).

## Lobby (`App.Lobby`)

`LobbySystem` is a plain stack local in main. The UI is the MainMenu's lobby page, with
`LobbyView` / `LobbyAction` snapshots polled like `MainMenuAction`.

A MULTIPLAYER co-op or PvP pick from the menu enters the lobby instead of starting. Offline picks and
the sandbox start immediately, and command-line runs never lobby — a CLI game server calls
`lobby.markStarted` so menu clients joining it still get the go signal.

### Events (server-authoritative, reliable)

| Event | Meaning |
|---|---|
| `LbR` | Client ready toggle. |
| `LbG` | Start request — the server re-validates that all are ready. |
| `LbS` | Full state broadcast on every change. |
| `LbT` | Client PvP team pick. |
| `LbX` | Countdown done — launch. |

`LbS` carries: coop / countdown / started flags; the host's CO-OP MAP SETTINGS (seed / fill / lanes);
the PvP `numTeams`; `pvpMap`; and the roster with ready bits AND team picks. Clients ignore relayed
`LbR` / `LbT` / `LbG` through `sender != 0`.

The lobby page's "Map" block holds the co-op map settings as host-editable widgets committing on
release as a `SetMapSettings` LobbyAction, and as a read-only line on clients. At launch main hands
them to the host's GameMatch through `setMapSettings` before `spawnWorld`; clients still generate
ONLY from the game's GMp event, so the lobby copy is display.

### PvP match setup (lobby page "Match" block)

* The host's MAP combo (`EPvpMap` from Game — Lane / Wide lane / Chokepoints / Circle; `SetPvpMap`,
  applied at once). The names reach the UI through `LobbyView::pvpMapNames`, so UI never imports
  Game.
* A "Number of teams" slider (2..8, `SetNumTeams`, commit on release). Clients see a read-only line.
* Every player picks a team through the combo on their OWN row of the player list (`SetTeam` → `LbT`
  on a client, applied at once on the host; the `LbS` echo moves the row). A joiner lands on the
  least-populated team, lowering the count re-seats anyone above it, and any map or team-layout
  change mid-countdown cancels it.
* At launch main hands count + picks + arena to the host's GameMatch (`setLobbyTeams` and
  `setPvpMap`, before `spawnWorld`). See [`Code/Game/CONTEXT.md`](../Game/CONTEXT.md) for how the
  arena and the per-team Bases are built, and how clients get them through GMp.

### Ready check and launch

Ready check per player; anyone may Start once ALL are ready → a 3 s countdown
(`c_countdownSeconds`, real clock). ANY un-ready, or a mid-countdown join, cancels it.

The HOST's mode is authoritative: a client's GameMatch coop flag comes from the first `LbS`, and that
client CONSTRUCTS its GameMatch inside the dispatch — the world is visible behind the lobby, and
`updateWindowed` is gated on menu-inactive so game input, camera and HUD stay off. At countdown end
the server spawns its world and calls `game->onClientJoined` for every lobby client (capsules plus
world replay). A client joining a RUNNING game gets `LbS(started)` BEFORE the replay, through the
join-hook order.

### Leaving

* The lobby page's Leave button — also on the "Connecting..." page, the way out of a dead address —
  is a `LobbyAction::Leave` that main turns into `exitToMenu`. Host: the server closes and every
  client sees a disconnect. Client: just its disconnect, and the server's `onClientLeft` hooks drop
  it from the roster and the match.
* A CLIENT whose server goes away (host left through Leave or the escape menu's Exit to menu, or link
  death) gets `NetworkManager::setOnServerLost`, fired inside `receive()` before the manager's
  auto-reconnect. main only sets a flag there and runs `exitToMenu` at the next loop top (pre-kick
  window; the shutdown ends the reconnect), with a "Disconnected from the server" status line on the
  front page. Command-line clients keep the auto-reconnect.

**Known limit:** a CLI `--game --connect` client in a menu-hosted lobby never readies — its lobby is
inert, so the host cannot start.

## Chat (`App.Chat`)

`ChatSystem` is a plain stack local in main next to the lobby. The UI is `UI:ChatPanel`: main pushes
a `ChatView` snapshot ONLY when `chat.generation()` changed, and polls `takeChatOutgoing()` every
frame.

* Drawn EMBEDDED in the lobby page, and as a translucent OVERLAY in the game layout's bottom-right
  corner (co-op and PvP; single player fires locally only).
* ONE event: `"ChM"` [string ≤ 200 chars], reliable. A sent line is `fireNetworkEvent`'d — the local
  fire is our own log entry, and the server relays to the other clients re-attributed to the real
  sender. main's dispatcher routes "ChM" to `chat.handleNetEvent()` BEFORE the Lb*/game split.
* Both server-side event filters admit it: `ChatSystem::allowsEvent` in the lobby filter, and the
  Game layer's Gq* filter repeats the 256-byte cap by hand (Game cannot import App).
* Names are "Host" / "Player N", like the lobby roster.
* Keys: Enter with no active widget opens the input line — read through ImGui, so it works while the
  game viewport owns the keyboard; Enter sends; Esc cancels. In-game both refocus `##GameViewport` so
  the viewport-focus hotkey gate re-arms. While typing the grid hotkeys are off, because the viewport
  is not focused.
* The log survives lobby → match and resets on exit-to-menu (`chat.reset()` + `ui.clearChat()`). 100
  messages kept.

## Testbed keys (`InputControls`)

| Key | Action |
|---|---|
| F5 | Reload shaders |
| F6 | Recompile script |
| F7 | Write the profiler text report (see Profiling) |
| T / R / G | Gizmo modes |
| Pause/Break | Toggle the global simulation pause (see Core's GLOBAL PAUSE — also in game mode) |

**Lights**

| Key | Action |
|---|---|
| 1 | Clear test lights |
| 2–6 | Spawn one light of each type at the camera |
| 7 | Burst 100 points |

All share one lazily built template; values live on the component and are deliberately not
serialized. While the game HUD's hotbar is ACTIVE (see `Core.GameHud`), keys 1..9 and 0 select hotbar
slots instead and the testbed meanings above are skipped.

**Physics and force**

| Key | Action |
|---|---|
| 8 / 9 | Spawn + throw a physicsCube / physicsSphere |
| 0 | Hang an 8-cube joint chain from `staticBody()` |
| N | Spawn a force emitter from the Force/Emitter tweak values |
| M | Re-apply those tweaks to the LAST spawned emitter |
| H | 500-emitter stress burst |
| F | Physics sphere carrying a bubble |
| J | Territory query grid |
| B | Clear |

**Rendering debug**

| Key | Action |
|---|---|
| P | Toggle GI probe debug |
| O | Cycle its mode |
| L | Aim the sun down the camera forward |

**Player control**

`C` toggles player control.

* On a network client it drives the locally-owned capsule (see Multiplayer) with the same
  first/third-person camera follow.
* Single player or server, it spawns and possesses a local upright player capsule
  (`playerCapsule.pre`, `LockRotation`): camera-relative WASD velocity steering, Space jump off a
  ground raycast, LShift sprint, and V to toggle first/third person (third pulls in on a wall
  raycast). The camera follows the interpolated body pose. "Player" tweaks cover eye height, third
  distance and sprint; move params are shared with "Network/Player".
* `InputControls::applyPlayerCamera` overrides the frame camera right after the fly camera in main.

**Network**

| Key | Action |
|---|---|
| U (server) | Throw `netPhysCube.pre` |
| C (client) | Toggle player control |
| K | Fire "NetPing" |
