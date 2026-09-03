# Input

> Library documentation for `Code/Input`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction. The testbed key bindings (`InputControls`, which lives in `Code/App`) are in
> [`Code/App/CONTEXT.md`](../App/CONTEXT.md).

Links UI (+ openxr_loader PRIVATE). `Globals::input` drains events; `Globals::vrInput` reads OpenXR.

**The OS event pump is NOT here.** It lives on the window thread in `Core.Window`; this library
consumes what that thread buffered. Read the window-thread section first — everything else depends
on it.

## The window thread (`Core.Window`)

[Window.ixx:26](../Core/Public/Window.ixx#L26).

### Why it exists

Win32 fixes an HWND's message queue to the thread that CREATES it — there is no transfer API — and
SDL3's "main thread" is whoever calls `SDL_Init(SDL_INIT_VIDEO)`. So `Window::initialize` spawns a
dedicated thread that initializes SDL, creates the window, and OWNS the message pump for the process
lifetime.

**WndProc dispatch — raw input, cursor, IME, and Windows' modal drag/resize loops, which used to
freeze the whole engine — never touches the engine's main thread again.**

### Per frame

| Thread | Call |
|---|---|
| Engine main, loop top, before the fence wait | `requestPump()` |
| Engine main, in `Input::update` | `waitPumpDone()`, then `lockEvents()` / `unlockEvents()` |

`lockEvents()` hands out the buffer directly and **does NOT drain** — the caller clears it before
unlocking.

### Between pumps it helps the job system

The window thread is NOT spinning on the OS queue. It runs queued window ops, then calls the
**idle-work hook** (`setIdleWork(work, wait, wake)`), which App wires to
`jobSystem.tryRunOneHighJob` / `externalHelperWait` / `wakeExternalHelper`
([main.cpp:158](../App/main.cpp#L158)).

> **High priority ONLY.** Normal and Low carry multi-second jobs — V3 terrain tiles, nav builds, the
> UI widget pass — and one of those would stall the next frame's pump behind it.

`work` and `wait` are installed through the op queue so they only ever run on the window thread;
`wake` is main-thread-only, called from `requestPump`, **so a pump request always interrupts the
nap.** The thread claims a reserved scheduler context through
`jobSystem.registerExternalHelper()`, which is what gives it `PerWorker::local()` and profiling.

### Marshalling

`runOnWindowThread(op, wait = false)` executes at the next pump.

> **`wait = true` is INIT-TIME ONLY** — the Vulkan surface creation and the ImGui SDL backend init. A
> wait can stall behind a helped job, so never call it per frame. `setTitle` is a queued async op.

`servePump` SNAPS the served epoch to the observed request epoch: one serve can cover several queued
requests, and a `+1` would deadlock `waitPumpDone`.

**Accepted cross-thread SDL reads** (cached state, no dispatch): `getWindowSize`,
`SDL_GetKeyboardState`, and `ImGui_ImplSDL3_NewFrame`'s cursor and capture calls, which still run on
main inside `UI::update`. SDL re-applies cursors on the window thread through WM_SETCURSOR — verify
drags that leave the window after touching this.

`getDisplayRefreshHz()` is queried on the window thread at creation and on display/mode-change
events; 0 = unknown. `Core.Time`'s stable-dt snap uses it as the exact vsync period instead of a
measured estimate.

### Pump timing

**Owned by `Core.Time::beginFrame`**, not by Input — see
[`Code/RendererVK/CONTEXT.md`](../RendererVK/CONTEXT.md) for the whole frame-pacing story.

The kick fires `Time → Input pump lead (ms)` (Saved, default 2.0) BEFORE the frame starts, so the
window thread pumps while main still waits and **the events are at most one lead old when sampled**.

* Pumping at wait-start was a whole vsync stale.
* Pumping only after the wait put the pump on the critical path.
* **CAPPED:** the frame end is exact, so the limiter kicks inside its own wait.
* **UNCAPPED:** the fence decides, so the kick targets the EARLIEST plausible unblock — the raw last
  start plus a **decayed running MINIMUM** of the raw interval. Under FIFO the CPU unblocks early on
  alternate frames, and a mean-based prediction kicked too late on those. The fence is waited
  (bounded) until a lead before that, then kicked, then waited out; an earlier fence signal just ends
  the slice and kicks now.

The window thread sets `timeBeginPeriod(1)` process-wide, so the limiter's `Sleep(1)` steps are real
milliseconds.

## `Input::update` — the gate and the dispatch

[Input.cpp:23](Private/Input.cpp#L23). Runs on MAIN.

1. `waitPumpDone()` under a `"Input wait pump"` Wait scope — this waits out the rare case where the
   window thread is still mid-pump.
2. Dispatch straight out of the window's buffer, **held under its lock**: the window thread only
   appends mid-pump and the next pump is a whole frame away, so the hold is uncontended.
3. Every event goes to `ImGui_ImplSDL3_ProcessEvent` first, and a KEY_DOWN additionally to
   `ui.handleKeyEvent` — the DSL Script Editor's raw-key path, which ignores everything else.
4. The capture gate (below).
5. Window events fan out to the system listeners; then a per-type switch drives the mouse and
   keyboard listeners. **Key repeats are dropped** (`evt.key.repeat == 0`).

Profile scopes: `"Key dispatch"` and `"Mouse button dispatch"` — **a fat "Input" frame is almost
always a heavy one-shot handler** (F5 shader reload, F6 script recompile, the spawn/possess keys).

### The ImGui capture gate

**Keyboard** events are dropped when ImGui wants the keyboard or text input, **or the Script Editor is
focused**, AND the viewport is not focused.

> The Script Editor handles raw key events itself rather than through a normal ImGui widget, so it
> never sets `WantCaptureKeyboard` / `WantTextInput` on its own. `isScriptEditorFocused()` is checked
> alongside those so typing a digit there — a vector literal's `1.0,2.0,3.0` — does not ALSO fire
> whatever global gameplay shortcut that key is bound to.

**Esc always reaches the listeners** unless a text field is being edited (ImGui's own Esc-cancels-edit
via `WantTextInput`) or the Script Editor is focused.

> With keyboard nav on, ANY focused ImGui window sets `WantCaptureKeyboard` — so the escape-menu
> overlay, focused while open, would otherwise eat the Esc that closes it.

**Mouse** events are dropped when `WantCaptureMouse` AND (the viewport is not focused OR the viewport
is GRABBED). **Exception: a left mouse-button UP always passes through while the viewport is
grabbed**, to prevent a stuck mouse.

main's escape block toggles the overlay: open → close, else open (in game mode, only once
`escWouldCancel` has nothing left). While the overlay is over a RUNNING game the frame camera is left
untouched — neither the game's follow cam nor the fly camera runs, where the fly-camera branch used
to overwrite it with the testbed pose.

## Listeners

`addMouseListener()` / `addKeyboardListener()` / `addSystemEventListener()` return **move-only RAII
handles** (`InputListenerHandle<T>`, [InputDef.ixx:59](Private/InputDef.ixx#L59)) that unregister in
their destructor. There is no manual remove API.

* Assign the callback slots through `->`. The listener object itself is owned by `Input` and is
  address-stable.
* **A handle must not outlive `Globals::input`** — a static-teardown holder needs init_seg ordering.
* Callbacks are `oc::function` slots: `MouseListener` (moved / wheel / pressed / released),
  `KeyboardListener` (textEditing / textInput / keyPressed / keyReleased), `SystemEventListener`
  (windowEvent / quit). `JoystickListener` is a virtual-method interface and is currently unused by
  the dispatch.

**State queries:** `isKeyDown(scancode)` and `isKeyDownByName(name)` read SDL's keyboard state array
directly. `isMouseInWindow` / `isWindowHasFocus` / `wantMouseGrab` / `wantMouseVisible` /
`isMouseCaptured` are flags the App and UI set (`setMouseCaptured` means a tool such as the gizmo
owns the left-drag).

## `GizmoController`

[GizmoController.ixx:18](Private/GizmoController.ixx#L18). Implements `IGizmo`, declared by
`UI.Gizmo`.

**It lives in Input, not UI, because it needs mouse listeners and viewport focus** — but the UI OWNS
the instance through the interface, and main does `ui.setGizmo(&gizmo)`.

* Spawns `Entities/Gizmo.pre`, follows the Scene panel's selection, and keeps a **constant apparent
  screen size** (world scale = distance × `m_screenSize` 0.06).
* `applyMode` / `setChildEnabled` switch the visible handle set: Translate axes and planes, Rotate
  rings, uniform Scale. T/R/G bind to the modes in the testbed.
* **Picking is analytic** in gizmo space — ray vs axis line, plane quad, or ring — with separate
  tolerances: `m_axisPickFrac` (0.07 of arm length), `m_planePickScale` (1.4× the handle's bounds
  radius), `m_ringPickScale` (0.6×).
* **Dragging is ABSOLUTE, not incremental.** `beginDrag` snapshots the gizmo origin, the selected
  entity's parent world transform, the drag axis or plane normal, the grab hit point, and the start
  rotation/scale — so there is no per-frame drift.
* The camera and viewport rect are captured in `update()`, so the **event-driven** drag can unproject
  without them being passed in.
* A kinematic or static physics body does NOT follow a gizmo drag — see PhysicsComponent's
  "the body owns the pose".

## VR

`VrInput` (`Globals::vrInput`) — OpenXR controller input through `Core.VrSession`'s `IVrSession`, so
Input and RendererVK stay unlinked.

* `getMoveAxis()` (left stick: x strafe, y forward), `getTurnAxis()` (right stick: x yaw),
  `isActive()` (a thumbstick delivered input this frame).
* `getHand(EVrHand::Left|Right)` → `VrHandState{ poseValid, position, orientation, trigger, grip,
  primaryButton, secondaryButton }` — A/X and B/Y.
* Head pose: `isHeadPoseValid` / `getHeadOrientation` / `getHeadPosition`, **in the tracking (LOCAL /
  play) space** — compose the play-space transform, as the renderer does, for world space. Used for
  head-relative locomotion.
* The reference space and session are owned by the RENDERER and not destroyed here; the VIEW space
  for the head pose is owned and destroyed here.

## Camera controllers

**`FreeFlyCameraController`** — WASD + mouse-look, Space/Ctrl vertical, boost ×10, speed 5,
sensitivity 0.01, near 0.05 / far 42000.

* `setMovementEnabled(false)` is **mouse-look only**: the WASD/Space/Ctrl fly keys are released to
  whoever borrowed them — player control routes them to the owned entity while active (testbed key
  C).
* `setPosition` is a hard reposition, so player control hands the camera back where the possessed
  capsule left it instead of popping to the pre-possession spot.
* `m_maxLookDelta` (150 px) rejects a mouse delta that large between two events as "we missed some".
* `setLockToWorldUp` keeps the horizon level.

**`VRFreeFlyCameraController`** — the VR variant. App picks between them by
`renderer.isVrEnabled()`.
