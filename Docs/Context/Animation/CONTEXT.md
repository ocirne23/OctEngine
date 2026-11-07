# Animation

> Library documentation for `Code/Animation`.
> Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

CPU skeletal animation runtime. Core-only: it knows nothing about entities, assets or the GPU.
It samples clips into a **bone palette** (one `mat4` per bone) and hands that out; somebody else
uploads it.

## Where it sits in the pipeline

| Step | Owner | Code |
|---|---|---|
| Import a rig into a `Skeleton` | **File** (Assimp) | `SceneData::buildSkeleton` |
| Import an FBX's tracks as `ClipAsset`s (by node name) | **File** (Assimp) | `ISceneData::importClipAssets` — [ISceneData.ixx](../../../Code/File/Private/ISceneData.ixx) |
| Author / save `.clip` files | **UI** | The Animation Editor (see the [UI CONTEXT](../UI/CONTEXT.md)) |
| Keep the rig's own skeleton copy | **RendererVK** | `ObjectContainer::m_skeleton` — [ObjectContainer.ixx:149](../../../Code/RendererVK/Private/ObjectContainer.ixx#L149) |
| Cache clip assets by name, build clip libraries per skeleton + animator | **Entity** | `WorldState::getOrLoadClipAsset` / `getOrBuildClipSet` — [WorldState.ixx](../../../Code/Entity/Private/WorldState.ixx) |
| Drive the player each frame from an `.apl` graph | **Entity** | `AnimatorComponent` |
| Consume the palette for GPU skinning | **RendererVK** | `allocateSkinningPalette` / `setSkinningPalette` |

## Clips: one path from keys to the player

**Every clip the runtime plays is a `ClipAsset` first** ([ClipAsset.ixx](../../../Code/Animation/Private/ClipAsset.ixx)): keyframes by
BONE NAME, a duration, a step count (`steps`: the editor's key grid, the duration in that many equal steps - there is NO frame rate;
keys are in seconds and playback is continuous), a loop flag, events and the editor's preview rig. The importer sets the step count to the
file's frames (`duration * fps`). It knows no skeleton;
`buildAnimationClip(asset, skeleton, name)` resolves the track names into an `AnimationClip` (a track the rig has no bone for is left out).

| Source of a global clip name | How it becomes a `ClipAsset` |
|---|---|
| `<name>.clip` (the file stem is the name) | `parseClipAsset` of the file text |
| an `.anm` `Animation <name>` entry | `importClipAssets` of its FBX (the first track that passes `Skip` / `Track`), then the entry's `Loop` and `Event`s |

**A `.clip` wins over an `.anm` entry of the same name**, so converting an `.anm` clip in the Animation Editor replaces it at runtime with
no `.apl` change. `ISceneData::loadAnimations` (the old direct FBX-to-set call) is built on `importClipAssets` + `buildAnimationClip` too.

### The `.clip` text

```
Clip 1
    Duration 1.0333
    Steps 31                       the key grid: the duration in this many equal steps
    Loop true
    PreviewRig character
    Event footstep 0.25            name, normalized time (like .anm)
    Track "mixamorig:Hips"
        Position
            0 0 1.02 0             time (s), x y z
            0.5 0 1.04 0 Smooth    an optional Step / Smooth: how the key blends to the NEXT key (default Linear)
        Rotation
            0 0 0 0 1              time, quaternion x y z w
        Scale
            0 1 1 1
```

Indentation is for the reader: a key line belongs to the last channel keyword, a channel to the last `Track`. Floats are written in their
shortest exact form (`std::to_chars`), so a load + save round-trips bit for bit. `sortClipKeys` runs after the parse (keys closer than 1e-5 s
merge, the later one wins).

### Key interpolation (`KeyInterp`)

A key's mode sets the segment from it to the next key: **Linear** (lerp / slerp), **Step** (hold), **Smooth** (a cubic Hermite, Catmull-Rom
tangents over the neighbour key times, flat at the first and last key; rotations do it on the sign-aligned 4D components, then normalize).
The samplers are `AnimKeys::sampleVec3` / `sampleQuat` in `:Clip` — **inline, shared by `AnimationPlayer` and the editor's curve view**, so
what the editor draws is what plays. The segment search is a binary search (`AnimKeys::findSegment`).

### Key reduction

`reduceClipKeys(clip, tolerance)` drops each Linear key the line between its kept neighbours reproduces within the tolerance — checked at
EVERY key dropped since the last kept one, so the error cannot creep along a run of drops — and a channel that never moves keeps one key.
Step and Smooth keys stay. The importer runs it by default (FBX bakes a key per frame).

**Rigid-part models (no skin) are NOT in this library** — the box-limb character's bones and walk
are Entity's `HumanoidAnimatorComponent` (and the parked `SceneAnimatorComponent`). The one thing they take from
here is **`Animation:BoneMath`** ([BoneMath.ixx](../../../Code/Animation/Private/BoneMath.ixx)): the numeric kernels both
share — `polyTrig` / `polyTrig4` (sin + cos by polynomial, scalar and SSE), `phaseTrig` (a 0..1
phase folded onto the nearest quadrant axis, no CRT call), `phaseSin` (the sine alone, folded with
`min` and sign bits so it has NO branch; error < 4e-6), and `ParentCompose`
(`composeTransform(parent, local)` for many locals under one parent, prepared once, SSE).
**All of it is inline code in the module interface**: it compiles into the caller, so the sharing
adds no call. The `Transform` layout it relies on (32 bytes, two registers) is static_asserted
there.

