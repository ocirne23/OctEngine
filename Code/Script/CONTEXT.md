# Script

> Library documentation for `Code/Script`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency
> direction. The **DSL language reference** and DslCompiler usage are in
> [`Code/DslCompiler/CONTEXT.md`](../DslCompiler/CONTEXT.md).

**Scripts compile to real DLLs.** This library is the compile/load pipeline plus the DSL subsystem;
it links File only, and knows nothing about the engine — no renderer, entity or input. The engine-side
glue (`ScriptContext`, `ScriptEventManager`, `ScriptComponent`) lives in **Entity**.

> **When Claude is asked for a script, author a `.dsl` through DslCompiler.** Never a `.scr`.

## The pipeline

```
.dsl text  --DslCompiler/editor-->  .dsl file (generated C++ on top, //@-commented DSL below)
           --ScriptHost::getOrLoad-->  cl /LD  -->  Assets/Local/Scripts/<stem>.dll  -->  LoadLibrary
```

`getOrLoad` is **extension-agnostic**: a `.scr` and a `.dsl` are both just a source file whose
body-only C++ compiles under the same `ScriptAPI.h` ABI. It never inspects the extension, only the
file's content.

## `ScriptHost` (`Globals::scriptHost`)

[ScriptHost.ixx:60](Public/ScriptHost.ixx#L60).

* `getOrLoad(path, forceRecompile = false)` returns the cached module, compiling on first use. **On
  failure it returns null and KEEPS the previous build**, and first-time failures are cached so they
  are not retried every frame. The returned pointer is stable until shutdown.
* `handleScriptReloadRequests(paths)` and `setCurrentScriptPath` / `reloadCurrentScript` — **F6
  recompiles whatever the editor panel is editing.**
* `m_loadMutex` serializes the cache and a miss's compile/load, because concurrent `ScriptComponent`
  spawns hit it from parallel spawn jobs. Module pointers stay stable (node-based map), so cached
  callers keep lock-free reads.

### The compile

**Blocks the main thread by design** — F6, an editor save, or a spawn that needs its module — under
an explicit `AllowMainThreadIO`.

`findVcvars()` locates `vcvars64.bat` through `vswhere -latest -prerelease`, cached after the first
lookup, with hardcoded VS 18 / 2022 fallbacks.

```
cl /nologo /LD /std:c++20 /Zc:preprocessor /MD /Od /Zi /EHsc /arch:AVX2 /wd4100 /DSCRIPT_BUILD
   /I<Code/Script/Public> /I<Dependencies/Include>
   /FI ScriptAPI.h  /Tp <source>
   /link /DEBUG /INCREMENTAL:NO /PDB:<ping-pong name>
```

| Flag | Why |
|---|---|
| `/FI ScriptAPI.h` | The source is **body-only C++** — no `#include`, no `#define`. |
| `/Tp` | Compile as C++ **regardless of the extension**. |
| `/Zc:preprocessor` | The conformant preprocessor that `ScriptCtxMacros.h`'s variadic-arity dispatch needs. The main engine build gets it from the VS project settings; this standalone `cl` invocation does not. |
| `/Od /Zi /link /DEBUG` | A usable PDB: set breakpoints in the script, step, inspect locals. `/Od` is deliberate — scripts are tiny and it keeps line info intact. |
| `/DSCRIPT_BUILD` | Selects the layout-mirror `Entity` struct in ScriptAPI.h. |

Two mechanics worth knowing:

* **It builds to `<stem>.building.dll` and renames.** The live `<stem>.dll` is file-locked while
  loaded, so it cannot be overwritten directly; `getOrLoad` frees the old module, then renames the
  temp over it — leaving exactly one DLL per script.
* **The PDB name ping-pongs** (`pdbSerial`), with the absolute path embedded in the DLL, **so a
  rebuild never targets the PDB the debugger is currently holding open** (LNK1201). Superseded PDBs
  the debugger still holds go into `pendingPdbDeletes` and are retried later by `sweepPendingPdbs`.

Intermediates (`.obj` / `.lib` / `.exp` / the compiler PDB) are cleared; the build log lands in
`Assets/Local/Scripts/build.log`.

### The cooked alternative (`SCRIPTS_STATIC`, off by default)

`Code/AppScripts` generates ONE aggregate TU that `#include`s **every `Assets/Scripts/*.dsl`** into
its own namespace and compiles it into the engine, linking every engine library like App.

* **`.scr` is NOT cooked.** Leftover `.scr` assets are inert reference files, and one with stale
  generated C++ must not be able to fail the whole aggregate.
* A `.dsl` is includable because it holds real body-only C++ up top; the `//@@dsl ... //@@end` block
  below is ordinary comments to the compiler.
* Each file's trailing `REGISTER_*()` markers register the functions it defines, so **the aggregate
  needs no parsing.** They call `ocRegisterScriptEntry` into a Meyers-singleton registry that
  `getOrLoad` resolves against instead of invoking cl and LoadLibrary. Lookup is lenient: paths are
  forward-slash-normalized and either may be a tail of the other.
* **The real win is `SCRIPT_STATIC_BUILD`**: `ctx->foo(x)` becomes an INLINE FORWARDER to the engine's
  `thunk_foo(x)` instead of a function-pointer call, so `/GL` + `/LTCG` inlines it.
* All-or-nothing: one broken script fails the aggregate. Adding, renaming or removing a script needs a
  reconfigure; editing a body just recompiles the aggregate.
* Off (the default) it is an empty stub, so App's link line names it unconditionally.
* VS target `run-cooked` builds a sibling `Build-Cooked/` tree with `SCRIPTS_STATIC=ON`.

### `ScriptModule`

DLL entry pointers plus `dataSize`, `dataLayoutId`, `dataFields` / `numDataFields`,
`requiredComponents`, `eventNames`, the `faulted` flag, and `eventKeyToIndex` (filled by
`ScriptEventManager` on every reload).

**Do not cache function pointers across reloads.** `dataFields` points into the module's own static
storage and is valid exactly as long as the module.

## `ScriptAPI.h` — the host↔DLL ABI

[ScriptAPI.h](Public/ScriptAPI.h). **The one plain header in the codebase** (see Style), `#include`d
on both sides. C linkage; only glm vectors, PODs, floats and raw pointers cross.

### The function table is APPEND-ONLY

`ScriptContext` is generated from the `SCRIPT_CTX_FUNCS` X-macro list, where each row states its
parameters ONCE as `(type, name)` pairs and `ScriptCtxMacros.h` expands them into the declaration and
(for the cooked build) the forwarding call. **Capped at 8 parameters** — `audioTrigger`, the widest
today, uses 7; a 9th needs one more `_PARAMS`/`_ARGS` pair and a wider `SCRIPT_CTX_PICK`.

> **Cached DLLs must keep working, so entries only ever APPEND.** `networkEventSender` is zero-arg,
> which the macro list cannot express, so it is declared by hand **after** the generated pointers for
> exactly that reason. `log`/`logf` sit outside the list for their variadic signatures.

### The `Entity` mirror

Scripts see a layout-compatible mirror — `pos`, `scale`, `rot`, `parent` only — selected by
`SCRIPT_BUILD`. The offsets are static_asserted in `ScriptContext.cpp`.

Components a script cannot name are **opaque forward-declared handles** (`PhysicsComponent`,
`AudioComponent`, `ForceComponent`, `SceneComponent`, `LightComponent`, `AnimatorComponent`), passed
through to `void*` thunks. **An object pointer converts to `void*` implicitly, so no cast is needed at
the call site** — and `ScriptData` can cache a typed pointer per component with no layout to keep in
sync.

### `ScriptData` and the required-component slots

`ScriptRequiredComponents()` returns an `EComponentID` bitmask (from `//@@require`).

**The host allocates one 8-byte pointer slot per set bit at the FRONT of the `ScriptData` block, in
ascending bit order, and fills each straight off `getComponent<T>()` right after allocating — before
`OnSpawn` runs.** That is why `self.physics` emits as `scriptData-><name>` and reads an
already-resolved pointer instead of re-fetching the handle on every access.

> This is the mechanism behind the Entity checklist item *"a missing require-slot fill branch shifts
> every later slot"* — it crashed once.

`ScriptDataLayoutId()` is a hash of every field's **type and name, in order**. The host keeps an
existing data block across a hot-reload **only while this matches**; otherwise it drops the block —
freeing the arrays those `OcArray` handles owned — and starts from a zeroed one.

> **Size alone cannot tell a layout change from a no-op**: `int a; int b;` and `float a; float b;`
> are the same size, and reinterpreting one as the other silently corrupts every field, array handles
> included. Absent = treat every reload as a layout change.

`ScriptDataFields()` returns `OcScriptField[]` — **the only engine-side layout description**. Absent
or 0 for every script that never marks a field private/public.

### `OcArray`

The DSL's `T[]` is a plain POD id, so it can live in a persistent `ScriptData` block: **the ELEMENTS
live engine-side**, which means a hot-reload that keeps the block also keeps the contents with
nothing to copy, and **the script never owns heap memory.**

* 0 = no array yet, so a zeroed block is a set of empty arrays and the first push creates one lazily.
* **The id is generation-tagged engine-side**, so a stale or garbage value — a reinterpreted field
  after a layout change — fails lookup and every operation degrades to a no-op or default rather than
  touching memory.
* `OcArraySpan{ data, count }` resolves a POD array's storage ONCE for a `foreach`, so the loop
  indexes memory directly. `data` is null and `count` 0 for a stale, empty or non-POD array, or an
  `elemSize` mismatch, **so a loop bounded by `count` never dereferences null.** Only POD element
  kinds get a span — Entity elements are refcounted handles and String elements are engine-owned
  strings.
* **THE SPAN IS ONLY VALID WHILE THE ARRAY DOES NOT GROW.** See the container rule below.

## The DSL

`self.<component>` access is gated by `//@@require`; the full language is documented in
[`Code/DslCompiler/CONTEXT.md`](../DslCompiler/CONTEXT.md). Points that belong here:

* **`math.*` — ALL ANGLES IN DEGREES.** The `ocInverseLerp` / `ocRemap` / `ocMoveTowards` / ... helpers
  in ScriptAPI.h avoid double-evaluation. Randomness is one ENGINE-side generator, so hot-reload does
  not reset it.
* **Animation is the animator's surface** — `self.animator.setFloat/setBool/setTrigger/...`. Nothing
  names a clip.
* **A getter-call member MUST register `writable = false`**: a writable member's emit is used verbatim
  as an assignment target.
* **There is NO null.** Absent values are OPTIONAL types (`Entity?`), readable only through `ifexist`;
  dead entities in arrays report a lookup miss.

### `ScriptData` fields

`//@@data [private|public] <type> <name>`; default Hidden. Exposed fields surface through
`ScriptDataFields()`. **The Properties panel edits them LIVE and never serializes**; the Entity Editor
authors INITIAL values as text under `Component Script` / `Data` in a `.pre`, re-applied by
`applyInitialValues` after every `syncScriptData` reallocation.

### The container mutation rule

> **You cannot `push` / `clear` / `removeAt` a container while a `foreach` or `ifexist` is reading
> it — directly or through ANY chain of user function calls.**

`ScriptLoader::checkContainerMutations` enforces it as a whole-document **fixed-point over the call
graph**: a push through a parameter poisons the function for every caller (conservative on purpose).

**It is a SAFETY rule, not a diagnostic** — a POD `foreach` resolves the element storage once and
indexes it raw, so growth mid-loop would dangle that span.

It runs from BOTH `save()` and `load()`, deliberately at document level rather than as part of the
parse, **because the editor's save path never parses** — so no authoring route can produce a script
that breaks it. The editor-side grey-out (`ScriptLang::isContainerIterated`) is still syntactic, so
transitive mutation through a user function is authorable and gets refused at load.

`ref` is refused on handle types and non-writable containers; non-`ref` elements cannot be assigned.

### `//@@require` is enforced LIVE

`requirementsMet` (a mask test) runs at **every entry point**, not latched at spawn — hot-reload
mutates the module in place.

* `OnSpawn` runs once, the first frame the entity qualifies (`onSpawnRan`). **`OnDestroy` pairs with
  that flag, never with the live check.**
* `syncScriptData` re-allocates, refills the require slots and clears `onSpawnRan` when the module
  layout changed. `syncScriptDataLive` (every post-spawn entry point) **also re-registers the event
  listener**, whose stored pointer would otherwise dangle.

## Fault containment

`OC_SCRIPT_FAULT_CONTAINMENT` at the top of
[ScriptComponent.cpp](../Entity/Private/Components/ScriptComponent.cpp), default 1. **Set it to 0 for
raw calls, so faults crash normally and give clean dumps.**

Every entry-point call goes through SEH-guarded invokers — plain `__try` / `__except`, no C++
exceptions, **zero cost until a fault**. The `__try` helpers must stay free of objects with
destructors (C2712), so they only return the exception code and plain C++ wrappers do the logging.

**Caught** (`scriptFaultFilter`): access violation, in-page error, illegal instruction, array bounds
exceeded, privileged instruction, integer divide by zero (the common one), integer overflow
(`INT_MIN / -1`), and the six float faults.

> The float faults only exist if someone UNMASKS the FP control word. Masked — the default, and what
> the engine wants — float math produces inf/NaN and never raises.

**Deliberately NOT caught:** breakpoints and single-step stay with the debugger; C++ exceptions
(0xE06D7363) stay fatal as they are today; and **stack overflow stays fatal because the guard page is
spent** — continuing after it would fault unrecoverably anyway.

A fault marks the module `faulted` and logs. `requirementsMet` folds the flag in, so the script stops
running everywhere — **`OnDestroy` included, since its data may be half-written** — and
ScriptEventManager's direct `OnEvent` path checks it itself. A successful (re)load clears it, so
**F6 / Compile & Run is the recovery path.**

## The DSL subsystem

Under `Private/DSL/`, as partitions re-exported from `Public/ScriptHost.ixx`. **Editor-agnostic**:
nothing here depends on UI or ImGui, and UI's Script Editor panel is simply its only consumer today.

| Partition | Role |
|---|---|
| `:DSL` | The document data model — `DSLSymbol` structures, lines, `dataFields`, `eventNames`, `requiredComponents`. |
| `:ScriptLang` | Syntax, formatting and the autocomplete rules. |
| `:ScriptBindings` | **The engine-exposure registry.** `registerStruct` / `registerObject` / `registerComponentType` / `registerEntryPoint` are PUBLIC, so any library can expose its own surface. Each member carries an emit template: `$r` = the receiver's emitted expression, `$1..$n` = arguments in the callee's parameter order. |
| `:ScriptLoader` | Native save/load of the `.dsl` format, plus `checkContainerMutations`. |
| `:Transpiler` | Emits the C++. |

> Types an outside importer needs must be `export`ed — **partitions leak unexported types across
> same-module files, which is easy to miss.**

### The `.dsl` file format

**The format IS the editor's EXPANDED view.** `save()` writes the exact text `Syntax::format` renders
(fully typed), one line per `SyntaxLine`, each prefixed `//@` and bracketed by `//@@dsl <version>` ...
`//@@end`.

The whole DSL block therefore reads as C++ comments, **leaving the rest of the file free to hold the
transpiled C++ — one dual-purpose file.** The `@` keeps DSL lines distinguishable from the generated
code's own ordinary `//` comments, and the markers double that, since a DSL `end` line serializes as
`//@end`.

`load()` is the exact inverse. **References are stored by NAME, never by index**, which is what keeps
old files loadable as the builtin and sidebar lists evolve; names resolve in two passes so forward
calls work.

**Failure policy:** an editor-saved file always parses, so any error means hand-editing or version
drift — `load()` then **REFUSES the whole file** with a line-numbered error rather than constructing a
partial document. The same transactional contract is what lets the editor's paste offer a whole
candidate and simply cancel when it does not resolve. After a successful parse the document is
re-rendered and compared line by line; a mismatch still loads but logs a warning per differing line,
and the next save normalizes it.

### What the transpiler emits

**One function per DSL function, at file scope — no wrapper class.**

* `ctx`, `self` and `scriptData` are **auto-injected as every function's leading parameters**,
  invisible to the author at both the declaration and every call site. So `self.thing` and free engine
  calls transpile straight through the real ABI thunks instead of an intermediate wrapper object.
* `scriptData` is injected everywhere, not just into entry points, because `self.data` needs it in
  scope wherever it is dotted into. A non-empty field list emits a real `struct ScriptData { ... };`
  plus `ScriptDataSize()`, and `self.data` becomes `(*(ScriptData*)scriptData)`.
* **A DSL function named after one of the 5 entry points** — OnSpawn / OnDestroy / Update / OnEvent /
  OnPhysicsEvent — transpiles to its EXACT exported signature and gets its `REGISTER_*()` macro, so it
  IS a real loadable entry point. Every other function is a plain internal `static` helper.
* `OnEvent` additionally emits `ScriptEventCount` / `ScriptEventName` from the document's own event
  list, and `self.events.<name>` is that same list's index as a compile-time constant — **so the
  host's name→index resolution always agrees with what the body compares against.**
* Expression chains emit FLAT, exactly as authored: **C++'s own precedence supplies the `* /` over
  `+ -` the DSL defers to emit time**, with parentheses only where the author grouped them.
* Every function is forward-declared up front, **so call order in the `.dsl` never matters.**

## `DslCompiler`

```
DslCompiler <input> [output.dsl] [--compile]
```

`Code/DslCompiler`, a console exe, and **THE way Claude authors `.dsl` files** — see
[`Code/DslCompiler/CONTEXT.md`](../DslCompiler/CONTEXT.md) for usage and the full language reference.

Write only the DSL text; the tool validates it with the same loader and binding vocabulary the editor
uses, transpiles, and writes the exact file the editor's Save produces. `--compile` additionally runs
the output through the real ScriptHost pipeline — the F6 path — as the final check that it loads in
the engine as-is.

## `.scr` — legacy

A `.scr` IS the generated C++ (the same 5 entry points) with the node graph appended as `//@graph` /
`//@node` comments.

**The `.scr` / NodeEditor path is REMOVED from the running editor:** no node panel, no `.scr` open or
create routes. The `NodeEditor/` sources remain in the tree for reference and still compile as unused
partitions.

A `.scr` shows as a plain text file in Content, is not offered by script pickers, and is not cooked.
`ScriptHost` itself would still compile one if a `.pre` carried the path, but nothing authors that any
more.
