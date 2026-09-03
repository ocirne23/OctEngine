# Input

> Library documentation for `Code/Input`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction. The testbed key bindings (`InputControls`, which lives in `Code/App`) are in
> [`Code/App/CONTEXT.md`](../App/CONTEXT.md).

## The window thread (`Core.Window`)

### Why it exists

Win32 fixes an HWND's message queue to its CREATING thread — there is no transfer API — and SDL3's
"main thread" is whoever calls `SDL_Init(SDL_INIT_VIDEO)`. So `Window::initialize` spawns a dedicated
thread that initializes SDL, creates the window, and owns the pump forever.

WndProc dispatch — raw input, cursor, IME, and Windows' modal drag/resize loops, which used to freeze
the engine — never runs on the engine main thread.

### Pump timing

Each frame the pump is kicked roughly `Time → Input pump lead (ms)` (Saved) BEFORE the frame starts,
so it runs while main still waits and the events are at most a lead old when sampled.

* Pumping at wait-start was a whole vsync stale.
* Pumping only after the wait put the pump on the critical path.

ONE path serves both modes (`Time::beginFrame`). The frame starts at whichever comes LAST of:

1. the limiter's desired end (capped; exact), and
2. the fence signal, predicted as the raw last start plus a decayed running MINIMUM of the raw
   interval (`m_minFramePeriodSec`, relaxing ~2 %/frame).

> The minimum, not the mean: under FIFO the CPU unblocks EARLY on alternate frames — 4/8 ms around a
> 6 ms period — and a mean-based kick was too late on those.

The kick then fires a lead before the EARLIER of the two predictions. It gets there by
sleeping/spinning to that moment ITSELF ("Pump kick wait") while POLLING the fence
(`waitFrameSlot(0)` = status check, marker-free; only the UNCAPPED path breaks on an early signal,
since capped the frame starts at the desired end regardless), then does the blocking fence wait, then
— capped only — `limitFrameRate` to the desired end.

* The capped path MUST pre-kick too: a GPU-bound capped frame (fence wait > target) is "already late"
  at the limiter, and kicking there landed at frame start.
* NEVER use a driver-timed fence wait for this. The spec allows a finite timeout to run
  "substantially longer than requested", and NVIDIA's ran to the signal — the kick landed at frame
  start and blocked main for the pump's length. ("Pump kick wait" is the poll's scope.)
* The window thread sets `timeBeginPeriod(1)` process-wide, so the limiter's `Sleep(1)` steps are
  real milliseconds.

### Sampling on main

`Input::update` calls `waitPumpDone()` and dispatches straight out of the window's buffer under
`lockEvents()` / `unlockEvents()` ON MAIN, exactly as before. ImGui event processing and listeners
never moved.

### Helping the job system

Between pumps the window thread helps the scheduler.

* `jobSystem.registerExternalHelper()` claims a reserved extra scheduler context
  (`m_numContexts = workers + 2`).
* `tryRunOneHighJob()` runs HIGH-priority jobs ONLY — Normal and Low carry multi-second jobs (V3
  tiles, nav builds) that would stall the next frame's pump — until the request epoch parks it.

### Marshalling and known cross-thread calls

* Window-affine SDL calls marshal through `window.runOnWindowThread(op[, wait])`, executed at the
  next pump. Blocking waits are INIT-ONLY: `Surface::initialize`'s `SDL_Vulkan_CreateSurface` and
  `ImGui_ImplSDL3_InitForVulkan`. `setTitle` queues async.
* `servePump` SNAPS the served epoch to the observed request epoch — one serve can cover several
  queued requests, and a +1 would deadlock `waitPumpDone`.
* Accepted cross-thread SDL: `getWindowSize` / `SDL_GetKeyboardState` reads, and
  `ImGui_ImplSDL3_NewFrame`'s cursor/capture calls (still on main, in `UI::update`; SDL re-applies
  cursors on the window thread through WM_SETCURSOR — verify drags that leave the window).

## ImGui capture gate (`Input::update`)

* Keyboard events are dropped while ImGui wants the keyboard or text input, or the Script Editor is
  focused, and the viewport is NOT focused.
* Mouse events are dropped while ImGui wants the mouse.
* **Exception:** an Esc KEY-DOWN always passes unless a text field is being edited. With
  `NavEnableKeyboard` every focused ImGui window sets `WantCaptureKeyboard`, so the escape-menu
  overlay — focused while open — would otherwise eat the Esc that toggles it closed.

main's escape block toggles: open → close, else open (in game mode, only once `escWouldCancel` has
nothing left). While the overlay is over a RUNNING game the frame camera is left untouched — neither
the game's follow cam nor the fly camera runs, where the fly-camera branch used to overwrite it with
the testbed pose.

## Listeners

SDL3 event pump on the window thread (above); listener objects hold `std::function` callbacks.

`addKeyboardListener()` / `addMouseListener()` / `addSystemEventListener()` return move-only RAII
handles (`InputListenerHandle<T>`, InputDef.ixx) that unregister in their destructor. Assign callback
slots through `->`; there is no manual remove API.

## Other

* **`GizmoController`** — in Input because it needs mouse listeners and viewport focus, but DRIVEN by
  the UI through `IGizmo`. Spawns `Entities/Gizmo.pre`, follows the Scene selection, holds constant
  apparent screen size, T/R/G switch Translate/Rotate/Scale, and a drag writes back to the entity.
  A kinematic or static physics body does NOT follow — see PhysicsComponent.
* **`VrInput`** (`Globals::vrInput`) — OpenXR controller input through `Core.VrSession`'s
  `IVrSession`.
* **Camera controllers** — `FreeFlyCameraController` (WASD + mouse) and
  `VRFreeFlyCameraController`. App picks by `renderer.isVrEnabled()`.
