# Animation
Library documentation for `Code/Animation`. Read `.claude/CLAUDE.md` first (rules, building, style, dependency direction).

* Skeletal animation runtime (Core-only). Partitions: `Animation:Skeleton`, `Animation:Clip` (`AnimationClip`/`AnimationSet` + `AnimationPlayer`: CPU sampler producing bone palettes, crossfades, `BlendSpace1D`, per-bone posing via `setBoneTransform`/`setBoneOffset`), `Animation:StateMachine` (`AnimStateMachine`: parameter-driven states/transitions)
* File builds Skeleton/clips from Assimp; RendererVK consumes the palette for GPU skinning; Entity's AnimatorComponent + .apl assets drive it (World caches retargeted clip sets per skeleton+animator)
