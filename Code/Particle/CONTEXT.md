# Particle

> Library documentation for `Code/Particle`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

GPU particles plus projected decals.

The renderer side (`ParticlePipeline` / `DecalPipeline`, the GPU structs and the `MAX_*` constants in
Layout.ixx) lives in RendererVK. This library is the gameplay layer.

## `Globals::particleSystem`

* `initialize()` — before world spawns.
* `update(renderer, dt)` — AFTER `world.update`. It turns rates and bursts into GPU spawn requests,
  re-uploads every emitter config (so live `.pfx` edits apply retroactively), and ages and submits
  decals.

## Simulation (on the GPU)

Shaders: `particle_begin/emit/sim.cs.glsl`.

* One persistent pool (`MAX_PARTICLES` 256K), a dead-index stack, and two alive lists ping-ponged by
  frame parity.
* All dispatches are indirect, and the OUT alive count IS the draw's `instanceCount` — so spawning
  never re-records.
* **Sim** — gravity, drag, noise turbulence, plus optional screen-space collision against last
  frame's G-buffer.
* **Draw** — billboards in scene colour, in ONE pipeline: velocity stretch, flipbooks, per-particle
  GI + sun lighting, soft depth fade, premultiplied blend with per-emitter additivity
  (0 = smoke .. 1 = fire).
* Tweaks under `Particles/*`.

## Effects

* `ParticleEffectDesc` is a named set of emitter descs, loaded from `.pfx` text (grammar documented
  in Effect.ixx; demo `Assets/Effects/fire.pfx`) and cached by path.
* `createEffect(pathOrDesc, pos, rot)` returns a RAII `ParticleEffect`
  (`setTransform` / `setVelocity` / `setEmitting` / `burst`). Destroying it retires live particles
  through a KILL flag.
* The renderer slot API (`createParticleEmitter` and friends, `MAX_PARTICLE_EMITTERS` 256) is
  main-thread.
* Textures load through `Renderer::loadEffectTexture` into the bindless array.

## Decals

`spawnDecal(DecalDesc, pos, normal)` into an aging pool.

* Lifetime 0 = persistent; `removeDecal` fades out; the oldest non-persistent decal is evicted at
  `MAX_DECALS` 4096.
* Pushed per frame through the lock-free `Renderer::addDecal`.
* Drawn as ONE instanced draw of unit cubes after the opaque pass (front-cull, no depth test): the FS
  reconstructs the surface from G-buffer depth, projects into decal space (+Z axis), angle-fades and
  blends premultiplied. `DECAL_FLAG_LIT` approximates sun + GI.
* Being pure screen-space, decals wrap static AND skinned geometry.

## `ParticleComponent`

Authored as `Component Particle`: `Effect <path.pfx>`, optional `Emitting false`.
Demo: `Entities/Debug/particleFire.pre`.

It follows the entity transform and feeds a finite-difference velocity.
