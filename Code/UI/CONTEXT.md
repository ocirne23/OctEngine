# UI

> Library documentation for `Code/UI`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction. The main menu and lobby FLOW is described in
> [`Code/App/CONTEXT.md`](../App/CONTEXT.md).

ImGui (docking branch) for all UI. Links Entity, RendererVK and Script (+ imgui PRIVATE).

`Globals::ui` sits in init_seg `OC_SEG_UI` and is **the FIRST engine global to destruct**: the panels
hold EntityPtrs (Scene selection, the Entity Editor document, a pending prefab save) and EntityChange
queues, which must be released while everything an entity destructor touches is still alive.

**The UI never mutates the world directly.** Panel mutations flow back as an `EntityChange` queue
through `takeEntityChanges()`, which merges every panel's queue into ONE vector — each source appends
and keeps its own capacity, **so the steady-state all-empty frame builds nothing and the returned
vector never allocates.**

---

# The widget pass — off the main thread, pipelined one full frame

The whole widget pass — `UI::updateJob`: the prepare wait, `ImGui::NewFrame`, every panel, the gizmo
update, and the draw-data snapshot — runs as **ONE Normal post-update job.**

| Step | Thread | When |
|---|---|---|
| `joinPostUpdateJobs()` | main | FIRST thing in the loop |
| `UI::flushMainThreadWork()` | main | right after the join |
| tweak poll / input / drains / sim | main | |
| `UI::prepare()` | main (submits jobs) | early — right after `input.update`, BEFORE controls / camera / `game.updateWindowed` |
| `UI::update(...)` | main | right before `kickPostUpdateJobs()` |
| `updateJob` | a worker | during present, `profiler.endFrame` and the fence/vsync wait |

**So the pass runs in a window where main MUTATES NOTHING the panels read and the workers are
otherwise idle — the UI is effectively free** instead of stealing a worker from the sim's parallel
phases.

> `"Post-update join"` is near-zero unless the widget pass outlasted the whole stall — expect that
> uncapped or GPU-light, where the fence never blocks.

The pass reads the frame's POST-sim state and no sim work overlaps it, **so there are no torn-read
caveats**; the one writer near it is the gizmo / EntityEditor writing entity fields while main idles
in the fence wait.

## `UI::update` — the main-thread half

Two things only:

1. **`ImGui_ImplSDL3_NewFrame()`** — the SDL3 backend queries the window and sets the OS cursor and
   mouse capture, **which are thread-affine to the window's owner.** `ImGui::NewFrame` itself only
   consumes the io state this writes, so it can run on the job, sequenced by the submit.
2. `submitPostUpdate` of the pass, capturing **`rootEntities` and `camera` BY REFERENCE — they must
   outlive the join** — and `deltaSec` by value. It carries `EJobFlag_ForeignWait`, because
   `updateJob`'s first act waits on `m_prepareCounter` (see Threading).

**Consequence:** UI visuals and every UI output are **one frame latent by design** — queues drain next
frame, and widgets built from frame N's state reach the screen at frame N+1's present.

## `flushMainThreadWork`

Work the panels collected but must NOT run on a worker:

* **TweakPanel `onChange` callbacks** — they re-record renderers, reload shaders with `waitIdle` and
  poke box3d, so the panel only COLLECTS changed vars.
* **The Entity Editor's container imports** — panels use lookup-only `World::findLoadedContainer`, and
  a miss queues the name for `flushContainerLoads`, because an import creates GPU resources.

Panel enable-toggles are `EntityChange::SetEnabled` for the same reason: `setEnabled` walks the
subtree suspending physics bodies, which are main-thread-only box3d writes. **"The UI never mutates
the world directly" now actually holds for it.**

## The draw-data snapshot

`renderImGuiToSnapshot()` ends the job: `ImGui::Render`, then a **DEEP COPY of the draw lists through
`CloneOutput`** into a UI-owned snapshot handed to `Renderer::setImGuiDrawData`.

