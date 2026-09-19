# Animation

> Library documentation for `Code/Animation`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

CPU skeletal animation runtime. Core-only: it knows nothing about entities, assets or the GPU.
It samples clips into a **bone palette** (one `mat4` per bone) and hands that out; somebody else
uploads it.

## Where it sits in the pipeline

| Step | Owner | Code |
|---|---|---|
| Import a rig into a `Skeleton`, and clips into an `AnimationSet` | **File** (Assimp) | `ISceneData::loadAnimations` — [ISceneData.ixx:80](../File/Private/ISceneData.ixx#L80) |
| Keep the rig's own skeleton copy | **RendererVK** | `ObjectContainer::m_skeleton` — [ObjectContainer.ixx:149](../RendererVK/Private/ObjectContainer.ixx#L149) |
| Cache clip libraries per skeleton + animator | **Entity** | `World::getOrBuildClipSet` — [World.ixx:238](../Entity/Private/World.ixx#L238) |
| Drive the player each frame from an `.apl` graph | **Entity** | `AnimatorComponent` |
| Consume the palette for GPU skinning | **RendererVK** | `allocateSkinningPalette` / `setSkinningPalette` |

Rigid child ENTITIES (no skin) do not go through this pipeline at all — see Procedural part animation.

**Retargeting is by BONE NAME**, done at import: `loadAnimations` resolves every channel against a
target skeleton, so a rig in one file and its animations in others (Mixamo exports) work. A channel
that names a node the skeleton does not have gets `boneIndex = -1` and is ignored.

## `AnimationPlayer` — the runtime workhorse

[AnimationClip.ixx:75](Private/AnimationClip.ixx#L75). One per animated entity. `tick(deltaSeconds)`
once per frame, then read `getPalette()`.

**The active source is either a single clip or a 1D blend space.** Switching sources CROSSFADES: the
outgoing pose is frozen into a snapshot and the new source blends in as `m_fade` ramps 0 → 1.

```
play(clip | "name", fadeSeconds)        playBlendSpace(space, fadeSeconds)
setBlendParameter(x)                    setSpeed(s) / setPaused(b)
getNormalizedTime()  -> 0..1 progress of the active source
getFiredEvents()     -> notifies crossed during the last tick()
```

* **Sampling is per-bone TRS**, so rotations slerp instead of matrix-lerping.
* A `BlendSpace1D` blends the two samples bracketing the parameter. Playback is
  **phase-normalized** — every clip shares a 0..1 phase advanced at the blended duration — so
  footfalls stay roughly aligned as the blend shifts.
* `setClipLibrary(set)` is what makes `play("name")` work.
* Bones a clip does not animate fall back to the bind pose.

### Programmatic bone posing

Sticky modifiers layered on top of the sampled pose, addressed by name or index, always in the bone's
LOCAL (parent-relative) space. They apply every tick until cleared.

| Call | Effect |
|---|---|
| `setBoneTransform(...)` | **Override** — replaces the bone's local transform entirely. |
| `setBoneOffset(...)` | **Additive** — post-multiplied onto the animated pose. The `quat` overload just rotates a bone relative to where the clip put it. |
| `clearBoneModifier` / `clearBoneModifiers` | Remove them. |

`m_anyBoneModifier` short-circuits the whole path when nothing is posed.

## Procedural part animation — `Animation:Procedural`

[Procedural.ixx](Private/Procedural.ixx). **A SEPARATE runtime, not a clip source**: no `Skeleton`, no
keys, no `AnimationPlayer`, no state machine. It moves RIGID PARTS (a box limb, a turret barrel) with
closed-form sines, and it is made for tens of thousands of instances. Entity's
`SceneAnimatorComponent` is the consumer; this library still knows nothing about entities.

```
value = layerWeight * amplitude * sin(2 pi * (harmonic * layerPhase + trackPhase))
```

* **`PartLayer`** — ONE phase + ONE weight. A **stride** layer (`cyclesPerMetre > 0`) advances its
  phase by the DISTANCE moved (feet do not slide) and takes its weight from the speed (1 at
  `fullSpeed`); a **timed** layer runs at `cyclesPerSecond` with a constant `weight`. Weights move
  at `fadeRate` per second. **A blend IS the weight** — there is no pose to blend. Any
  number of layers.
* **`PartTrack`** — a rotation about a fixed part-local axis AROUND THE BIND rotation, or a
  translation along a fixed offset. `harmonic` is 1 or 2 only: harmonic 2 comes from the double-angle
  identities, and the track phase is stored as sin/cos, so **a tick costs one `sin`/`cos` pair per
  ACTIVE LAYER, never per track.** The half-angle of the quaternion is a polynomial + normalize
  (swing capped at 180°).
* **`SceneAnimation`** — layers + parts + tracks (grouped by part). Immutable and shared: one per
  prefab. Only parts that a track moves are in it.
* **`SceneAnimatorState`** — the whole per-instance state: a vector with one `PartLayerState` per
  layer (`initialize(anim)` sizes it): phase, weight, `manualWeight` (`>= 0` replaces the layer's
  own weight rule — the gameplay override), and the layer's sin / cos of this tick, which
  `advanceParts` writes and `evaluatePart` reads.
* **`advanceParts(anim, state, dt, distance)`** returns FALSE when the pose is the same as
  after the last tick (every weight steady, and no active layer advanced — a unit that stands
  still). **The owner then skips the evaluation AND the writes.** A layer with weight 0 does not
  advance its phase.
* **`evaluatePart`** gives `outRot` when `part.rotates` and `outPos` when `part.translates` — the
  owner writes only those.
* **`SceneAnimationBuilder`** — `addLayer`, `swing`, `bob`, `walkCycle(WalkCycleParams)` (legs in
  opposite phase, each arm opposite to the leg on its side, an optional twice-per-cycle bob),
  `build()`. Parts come out with an identity bind; the owner calls `Part::setBind`.

## `AnimStateMachine`

[StateMachine.ixx:28](Private/StateMachine.ixx#L28). Drives an `AnimationPlayer`. Each state plays one
clip, or a blend space whose axis is bound to a named float parameter.

**Call order matters:** the owner sets parameters, calls `update(dt)` — which only changes *which*
source plays plus the blend axis — and then ticks the player.

* Parameters: `setFloat` / `setBool` / `setTrigger`, read back with `getFloat` / `getBool`.
* Conditions AND together: `floatGreater`, `floatLess`, `boolIs`, `trigger`.
  **Triggers are one-shot** — firing a transition consumes every trigger it tested
  ([StateMachine.cpp:105](Private/StateMachine.cpp#L105)).
* `addTransition(from, to, ...)` and `addAnyTransition(to, ...)` for an escape hatch such as death.
* Transitions are evaluated **in declaration order**, any-state ones included; the first whose source
  gate, `exitTime` gate and conditions all pass fires
  ([StateMachine.cpp:91](Private/StateMachine.cpp#L91)).
* `exitTime > 0` holds the transition until the current source reaches that normalized time.
* On the first `update` with no current state it enters the entry state (or state 0) and returns.

> `getCurrentStateName`'s `"<none>"` is a **namespace-scope** string, not a function-local static.
> The build is `/Zc:threadSafeInit-` and this is reachable from the script animator thunk on job
> workers — see Style in CLAUDE.md.

## Data types

| Type | Notes |
|---|---|
| `Skeleton` | Flattened hierarchy, **parent-before-child**, so one forward pass composes global transforms. EVERY source node becomes a bone, intermediate non-skinning nodes included, to keep the transform chain intact; non-skin bones keep an identity `inverseBind`. |
| `AnimationChannel` | Per-bone position / rotation / scale key tracks, sorted by time in seconds. |
| `AnimationClip` | `duration`, channels, `loop` (false = one-shot: clamps and holds the last frame), and `events`. |
| `AnimationEventKey` | A notify at a normalized time 0..1. Authored in `.anm` as `Event <name> <normalizedTime>`; surfaces through `getFiredEvents()`, which the entity's script or animator `onEvent` consumes. |
| `AnimationSet` | Named clip collection sharing one skeleton, with a name → index map. Owns its clips. |
| `BlendSpace1D` | Samples kept sorted ascending by position; `addSample` inserts in order. |

Event detection handles wrap-around: `detectEvents` splits the interval across 1.0/0.0 when playback
looped during the tick ([AnimationClip.ixx:155](Private/AnimationClip.ixx#L155)).
