# Script

> Library documentation for `Code/Script`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction. The DSL language reference and DslCompiler usage are in
> [`Code/DslCompiler/CONTEXT.md`](../DslCompiler/CONTEXT.md).

## Compilation

Visual scripts compile to real DLLs.

`ScriptHost` (`Globals::scriptHost`, Core-only) compiles a `.scr` with the installed MSVC toolchain —
vswhere → vcvars64.bat, `cl /LD /std:c++20 /MD /Od /Zi /DSCRIPT_BUILD`, `/FI ScriptAPI.h` — into
`Assets/Local/Scripts/`.

* Cached by path; hot-reloads through `getOrLoad(path, forceRecompile)` — F6, or a Script Editor
  save. Live entities hot-swap.
* On failure the previous build is kept.
* A `.scr` is BODY-ONLY C++: no `#include`, no `#define`.

### Cooked alternative (`SCRIPTS_STATIC`, off by default)

`Code/AppScripts` generates one aggregate TU that `#include`s every `.dsl` — only `.dsl`; `.scr` is
not cooked — into its own namespace, and compiles it INTO the engine
(`/WHOLEARCHIVE:AppScripts` keeps the registrations).

`REGISTER_*()` markers at the end of each `.scr` feed a static-init registry that `getOrLoad`
resolves from instead of cl and LoadLibrary — whole-program-optimized. It is all-or-nothing: one
broken `.scr` fails the aggregate.

## `.scr` files

A `.scr` IS the generated C++ — entry points `OnSpawn` / `Update` / `OnDestroy` / `OnEvent` /
`OnPhysicsEvent`, plus `ScriptEventCount` / `ScriptEventName` / `ScriptDataSize` — with the node graph
appended as `//@graph` / `//@node` comments. The NodeEditor regenerates code from the graph, and the
file is also hand-editable.

**The `.scr` / NodeEditor path is REMOVED from the running editor**: no node panel, no `UI::m_scene`,
no `.scr` open or create routes. The `NodeEditor/` sources remain in the tree for reference and still
compile as unused partitions. A `.scr` shows as a plain text file in Content, is not offered by
script pickers, and is not cooked. `ScriptHost` itself would still compile one if a `.pre` carried
the path, but nothing authors that any more.

> **When Claude is asked for a script, author a `.dsl` through DslCompiler — never a `.scr`.**

## `ScriptAPI.h` — the host↔DLL ABI

* C linkage; PODs, glm and raw pointers only.
* The `ScriptContext` function table is APPEND-ONLY — cached DLLs must keep working.
* Scripts see a layout-compatible `Entity` MIRROR: pos / scale / rot / parent only, with offsets
  static_asserted in ScriptContext.cpp.
* Components a script cannot name go through opaque handles: `ctx->entityGetForceComponent(entity)`
  returns `void*`, and `ctx->forceGet*` / `forceSet*(handle, ...)` cast back host-side. The
  NodeEditor hoists the handle so the lookup runs once.

`ScriptModule` holds the DLL entry pointers, event names and data layout (`dataLayoutId`,
`dataFields`, `requiredComponents`). Do not cache function pointers across reloads.

## DSL surface

* `math.*` namespace holds all scalar math, with **ANGLES IN DEGREES**. `ocInverseLerp` / `ocRemap` /
  `ocMoveTowards` / ... in ScriptAPI.h avoid double-evaluation. Randomness is one ENGINE-side
  generator, so hot-reload does not reset it.
* `self.animator.setFloat` / `setBool` / `setTrigger` / ... — animation is the animator's surface, and
  nothing names a clip.
* A getter-call member MUST register `writable=false`: a writable member's emit is used verbatim as
  an assignment target.

### ScriptData

`//@@data [private|public] <type> <name>` fields; default Hidden.

* Exposed fields surface through `ScriptDataFields()` (`OcScriptField[]`) — the only engine-side
  layout description.
* The Properties panel edits them LIVE (never serialized). The Entity Editor authors INITIAL values —
  text under `Component Script` / `Data` in a `.pre`, re-applied by `applyInitialValues` after every
  `syncScriptData` reallocation.

### Containers

`T[]` arrays live ENGINE-side: `OcArray` generation-tagged handles, and the storage survives
hot-reload.

