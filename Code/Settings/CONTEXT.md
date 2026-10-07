# Settings

**Every tweak's value and the tweak registry.** Depends on Core only; Threading and Audio link it, so every library
above them has it (App links it directly).

| Piece | File | What |
|---|---|---|
| `Settings.Tweaks` | [Tweaks.ixx](Public/Tweaks.ixx) | `TweakRegistry` (the rows, groups, flags, Saved / Synced, overrides, locks, listeners) + the `Tweak::` helpers |
| `Settings` (umbrella) | [Settings.ixx](Public/Settings.ixx), [Settings.cpp](Private/Settings.cpp) | `EngineSettings` + `Globals::settings` + `Settings::registerAll()` |
| `Settings.<Domain>` | `Public/<Domain>.ixx` + `Private/<Domain>.cpp` | one domain's plain-data types and its `Settings::register<X>(X&)` |

Domains: Time, Render, World, Physics, Audio, Spatial, Network, Nav, Threading, Terrain, Clutter, Trees, Rocks, Ocean,
Force, ParticleSystem, Hud, Input, App, Game.

---

# The model

* **ONE copy of every value: `Globals::settings`** (`EngineSettings`, init_seg `OC_SEG_SETTINGS` = `.CRT$XCA3`: built
  before every engine global, destroyed after them). Systems READ it directly (`Globals::settings.fog.density`, or a
  `const X& x = Globals::settings.x` reference) - **never a value copy**. A system that writes a setting (a game
  preset, the camera zoom, a stat readout) writes it there.
* **Types are plain data** (float / int / bool / glm / enums), defaults in the member initializers. An enum or small
  config struct a tweak points at lives in its Settings module, and the library imports it from there. The exception
  is `TimeSettings`: Core sits below this library, so the type is Core.Time's (main binds the instance with
  `Globals::time.bindSettings`).
* **Registration knows nothing of the systems.** `Settings::registerAll()` runs once from main right after the file
  hooks, before any system initializes, and calls every `register<X>` in a fixed order (the order is the panel's row
  order inside a category two domains share). A registration's own `onChange` only keeps settings consistent (a
  clamped pair, a normalized vector).