> `ImGui::GetDrawData()` is only valid until the next `NewFrame`, and the NEXT frame's present records
> from the snapshot — sequenced by the join.

**DOUBLE-BUFFERED** (`g_imguiSnapshots[2]`): the job for frame N runs concurrently with present N,
which is still recording from snapshot N−1. **So the job writes slot `N&1` and frees only what that
slot held from N−2**, which present N−1 finished with before this job was even kicked. Null on frame
0, and the renderer skips the pass.

### The font atlas is NOT part of the copy

ImGui 1.92 bakes glyphs ON DEMAND, and **`ImDrawData::Textures` points at the LIVE
`ImGui::GetPlatformIO().Textures`** — which `ImGui_ImplVulkan_RenderDrawData` would service from
inside `present()` **while the widget pass is in `NewFrame` mutating those same `ImTextureData`s and
growing that vector.** A freed atlas read, not just a stale one. **This is what made the pass's
overlap with present unsafe.**

So the snapshot sets `Textures = nullptr` — imgui's documented "control the timing of texture updates
yourself" path — and **`Renderer::updateImGuiTextures()` does the uploads on MAIN in the quiescent
window between the join and the next `UI::update()`.** A glyph baked by pass N uploads at the top of
frame N+1, before the present that draws it.

**The snapshot handoff is two-step for the same reason:** `setImGuiDrawData` from the job only parks
the snapshot as PENDING, and `updateImGuiTextures()` promotes it to what present records. A fast pass
that finished before present N recorded used to be drawn a frame EARLY, with its textures still
create requests — on frame 0 the atlas always is, which tripped imgui's `GetTexID` assert at startup
in Debug every time.

## Known sharp edge

EntityEditor's `commitRespawn` draft builds (`build*SpawnInfo`) still run on the job and can import a
container not yet cached — **only when authoring a NEW container name**, since the edited entity's own
container is always already loaded.

---

# The prepare / render split

ImGui is single-context (docking, **NO viewports**), so one thread at a time with a happens-before is
safe. The panels' data-heavy work additionally splits into **`prepare()` — NO ImGui, worker-safe data
work — and `render()`, which reads what prepare built.**

`UI::prepare()` submits **one High job per data-heavy panel that was OPEN last frame** — the
`ImGui::Begin` result, so a hidden panel costs nothing — on `m_prepareCounter`. `updateJob()` waits on
it first thing under a `"UI prepare wait"` scope; **a visible wait means the prep did not fully
overlap the pre-UI work.**

Every panel prepares INLINE in its render if nothing ran ahead, **so the split is an optimization,
never a requirement.**

| Panel | What prepare does |
|---|---|
| **ProfilerPanel** | Auto-pause check plus a ring snapshot. `snapshotTracks` is itself a `parallelFor` over the tracks — per-track copy and sort, with record buffers handed back to per-track scratch slots to keep capacity; the `m_trackMaxDepth` map merge stays serial — plus `aggregateStats` when the Stats tab was open. **`selectFrame` from a worker only ever SETS paused (an atomic exchange); resume stays in render.** |
| **MemoryPanel** | `buildSnapshot` — the tracker's atomic tree is reader-safe. |
| **OutputLog** | Log snapshot under its mutex, plus the level and text filter into `m_visible`. |
| **AssetBrowser** | The filesystem rescans. See below. |

**CONTRACT: prepare reads only view state that render wrote LAST frame** — pause, zoom, filters, the
metric toggle. The same ordering the single-threaded version had, **so a filter edit shows next
frame.** A panel that fiber-waits inside prepare or render must not cache thread_locals across it —
**the widget pass is itself a job fiber now.**

## AssetBrowser caching

Its listings are **CACHED per directory.**