**Retargeting is by BONE NAME**, done when a clip set is built: `buildAnimationClip` resolves every
track against the target skeleton, so a rig in one file and its animations in others (Mixamo exports)
work. A track that names a node the skeleton does not have is left out of the runtime clip.

## `AnimationPlayer` — the runtime workhorse

[AnimationClip.ixx:75](../../../Code/Animation/Private/AnimationClip.ixx#L75). One per animated entity. `tick(deltaSeconds)`
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
* `setTime(seconds)` scrubs the single-clip source and evaluates at once (no events, no crossfade) —
  the Animation Editor's preview. `getGlobalTransforms()` (model space, modifiers applied) and
  `getSampledLocalPose(bone)` (the sampled local TRS before the modifiers) expose the last evaluate.

### Programmatic bone posing

Sticky modifiers layered on top of the sampled pose, addressed by name or index, always in the bone's
LOCAL (parent-relative) space. They apply every tick until cleared.

| Call | Effect |
|---|---|
| `setBoneTransform(...)` | **Override** — replaces the bone's local transform entirely. |
| `setBoneOffset(...)` | **Additive** — post-multiplied onto the animated pose. The `quat` overload just rotates a bone relative to where the clip put it. |
| `setBoneModelRotation(index, q)` | **Model rotate** — turns the bone and its subtree about the bone's own origin by `q` in MODEL space (the skeleton root's frame), applied to the composed global transform. The caller needs no knowledge of the bone's local axes ("yaw the hips about up"). |
| `clearBoneModifier` / `clearBoneModifiers` | Remove them. |

`m_anyBoneModifier` short-circuits the whole path when nothing is posed.

## `AnimStateMachine`

[StateMachine.ixx:28](../../../Code/Animation/Private/StateMachine.ixx#L28). Drives an `AnimationPlayer`. Each state plays one
clip, or a blend space whose axis is bound to a named float parameter.

**Call order matters:** the owner sets parameters, calls `update(dt)` — which only changes *which*
source plays plus the blend axis — and then ticks the player.

* Parameters: `setFloat` / `setBool` / `setTrigger`, read back with `getFloat` / `getBool`.
* Conditions AND together: `floatGreater`, `floatLess`, `boolIs`, `trigger`.
  **Triggers are one-shot** — firing a transition consumes every trigger it tested
  ([StateMachine.cpp:103](../../../Code/Animation/Private/StateMachine.cpp#L103)).
* `addTransition(from, to, ...)` and `addAnyTransition(to, ...)` for an escape hatch such as death.
* Transitions are evaluated **in declaration order**, any-state ones included; the first whose source
  gate, `exitTime` gate and conditions all pass fires
  ([StateMachine.cpp:91](../../../Code/Animation/Private/StateMachine.cpp#L91)). A transition into the
  current state is skipped.
* `exitTime > 0` holds the transition until the current source reaches that normalized time.
* On the first `update` with no current state it enters the entry state (or state 0) and returns.

> `getCurrentStateName`'s `"<none>"` is a **namespace-scope** string, not a function-local static.
> The build is `/Zc:threadSafeInit-` and this is reachable from the script animator thunk on job
> workers — see Style in CLAUDE.md.

## Data types

| Type | Notes |
|---|---|
| `Skeleton` | Flattened hierarchy, **parent-before-child**, so one forward pass composes global transforms. EVERY source node becomes a bone, intermediate non-skinning nodes included, to keep the transform chain intact; non-skin bones keep an identity `inverseBind`. |
| `AnimationChannel` | Per-bone position / rotation / scale key tracks, sorted by time in seconds. Each key carries its `KeyInterp`. |
| `ClipAsset` / `ClipTrack` | The skeleton-free clip (see Clips above): tracks by bone name, the `.clip` text form. |
| `AnimationClip` | `duration`, channels, `loop` (false = one-shot: clamps and holds the last frame), and `events`. |
| `AnimationEventKey` | A notify at a normalized time 0..1. Authored in `.anm` as `Event <name> <normalizedTime>`; surfaces through `getFiredEvents()`, which the entity's script or animator `onEvent` consumes. |
| `AnimationSet` | Named clip collection sharing one skeleton, with a name → index map. Owns its clips. |
| `BlendSpace1D` | Samples kept sorted ascending by position; `addSample` inserts in order. |

Event detection handles wrap-around: `detectEvents` splits the interval across 1.0/0.0 when playback
looped during the tick ([AnimationClip.ixx:155](../../../Code/Animation/Private/AnimationClip.ixx#L155)).
A negative playback speed plays backward (the phase / time wraps the other way), and its events are tested over
`[cur, prev)` with the wrap reversed, so a reversed walk fires each footstep once.