* **A system's reaction is a LISTENER**, attached at its init: `Tweak::onChange(Globals::settings.grass.bladesPerPatch,
  this, [this] { ... })` - found by address, so it asserts if the row is not registered yet. A system that can die
  before exit (GameMatch's StructureSystem) calls `Tweak::removeListeners(this)`; `unregisterInRange` drops the
  listeners an object in the range owns. A re-registration keeps the listeners. They fire after the row's `onChange`,
  on main, through `TweakRegistry::notifyChanged(var)`: the panel's deferred flush (EVERY changed row, not only rows
  with a reaction), an override, a synced or saved value applied.
* **An owner that watches many rows** adds ONE change listener: `TweakRegistry::addChangeListener(owner, fn(const
  TweakVar&))`, fired after the row's own listeners for every change (the renderer's UBO bake). Removed by
  `removeListeners(owner)` / `unregisterInRange` like the row listeners.
* **Code that writes a setting** and wants the reactions calls `Tweak::notifyChanged(Globals::settings.x.y)`. A
  renderer setter that stores a lockable value (`setShadowParams`, `setFogParams`, ...) marks its bake dirty itself.
* **Everything registers, headless included** (before the move some only registered on the windowed path). The Game
  rows are global: their values persist across matches.

## Adding a tweak

A field in the domain's struct (with its default) + one `Tweak::` line in its `register<X>`. A new domain = a module
pair, a member in `EngineSettings`, an `export import` and a `register<X>` call in `registerAll`.

---

# The registry (`Settings.Tweaks`)

```cpp
Tweak::floatVar("Category/Sub", "Name", &s.variable, min, max, step, onChange, flags);
Tweak::intVar / boolean / color3 / float3 / ...
```

**Pointers are non-owning: the variable must outlive the registration** (every one is a member of
`Globals::settings`). **Identity is `"Category/Name"`.**

## Groups

The panel's top folds are **groups**, NOT part of the category string: `TweakGroups::c_table` in
Tweaks.ixx maps each ROOT category ("Sky" of "Sky/Clouds") to a group with a header colour —
Graphics / FX / System / Game — and a root listed nowhere lands in the trailing "Other" group.
`Tweak::groups()` / `Tweak::groupIndexOf(category)` are the lookups the TweakPanel uses. **A new
root category goes into that table**, otherwise it shows under "Other". Panel order = table order.

## `ETweakFlags`

Optional last parameter after `onChange`, or `Tweak::ScopedFlags` RAII to flag a whole
block. **Explicit per-call flags win over the block default** - except an explicit `None`, which does not opt out of
a Saved / Synced block (`registerVar` only checks for Saved | Synced).

**`Synced`** broadcasts server → clients:

* `TweakRegistry::update(dt)` per frame **poll-detects** changes — the panel writes through raw
  pointers, so polling is the only reliable hook.
* NetworkManager watches `syncGeneration()`, which the poll bumps on any Synced change.
* `packSynced` splits every Synced var into self-contained records chunked to fit one network message.
* It rides the engine-reserved `"OcTweakSync"` event, intercepted in `fireEventAttributed`, so it
  never reaches scripts or game hooks. **Only CLIENTS apply**, and `applySyncedBlob` ignores keys the
  receiver did not flag Synced and clamps to the receiver's own bounds.

**Policy:** all `Game/*` tweaks are `Synced` except `Game/Camera` (personal preference) and
`Game/Sim LOD` (per-process performance tuning, Settings.World).

**`Runtime`** marks a var that a lock never covers (below): a value its owner keeps live inside a locked section
(the renderer's sun direction / colour, ambient, up axis, the wind, the cloud coverage). It does not count as an
explicit flag, so a `ScopedFlags` default still applies to it. **To make a tweak live** (editable while its section is
locked): flag it `Runtime`. Its own UBO value being `UboLive` is not enough - any LOCKABLE value that also reads it
keeps the row under the lock (`clouds_coverage` was live, but `clouds_upperCoverage` read the row too).

## Locks

`Tweak::lock(name, categories, lockedByDefault, onChange)` → an id. A LOCK covers whole SECTIONS: each category it
names and that category's subtree. A var belongs to the **nearest** lock category on its path (the longest match),
so a nested lock ("Sky/Clouds") is not covered by its parent's ("Sky"); `Runtime` vars are never covered.

**Only LOCKABLE rows take part.** The owner names them once its sources are known (`markLockable(address, size)`;
the renderer marks every source of a baked value). A row that is not lockable has no lock (`lockOf` = `c_noLock`, no
panel button), is never read-only, never counts in `lockState`, and `isVarBakeable` is false for it; a fold whose
section has no lockable row carries no toggle (`lockAt`). Without an owner (headless) nothing is lockable.

**Every covered ROW has its own state** (`isVarLocked(var)`, `setVarLocked(var, state)`); `setLocked(id, state)`
sets every row of the section, and `lockState(id)` says None / Some / All (the fold toggle's look). A row that
registers later (or moves under a lock) takes the section's last state; a re-registration keeps its state. Main
thread; both setters fire the lock's `onChange`. `lockOf(var)` / `lockAt(category)` find a row's lock and the lock a
fold carries.

**What a row feeds is found by ADDRESS.** `TweakRegistry::sourceState(address, size)` answers Locked / Unknown /
Live for any value: an address inside a row's variable (or a Color3's intensity) is that row - locked, or a row no
lock covers, is Locked; unlocked or `Runtime` is Live; an address no row holds is Unknown. A range spanning several
rows (a vec3 over three float rows) reaches all of them. It is linear in the rows: the owner asks on a lock click
(the lock's `onChange`), never per frame. `isVarBakeable(var)` says whether a CHANGE of that row moves a baked value
(locked, or no lock covers it; never `Runtime`). **The owner gives the lock its meaning** - the renderer bakes every
UBO value whose sources are all locked into the shaders (see "The frame UBO and the tweak locks" in
[`Code/RendererVK/CONTEXT.md`](../RendererVK/CONTEXT.md)).

**The state is NOT saved** (a user decision): every run starts at the owner's default, a panel click lasts for the
run, and `--tweak "@lock/<name>=0|1"` (a section) / `"@lock/<Category/Name>=0|1"` (a row) overrides it for a run.
`saveFile` drops any `@lock/` line an older tweaks.cfg still holds.

## Command-line overrides

* **`--tweak "Category/Name=v [v v v]"`** (`setOverride`) works on any variable. It applies now or at
  the variable's registration; the snapshot is taken after the apply so it does not read as a change.
  This is how an unattended profiling run pins settings.
* **`--tweaks <file>`** (`loadOverrides`) applies a whole file of `Category/Name=v` lines with `#`
  and `//` comments. Later `--tweak` flags win over it.
* `Assets/Scenarios/cpu-profile.tweaks` = the heavy GPU features off, for CPU-focused runs.

## The IO hooks

`setFileIo(read, write)`: this library cannot see File (it sits right above Core), so main injects
`FileSystem::readFileStr` / `writeFileStr` (`installFileHooks`, before `registerAll`). Without them the registry keeps
working in memory.