> It used to enumerate the filesystem EVERY FRAME: the current folder plus a `file_size` syscall per
> list row, and the tree ran TWO `directory_iterator` passes per expanded folder recursively —
> **about 1 ms/frame of syscalls.**

Now `listing(dir)` serves a cached `DirListing` (sorted dirs-first, sizes included), scanned inline the
first time and re-scanned by the prepare job **at most every `RescanIntervalSec` (1 s — files also
arrive from outside: prefab saves, compiled scripts, cooked assets), and ONLY while the panel is
focused or hovered (`m_active`), and only for listings a render actually read (`touchedFrame`).**

`invalidateListings()` on any mutation the browser itself makes — new script, text or prefab, rename,
prefab save — forces the next draw to re-scan, and `navigateTo` re-scans the entered folder on the
spot.

---

# Panels

| Panel | Notes |
|---|---|
| **Viewport** | Renderer output. Assets drag-drop from Content to spawn at the cursor (`Camera::screenToWorld`), queued into `m_viewportChanges`. |
| **Scene** (`SceneView`) | Live hierarchy. **Selection is an OWNING `EntityPtr`** — a raw one dangled when anything else deleted the entity — and `pruneSelection` drops it once its tree leaves the world. F2 rename, Delete, drag-reparent, drag-to-Content saves a prefab, prefab instances locked (unpackable). |
| **Properties** | Name, enabled, transform, render info + bounds toggle, script path, live public ScriptData fields. |
| **Content** (`AssetBrowser`) | Filesystem browser rooted at `Assets/`; drag into viewport or hierarchy, prefab drop guards. **THE place scripts are created and opened**: "New Script" writes a minimal `.dsl` and opens it; double-click or drag a `.dsl` reaches the Script Editor, **whose sidebar holds Save/Reload — it has no path field of its own.** |
| **Script Editor** | The DSL text editor (autocomplete-driven). It handles RAW key events through `UI::handleKeyEvent`, which is why `isScriptEditorFocused()` exists — see the Input capture gate. |
| **Text Editor** | Plain text. |
| **Log** (`OutputLog`) | `Core.Log` view with filters and search. |
| **Entity Editor** | Authors a `.pre` document. See below. |
| **Tweaks**, **Stats** (`ui.setRenderStats`), **Profiler**, **Memory** | |

The docked set lives inside a `"Root"` window with a dockspace; `"Viewport"` uses
`ImGuiWindowFlags_NoBackground` so the 3D render shows through.

## Entity Editor

Authors a `.pre`: name and transform plus one section per component, bound to draft `SpawnInfo`s.

**Changes queue a `RespawnEntity`** (`commitRespawn`), since most SpawnInfo fields are spawn-only.
Drag widgets use `commitDrag(...)`, which also calls `applyDraftsLive()` per change frame — **so the
viewport tracks the drag and there is ONE respawn on release.**

`applyDraftsLive` only touches what is re-appliable in place: `RenderComponent::localTransform`,
`ForceEmitter` setters, and `LightComponent`'s list. **Physics, animator and script data stay
respawn-only.**

`onOpened` / `onRespawned` forward the entity main.cpp just spawned back into the panel.

## Chat (`UI:ChatPanel`)

The multiplayer text-chat widget: a `ChatView` snapshot pushed by main through `UI::setChatView` **only
when App.Chat's log generation changed**, plus `takeChatOutgoing()` for the sent line.

`renderEmbedded` draws it inside the lobby page (MainMenu holds a non-owning pointer, set by UI's
ctor); `renderOverlay` draws it as a translucent bottom-right window in the game layout, after the
viewport window and before the escape overlay.

