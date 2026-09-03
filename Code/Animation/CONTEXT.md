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
