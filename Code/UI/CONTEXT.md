# UI

> Library documentation for `Code/UI`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction. The main menu and lobby flow are described in
> [`Code/App/CONTEXT.md`](../App/CONTEXT.md).

ImGui (docking branch) for all UI. `UI::update` runs the panels; panel mutations flow back as an
`EntityChange` queue (`takeEntityChanges()`) — **the UI never mutates the world directly.**

## The widget pass: off the main thread, pipelined one full frame

The whole widget pass — `UI::updateJob`: `ImGui::NewFrame`, every panel, and the gizmo update — runs
as ONE Normal job, submitted as a POST-UPDATE JOB (see Threading).

**Frame shape**

1. `UI::update(rootEntities, camera, deltaSec)` is the main-thread half: `ImGui_ImplSDL3_NewFrame`,
   then `submitPostUpdate` of the pass, capturing the root list and camera BY REFERENCE and
   `deltaSec` by value. Called right before `kickPostUpdateJobs()` and `present`.
2. The batch is JOINED at the TOP of the NEXT frame (`joinPostUpdateJobs`, first thing in the loop
   scope). There is no separate UI counter or "UI join" any more.

So the pass runs during `present`, `profiler.endFrame` and the fence/vsync wait — a window where main
MUTATES NOTHING the panels read and the workers are otherwise idle. The UI is effectively free
instead of stealing a worker from the sim's parallel phases. ("Post-update join" is near-zero unless
the widget pass outlasted the whole stall — expect that uncapped or GPU-light, where the fence never
blocks.)

The pass reads the frame's POST-sim state. No sim work overlaps it, so there are no torn-read
caveats; the one writer near it is the gizmo / EntityEditor writing entity fields while main idles in
the fence wait.

Panel enable-toggles are `EntityChange::SetEnabled` — `setEnabled` walks the subtree suspending
physics bodies, which are box3d writes and main-thread-only. "The UI never mutates the world
directly" now actually holds for it.

### The job ends with its own draw data

`renderImGuiToSnapshot`: `ImGui::Render` plus a DEEP COPY of the draw lists through `CloneOutput`
into a UI-owned snapshot handed to `Renderer::setImGuiDrawData`. The NEXT frame's present records
from it, sequenced by the join — `GetDrawData` is only valid until the next `NewFrame`.

The snapshot is DOUBLE-BUFFERED: job N runs concurrently with present N, which still records from
snapshot N−1, so the job writes slot `N&1` and frees only that slot's N−2 contents (finished by
present N−1 before the job was kicked). It is null on frame 0, and the renderer skips the pass.

### The font atlas is not part of the copy

ImGui 1.92 bakes glyphs ON DEMAND, and `ImDrawData::Textures` points at the LIVE
`ImGui::GetPlatformIO().Textures`, which `ImGui_ImplVulkan_RenderDrawData` would service from inside
`present()` while the widget pass is in `NewFrame` mutating those same `ImTextureData`s and growing
that vector — a freed atlas read, not just a stale one. This is what made the pass's overlap with
present unsafe.

So the snapshot sets `Textures = nullptr` (imgui's documented "control the timing of texture updates
yourself" path), and `Renderer::updateImGuiTextures()` does the uploads on MAIN in the quiescent
window between the join and the next `UI::update()`. A glyph baked by pass N uploads at the top of
frame N+1, before the present that draws it.

### Main-thread pieces, in frame order

1. **join**
2. **`UI::flushMainThreadWork()`** — TweakPanel `onChange` callbacks (they re-record renderers, reload
   shaders with `waitIdle`, poke box3d, so the panel only collects changed vars) and the Entity
   Editor's container imports (panels use lookup-only `World::findLoadedContainer`; a miss queues the
   name for `flushContainerLoads`).
