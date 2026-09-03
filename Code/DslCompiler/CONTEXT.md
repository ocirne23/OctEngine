# DslCompiler — command-line DSL → .dsl compiler

> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.
> The ScriptHost side is in [`Code/Script/CONTEXT.md`](../Script/CONTEXT.md).

The way to author `.dsl` scripts WITHOUT hand-writing the transpiled C++.

You write only the DSL text; the tool validates it with the exact same loader the Script Editor uses,
transpiles it, and writes the dual-purpose `.dsl` file — generated C++ on top, plus the
`//@`-commented DSL block below — byte-identical to what the editor's Save button produces. The engine
then compiles and hot-reloads that `.dsl` like any script (F6, Script panel).

## Contents

* [Usage](#usage) · [Raw input format](#raw-input-format) · [Directives](#directives) ·
  [Entry points](#entry-points)
* [Language syntax](#language-syntax) — [functions and expressions](#functions-variables-expressions),
  [calls](#calls), [control flow](#control-flow), [arrays](#arrays-t)
* [Binding surface](#binding-surface-what-scripts-can-touch) — [self](#self-entity), [world](#world),
  [math](#math-all-angles-in-degrees), [hud](#hud), [free functions](#free-functions),
  [struct methods](#struct-methods)
* [Using the result](#using-the-result) · [Worked example](#worked-example)

## Usage

```
DslCompiler <input> [output.dsl] [--compile]
```

* Built to `Build/Code/DslCompiler/<Config>/DslCompiler.exe`. Build with `cmake --build Build --config Debug --target DslCompiler`.
* Relative paths resolve against `Assets/` (`FileSystem::initialize` runs, same as the engine).
* `output` defaults to the input path with its extension replaced by `.dsl`.
* Input is either RAW DSL text (below) or an existing `.dsl` (detected by its `//@@dsl` line) — the latter
  re-saves it normalized with the C++ regenerated, so `DslCompiler Scripts/Foo.dsl` refreshes a file in place.
* Errors print as `<input>(<line>): <what>` and exit code 1. Success prints `wrote <output>`, exit code 0.
* `--compile` (or `-c`): after writing, run the output through the REAL ScriptHost pipeline — vcvars64 + cl
  into `Assets/Local/Scripts/`, then LoadLibrary. The exact path the engine's F6 takes, so a green result
  means the script loads in the engine as-is. On success it prints the DLL path, the entry points found,
  the ScriptData size, the required-component mask and the event names; on failure cl's own error log
  prints and the exit code is 1. Side benefit: the DLL is mtime-stamped like an engine build, so the engine
  reuses it instead of recompiling. Use this as the final check after authoring a script.
* The tool initializes NO engine system; it exits via `_Exit` (global dtors assume the engine teardown order).

## Raw input format

Plain text. Directives use a SINGLE `@`; code lines are indented with TABS; `#` starts a comment line. No
markers, no `//@` prefixes:

```
@require PhysicsComponent
@data private int num
@event OnHit

function OnSpawn()
	printf(string format = "hello")
end

function Update(float deltaSeconds)
	if self.data.num > 0
		self.physics.setVelocity(vec3 velocity = vec3(0, 1, 0))
	end
end
```

The wrap step inserts `//@` at the start of EVERY line and brackets the result with `//@@dsl 1` / `//@@end`.
That single rule is why directives are written with one `@`: `@require` becomes the stored `//@@require` form.
Consequences:

* Do NOT write `@dsl` or `@end` marker lines yourself — the wrapper owns those (`@end` would terminate the
  block early).
* Indentation must be TABS (a leading space would end up inside the line body and fail tokenization).
* Indentation is informational; structure comes from the block keywords (`function`/`if`/... and `end`). The
  loader re-renders the document and warns on drift, so keep indentation correct anyway.

## Directives

| Directive | Meaning |
|---|---|
| `@require <Component>[, <Component>...]` | Components this script needs on its entity. Gates `self.<component>` access — using `self.physics` without requiring `PhysicsComponent` is a load error. The script only runs on entities that have every required component (`requirementsMet`). Names: `SceneComponent`, `RenderComponent`, `AnimatorComponent`, `PhysicsComponent`, `AudioComponent`, `ParticleComponent`, `ForceComponent`, `LightComponent`. |
| `@data [private\|public] <type> <name>` | One persistent per-instance field, read/written as `self.data.<name>`. Types: `int`, `float`, `bool`, `string`, `vec2/3/4`, `quat`, and arrays (`int[]`, `float[]`, `bool[]`, `string[]`). `Entity`/`Entity[]` are NOT storable. `private`/`public` exposes the field to the editor's Properties panel / Entity Editor (arrays can't be exposed); omitted = hidden. |
| `@event <Name>` | One named script event. Declares the constant `self.events.<Name>` (its index) and subscribes this script's `OnEvent` to the global event of that name (`fireEvent`/`world.sendEvent`). |

## Entry points

A function named after an entry point becomes a real exported one; its parameters are LOCKED (write them
exactly as shown). Everything else is an internal helper function.

```
function OnSpawn()                              # once, first frame the entity qualifies (runs even frozen)
function Update(float deltaSeconds)             # per frame
function OnDestroy()                            # pairs with OnSpawn
function OnEvent(int eventIdx)                  # compare eventIdx against self.events.<Name>
function OnPhysicsEvent(int begin, int sensor)  # contact/sensor events (needs ContactEvents/Sensor on the shape)
```

## Language syntax

### Functions, variables, expressions

```
function name([ref] <type> <paramName>, ...) [-> <returnType>]
	<type> <name> [= <value>]        # declaration; every name is unique per function (no shadowing)
	name = <value>                   # assignment; also += -= *= /= %=
	self.pos.y += 1                  # member-chain assignment (every hop must be writable)
	return [<value>]
	break
end
```

* Types: `int`, `float`, `bool`, `string`, `vec2`, `vec3`, `vec4`, `quat` (plus binding types below).
  `Entity` is not declarable on its own — entities arrive as parameters, `foreach`/`ifexist` bindings.
* Operators: `+ - * / %`, comparisons `== != < > <= >=` (one per expression — chain with parentheses),
  logical `&& ||`. No unary `!`; compare `== false`. Expression chains are FLAT; parentheses are the only
  nesting and are preserved verbatim.
* Literals: `1`, `1.5`, `-1`, `true`, `false`, `"text"`. A numeric literal adopts the type of the slot it
  sits in.
* There is NO null and NO index syntax (`a[i]` does not exist) — optionals and containers go through
  `ifexist`/`foreach` (below).
* Reserved words (cannot be names): the keywords, plus `ctx`, `self`, `ScriptData`, `scriptData`.

### Calls

```
test(int awa = 123)                              # user function, named args: <type> <name> = <value>
self.physics.applyImpulse(vec3 impulse = v)      # dotted call on a binding object
math.clamp(x, 0, 1)                              # positional also parses; named is the normalized form
vec3(0, 1, 0)                                    # struct constructors are positional
printf(string format = "n=%d", self.data.num)    # variadic: extra args after the declared ones
```

The editor's saved form writes named arguments (`type name = value`); positional input is accepted and
normalizes on save. A `ref` parameter receives an output — only a bare variable can be passed there.

### Control flow

Every block closes with `end`. `if`/`elseif`/`else` chains share one `end`.

```
if <bool>            / elseif <bool> / else
while <bool>
for int i = 0, i < 10, i += 1        # exactly three comma-separated clauses: declaration, condition, increment
foreach [ref] <elemType> x in <sequence>
ifexist [ref] <type> x in <source> [at <key>]
```

`ifexist` is the single checked lookup — the only way to read an element, an optional, or another entity's
component; it may chain an `else` (but never `elseif`):

```
ifexist Entity e in world.spawn(string assetPath = "Entities/x.pre", vec3 position = p)   # optional proven
ifexist ref int v in self.data.list at 3                                                  # array element at index
ifexist PhysicsComponent pc in otherEntity                                                # component off an entity
	pc.applyImpulse(vec3 impulse = vec3(0, 5, 0))
else
	printf(string format = "no physics")
end
```

`ref` binds the element writable (write-back at block end); without it the binding is a COPY and assigning to
it is refused. `ref` is refused on handle types (`Entity` — writes already go through the handle) and on
read-only containers (`scene.children`, `world.entitiesInRadius`).

RULE: you cannot `push`/`clear`/`removeAt` a container while a `foreach`/`ifexist` is reading it — directly or
through any chain of user function calls. The loader refuses the whole file (this is a safety rule: POD
foreach resolves the storage once; growth would dangle it).

### Arrays (`T[]`)

Declared as `@data` fields; storage lives engine-side and survives hot-reload. Surface:
`.push(value)`, `.clear()`, `.removeAt(index)` (out of range = no-op), member `.count`; read via
`foreach`/`ifexist ... at <index>`.

## Binding surface (what scripts can touch)

The AUTHORITATIVE registry is `registerScriptDslBindings()` in `Code/Entity/Private/ScriptContext.cpp` —
when in doubt (exact parameter names, new bindings), read it. Summary:

### self (Entity)
Members: `pos` (vec3), `scale` (float), `rot` (quat) — all writable; `parent` (`Entity?`, via `ifexist`);
`data` (own fields), `events` (own event indices) — both legal ONLY directly on `self`.
Functions: `setEnabled(bool enabled)`.
Components (need `@require`; legal only on `self` — reach another entity's via `ifexist <Component> c in e`):

* `self.scene` — `children` (Entity sequence), `getNumChildren()`, `findChild(name)` → `Entity?`,
  `getChild(index)`, `addChild(entity)`, `removeChild(entity)`, `removeChildIdx(index)`
* `self.physics` — reads (members): `velocity`, `angularVelocity` (deg/s), `mass`, `centerOfMass`,
  `position` (simulated pose), `awake`, `gravityScale`, `linearDamping`, `angularDamping`. Writes
  (functions): `setVelocity`, `setAngularVelocity(degreesPerSecond)`, `setAwake`, `applyImpulse[AtPoint]`,
  `applyAngularImpulse`, `applyForce[AtPoint]` (per-step, re-apply every Update), `applyTorque`,
  `setGravityScale`, `setLinearDamping`, `setAngularDamping`, `teleport(position, eulerDeg)` (the ONLY way
  to move a body; lands next physics update), `getPointVelocity(worldPoint)`
* `self.animator` — `setFloat/setBool/setTrigger/getFloat/getBool(name, ...)`, `setEnabled`; members
  `state` (string), `timeInState`, `speed`, `enabled`
* `self.render` — `setOffsetPos/Scale/Rot` (per-instance offset relative to the entity); members
  `boundsRadius`, `boundsCenter`, `visible` (hint), `skinned`, `offsetPos/Scale/Rot`
* `self.audio` — `trigger(alias)`, `stop(alias)` (aliases from the .pre's Component Audio)
* `self.particle` — `burst()`, `setEmitting(bool)`; member `emitting`
* `self.force` — setters `setOutput/setReach/setFocus/setDistribution/setWidth/setTeam/setDirection/
  setOffset/setCentered`; matching read members plus `appliedForce`/`pressure` (GPU readback, ~2 frames old).
  No on/off — silence with `setOutput(0)`
* `self.light` — member `lights` (writable `Light` sequence: `foreach ref Light l in self.light.lights`,
  `ifexist ref Light l in self.light.lights at i`), member `count`. `Light` struct members: `enabled`,
  `color`, `intensity`, `range`, `offset`, `direction`, `coneAngle`, `edgeSoftness`, `width`, `height`,
  `length`, `rotation`; helpers `setSpotCone`, `setAreaSize`, `setTubeShape`, `tubeRadius`. Edits land via
  the `ref` write-back

### world
`spawn(assetPath, position)` → `Entity?` (returns the miss branch on the spawning frame — spawns are
deferred), `destroy(entity)`, `findRootEntity(displayName)` → `Entity?`, `entitiesInRadius(position, radius)`
(foreach-able), `nearestEntity(position, maxRadius, exclude)` → `Entity?`, `rayCast(origin, direction,
maxDistance)` → optional `RayHit` (`point`/`normal`/`distance`, via `ifexist RayHit h in ...`),
`rayCastDistance(...)` → float, `sendEvent(eventName)`, `sendEventTo(entity, eventName)`,
`sendNetworkEvent(eventName)`, `networkEventSender()`, `setSun(direction, color, intensity)`,
`spawnPointLight(position, range, color, intensity)`, `setGravity(gravity)`, `drawLine(from, to, color)`
(this frame only). Members: `sunDirection`, `time` (seconds since startup), `camera` (→ read-only
`position`, `direction`, `up`, `right`, `fovDeg`, `nearPlane`, `farPlane`).

### math (ALL ANGLES IN DEGREES)
`abs sign min max clamp saturate sqrt pow exp log log2 mod`, `floor ceil round trunc fract toInt`,
`lerp inverseLerp remap step smoothstep moveTowards`, `sin cos tan asin acos atan atan2 toRadians toDegrees`,
`wrapAngle deltaAngle lerpAngle moveTowardsAngle`, `randomFloat randomInt randomUnitVector` (engine-side
stream, hot-reload safe), `quatFromEuler(eulerDeg) quatFromAxisAngle(axis, angleDeg) quatIdentity()
quatLookRotation(forward, up)`. Members: `pi`, `tau`, `epsilon`.

### hud
`setSlot(slot, label, count)`, `setSlotCount`, `clearSlot`, `selectSlot`, `setHotbarVisible`,
`setBar(name, value, maxValue, color)`, `removeBar`, `setCounter(name, value, decimals, color)`,
`removeCounter`, `clear()`; member `selectedSlot`. Slot keys fire the global "Hotbar Select" event.

### Free functions
`printf(format, ...)` (engine log), `isKeyDown(keyName)`.

### Struct methods
* vec2/3/4: `length lengthSquared normalized dot distance distanceSquared scaled(factor) negated
  lerp(target, t) min max clamp abs floor ceil round`; vec2 adds `angleDeg perpendicular`; vec3 adds
  `cross reflect(normal) refract(normal, eta) xy`; vec4 adds `xy xyz`. Scale by scalar is `.scaled(f)` —
  `v * 2` does NOT parse (a chain's terms share one type).
* quat: `length normalized dot inverse conjugate then(next)` (a.then(b) = a first), `slerp(target, t)`,
  `rotate(v)` → vec3, `euler` → vec3 degrees, `axis`, `angleDeg`.

## Using the result

Reference the `.dsl` from a prefab: `Component Script` / `Path Scripts/YourScript.dsl` in the `.pre`
(see `Assets/Entities/` for examples), or set it on an entity in the editor's Properties panel. Hot-reload:
F6 / the Script panel's Compile & Run.

## Worked example

Raw input (`@require` + data + event) and what it can do:

```
@require PhysicsComponent, AudioComponent
@data private float cooldown
@data int[] hits
@event Explode

function Update(float deltaSeconds)
	self.data.cooldown = math.max(self.data.cooldown - deltaSeconds, 0)
	ifexist Entity target in world.nearestEntity(vec3 position = self.pos, float maxRadius = 10, Entity exclude = self)
		vec3 toTarget = target.pos - self.pos
		if toTarget.length() < 2 && self.data.cooldown == 0
			world.sendEventTo(Entity entity = target, string eventName = "Explode")
			self.data.cooldown = 1.5
		end
	end
end

function OnEvent(int eventIdx)
	if eventIdx == self.events.Explode
		self.audio.trigger(string alias = "Boom")
		self.physics.applyImpulse(vec3 impulse = math.randomUnitVector().scaled(float factor = 8))
	end
end
```

Note `target.pos` works: `pos`/`scale`/`rot`/`setEnabled` are Entity surface, valid on any entity binding.
Only `data`/`events`/components are self-only.