* No unchecked reads. `foreach [ref] T x in c` iterates; `ifexist [ref] T x in c [at k]` is the single
  checked lookup.
* POD arrays resolve once to a span (`ctx->arraySpan`) — one ABI call per loop, elements are plain
  loads, and `ref` binds a real C++ reference into the span. Entity and String arrays keep checked
  per-element reads.
* **THE SPAN IS ONLY VALID WHILE THE ARRAY DOESN'T GROW.** `checkContainerMutations` (ScriptLoader)
  enforces it as a whole-document fixed-point over the call graph — a push through a parameter
  poisons the function for every caller, conservative on purpose — and runs from BOTH `save()` and
  `load()`, since the editor's Compile & Run path never parses. A push or clear on a container an
  enclosing loop reads is refused.
* `ref` is refused on handle types and non-writable containers; non-`ref` elements cannot be assigned.
* The editor-side grey-out (`ScriptLang::isContainerIterated`) is still syntactic — transitive
  mutation through a user function is authorable and gets refused at load.

### Requirements

`//@@require` is enforced LIVE by `requirementsMet` (mask test) at every entry point — not latched at
spawn, since hot-reload mutates the module in place.

* `OnSpawn` runs once, the first frame the entity qualifies (`onSpawnRan`). `OnDestroy` pairs with
  that flag, never with the live check.
* `syncScriptData` re-allocates, refills require slots and clears `onSpawnRan` when the module layout
  changed. `syncScriptDataLive` (every post-spawn entry point) also re-registers the event listener,
  whose stored pointer would otherwise dangle.

### No null

There is NO null in the DSL. Absent values are OPTIONAL types (`Entity?`, readable only through
`ifexist`), and dead entities in arrays report a lookup miss.

## Fault containment

`OC_SCRIPT_FAULT_CONTAINMENT` at the top of ScriptComponent.cpp, default 1. Set it to 0 for raw
calls, so faults crash normally and give clean dumps.

* Every entry-point call goes through SEH-guarded invokers (`invokeScript*` in ScriptComponent.cpp):
  plain `__try` / `__except`, no C++ exceptions, zero cost until a fault.
* A hardware fault in script code — integer divide by zero, a stale pointer — marks the module
  `faulted` and logs instead of crashing.
* `requirementsMet` folds the flag in, so the script stops running everywhere, `OnDestroy` included
  (its data may be half-written). ScriptEventManager's direct `OnEvent` path checks it itself.
* A successful (re)load clears it (`loadDll`), so F6 or Compile & Run is the recovery.
* Deliberately unguarded: breakpoints, stack overflow, C++ exceptions (filter whitelist in
  `scriptFaultFilter`). Float divide-by-zero never faults — FP exceptions stay masked, so inf and NaN
  propagate.

## The DSL subsystem

Data model, autocomplete, bindings registry, save/load and the C++ transpiler live under
`Private/DSL/` as partitions re-exported from `Public/ScriptHost.ixx`. It is editor-agnostic; UI's
Script Editor panel (`import Script;`) is its consumer.

Types an outside importer needs must be `export`ed — partitions leak unexported types across
same-module files, which is easy to miss.

## `DslCompiler`

```
DslCompiler <input> [output.dsl] [--compile]
```

`Code/DslCompiler`, a console exe, and **THE way Claude authors `.dsl` files**. Write only the DSL
text; the tool validates it with the same loader as the editor and the full binding vocabulary,
transpiles, and writes the exact file the editor's Save produces.

* **Usage and full language reference:** [`Code/DslCompiler/CONTEXT.md`](../DslCompiler/CONTEXT.md).
* Raw input = single-`@` directives (`@require` / `@data` / `@event`) plus TAB-indented code. The tool
  prefixes every line with `//@` — which is what turns `@require` into the stored `//@@require` — and
  wraps it in the markers.
* An existing `.dsl` as input = normalize and regenerate the C++ in place.
* `--compile` cl-compiles and loads the output through the real ScriptHost pipeline (the F6 path) —
  the final check that the script loads in the engine as-is.
* Errors print as `<input>(<line>): <what>`. Relative paths resolve against `Assets/`. It exits
  through `_Exit`, since nothing is initialized and global dtors assume engine teardown order.