3. **tweak poll / input / drains / sim**
4. **`UI::update()`** — `ImGui_ImplSDL3_NewFrame` (SDL window, cursor and capture calls are
   thread-affine to the window's owner) plus the queue.
5. **kick**
6. **present**

After the loop, main joins the final in-flight batch before teardown.

**Consequences.** UI visuals and every UI output are one frame latent by design: queues drain next
frame, and widgets built from frame N's state reach the screen at frame N+1's present.

**Known sharp edge.** EntityEditor's `commitRespawn` draft builds (`build*SpawnInfo`) still run on the
job and can import a container not yet cached — only when authoring a NEW container name, since the
edited entity's own container is always already loaded.

## The prepare / render split

ImGui is single-context (docking, NO viewports), so one thread at a time with a happens-before is
safe. The panels' data-heavy work additionally splits into `prepare()` (NO ImGui — worker-safe data
work) and `render()` (ImGui, reading what prepare built).

`UI::prepare()` — main.cpp, right after `input.update` and BEFORE controls / camera /
`game.updateWindowed` — submits one High job per data-heavy panel that was OPEN last frame
(`m_profilerOpen` / `m_memoryOpen` / `m_logOpen` = the `ImGui::Begin` results, so a hidden panel costs
nothing) on `m_prepareCounter`. `updateJob()` waits on it first thing ("UI prepare wait" scope — a
visible wait means the prep did not fully overlap).

Every panel prepares INLINE in its render if nothing ran ahead, so the split is an optimization,
never a requirement.

### Prepared panels

* **AssetBrowser** — its listings are CACHED per directory. It used to enumerate the filesystem EVERY
  FRAME: the current folder plus a `file_size` syscall per list row, and the tree ran TWO
  `directory_iterator` passes per expanded folder recursively — about 1 ms/frame of syscalls. Now
  `listing(dir)` serves a cached `DirListing` (sorted dirs-first, sizes included), scanned inline the
  first time and re-scanned by the prepare job at most every `RescanIntervalSec` (1 s — files also
  arrive from outside: prefab saves, compiled scripts, cooked assets), and ONLY while the panel is
  focused or hovered (`m_active`) and only for listings a render actually read (`touchedFrame`).
  `invalidateListings()` on any mutation the browser itself makes (new script/text/prefab, rename,
  prefab save) forces the next draw to re-scan, and `navigateTo` re-scans the entered folder on the
  spot.
* **ProfilerPanel** — auto-pause check plus a ring snapshot. `snapshotTracks` is itself a
  `parallelFor` over the tracks (per-track copy + sort, record buffers handed back to per-track
  scratch slots to keep capacity; the `m_trackMaxDepth` map merge stays serial), plus
  `aggregateStats` when the Stats tab was open (`m_statsVisible`). `selectFrame` from a worker only
  ever SETS paused — an atomic exchange — and resume stays in render.
* **MemoryPanel** — `buildSnapshot`; the tracker's atomic tree is reader-safe.
* **OutputLog** — log snapshot under its mutex, plus a level and text filter into `m_visible`.

**Contract.** Prepare reads only view state that render wrote LAST frame — pause, zoom, filters,
metric toggle — the same ordering the single-threaded version had, so a filter edit shows next frame.
A panel that fiber-waits inside prepare or render must not cache thread_locals across it; the widget
pass is itself a job fiber now.

## Panels