Enter with no active item focuses the input (`ImGui::IsKeyPressed` — **ImGui sees every SDL event
before the engine's input gate**); Enter sends; Esc cancels. Both refocus the window named by the
caller (`##GameViewport`), **so the viewport-focus hotkey gate re-arms.**

## Main menu (`UI:MainMenu`)

**Not a docked panel: while active it REPLACES the whole widget pass** — a fullscreen menu over the
world, and `updateJob` early-outs after rendering it plus the escape overlay (**reachable from the
LOBBY page, its only leave mechanism**).

No panels run, so their open flags stay false and no prepare jobs are submitted; **the dockspace's
first-time layout runs on the first post-menu frame instead.**

It also owns the **escape overlay** for every running mode (`setEscapeMenuOpen` / `takeEscapeMenuAction`
— Resume / ExitToMenu / Quit) and the **lobby page** (`setMainMenuLobbyView` / `takeMainMenuLobbyAction`).

Both the menu action and the lobby action use the same sequencing: **the UI only RECORDS an action, and
main polls it after the post-update join and performs the mode start.** See
[`Code/App/CONTEXT.md`](../App/CONTEXT.md).

## Game HUD overlay (`UI:GameHudOverlay`)

Draws the in-game HUD OVER the viewport, with `Core.GameHud` as the model.

**Pure `ImDrawList` painting into the Viewport window's own draw list — no ImGui widgets, so it never
takes focus or eats input.** `UI::update` calls `render()` while the Viewport window is current.

It takes ONE `GameHud::Snapshot` per frame and, crucially, **reports back where it drew**:
`setSlotScreenRects` and `setPopupButtonRects`, or empty spans when nothing was drawn — **nothing drawn
means nothing clickable.** That is how gameplay hit-tests clicks against the hotbar and the barracks
popup.

Tweaks under `HUD`: Enabled, Scale, Opacity, Hotbar slot size, Hotbar text scale.

---

# Game layout

`UI::setGameLayout(true)`, set by main's `startWorldAndGame` for co-op and PvP, cleared by
exit-to-menu. Main thread, in the pre-kick window — **the pass reads it on the job.**

The widget pass draws **NO editor panels**. Instead:

* **ONE fullscreen undecorated `##GameViewport` window.** A real ImGui window, so **the HUD paints into
  its draw list and `IsWindowFocused` feeds the same `isViewportFocused` gate the editor's Viewport
  panel does** — the input gate in Input.cpp, the game's focus checks and the camera controllers all
  work unchanged. It is handed focus on the layout's first frame (`m_gameLayoutFocusPending`).
  > Clicking into the debug section takes focus off the game; clicking the world gives it back; the
  > escape overlay takes it while open.
* **Optionally a LEFT `##GameDebug` window**, while the escape menu's "Debug panels" checkbox
  (`MainMenu::debugPanelsEnabled`, offered only in the game layout) is set: pinned to the edge at full
  height, width edge-resizable, holding **Tweaks / Profiler / Memory / Log as TABS**. The viewport rect
  shrinks to the remainder.

The prepare gating flags follow the SELECTED tab; Content and Script Editor stay closed. No dockspace
runs, **so its first-time layout still builds on the first editor frame.** The sandbox pick keeps the
full editor.

---

# Gizmo split

`UI.Gizmo` declares `EGizmoMode` and `IGizmo`; **Input's `GizmoController` implements it** (it needs
mouse listeners and viewport focus, which live below UI); main owns the object and calls
`ui.setGizmo(&gizmo)`. **Non-owning — the controller detaches itself in its destructor.**

`UI::update` drives it from the panel's own viewport rect and selection, after the panels settle.

**But the gizmo ENTITY has to tick with the rest of the scene**, so main makes a separate
`ui.drawGizmoEntity(renderer, dt)` call at the entity-update point — *it needs the renderer, which
`UI::update` has no business holding.*

---

# The NodeEditor is dead code

`UI/Private/NodeEditor/` and the vendored `UI/Private/lib/imgui-node-editor` **still compile but are
not reachable from the running editor**: there is no node panel, no `UI::m_scene`, and no `.scr` open
or create routes. See [`Code/Script/CONTEXT.md`](../Script/CONTEXT.md).
