# Animation

> Library documentation for `Code/Animation`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Skeletal animation runtime. Core-only.

## Partitions

| Partition | Contents |
|---|---|
| `Animation:Skeleton` | `Skeleton` |
| `Animation:Clip` | `AnimationClip` / `AnimationSet`, plus `AnimationPlayer` — the CPU sampler that produces bone palettes: crossfades, `BlendSpace1D`, per-bone posing via `setBoneTransform` / `setBoneOffset` |
| `Animation:StateMachine` | `AnimStateMachine` — parameter-driven states and transitions |

## Consumers

* **File** builds the skeleton and the clips from Assimp.
* **RendererVK** consumes the bone palette for GPU skinning.
* **Entity**'s `AnimatorComponent` plus the `.apl` assets drive it.
  World caches retargeted clip sets per skeleton + animator.