| Panel | Notes |
|---|---|
| **Viewport** | Renderer output. Assets drag-drop from Content to spawn at the cursor (`Camera::screenToWorld`). |
| **Scene** (`SceneView`) | Live hierarchy. Selection is an OWNING `EntityPtr` — a raw one dangled when anything else deleted the entity — and `pruneSelection` drops it once its tree leaves the world. F2 rename, Delete, drag-reparent, drag-to-Content saves a prefab, prefab instances locked (unpackable). |
| **Properties** | Name, enabled, transform, render info + bounds toggle, script path, live public ScriptData fields. |
| **Content** (`AssetBrowser`) | Filesystem browser rooted at `Assets/`; drag into viewport or hierarchy, prefab drop guards. THE place scripts are created and opened: "New Script" writes a minimal `.dsl` and opens it; double-click or drag a `.dsl` reaches the Script Editor, whose sidebar holds Save/Reload — it has no path field of its own. |
| **Script Editor** | The DSL text editor (autocomplete-driven). |
| **Text Editor** | Plain text. |
| **Log** (`OutputLog`) | `Core.Log` view with filters and search. |
| **Entity Editor** (`EntityEditor`) | Authors a `.pre` document: name/transform plus one section per component, bound to draft SpawnInfos. See below. |
| **Tweaks** | Auto-populated. |
| **Stats** | `ui.setRenderStats`. |
| **Profiler**, **Memory** | See Core's Profiling and Memory sections. |

**Entity Editor detail.** Changes queue a `RespawnEntity` (`commitRespawn`), since most SpawnInfo
fields are spawn-only. Drag widgets use `commitDrag(...)`, which also calls `applyDraftsLive()` per
change frame, so the viewport tracks the drag and there is one respawn on release. `applyDraftsLive`
only touches what is re-appliable in place — `RenderComponent::localTransform`, `ForceEmitter`
setters, `LightComponent`'s list — while physics, animator and script data stay respawn-only.

### Chat (`UI:ChatPanel`, `Private/Chat/`)

The multiplayer text-chat widget: a `ChatView` snapshot, pushed by main through `UI::setChatView`
only when App.Chat's log generation changed, plus `takeChatOutgoing()` for the sent line.

* `renderEmbedded` draws it inside the lobby page (MainMenu holds a non-owning pointer, set by UI's
  ctor); `renderOverlay` draws it as a translucent bottom-right window in the game layout, after the
  viewport window and before the escape overlay.
* Enter with no active item focuses the input (`ImGui::IsKeyPressed` — ImGui sees every SDL event
  before the engine's input gate); Enter sends; Esc cancels. Both refocus the window named by the
  caller (`##GameViewport`) so the viewport-focus hotkey gate re-arms.
* Flow described in [`Code/App/CONTEXT.md`](../App/CONTEXT.md).

### Main menu (`UI:MainMenu`)

Not a docked panel: while active it REPLACES the whole widget pass — a fullscreen menu over the
world, and `UI::updateJob` early-outs after rendering it. See
[`Code/App/CONTEXT.md`](../App/CONTEXT.md).

## Game layout

`UI::setGameLayout`, set by main's `startWorldAndGame` for co-op and PvP, cleared by exit-to-menu.

The widget pass draws NO editor panels. Instead:

* One fullscreen undecorated `##GameViewport` window — a real ImGui window, so the HUD paints into
  its draw list and `IsWindowFocused` feeds the same `isViewportFocused` gate the editor's Viewport
  panel does. The input gate in Input.cpp, the game's focus checks and the camera controllers work
  unchanged. It is focused on the layout's first frame.
* Plus, while the ESCAPE MENU's "Debug panels" checkbox (`MainMenu::debugPanelsEnabled`, offered only
  in the game layout) is set, a LEFT side `##GameDebug` window pinned to the edge at full height
  (width edge-resizable, 300 .. 60 %) holding Tweaks / Profiler / Memory / Log as TABS. The viewport
  rect shrinks to the remainder.

The prepare gating flags (`m_profilerOpen` / `m_memoryOpen` / `m_logOpen`) follow the SELECTED tab;
Content and Script Editor stay closed. No dockspace runs, so its first-time layout still builds on
the first editor frame. The sandbox pick keeps the full editor.

## Gizmo split

`UI.Gizmo` declares `EGizmoMode` and `IGizmo`; Input's `GizmoController` implements it; main owns the
object and does `ui.setGizmo(&gizmo)`.

`UI::update` drives it after panels settle the viewport rect and selection, and main calls
`ui.drawGizmoEntity(renderer, dt)` at the entity-update point.
