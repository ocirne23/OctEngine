# Particle

> Library documentation for `Code/Particle`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

GPU particles plus projected decals.

**This library is only the gameplay/authoring layer.** The GPU side — `ParticlePipeline`,
`DecalPipeline`, the GPU structs and the `MAX_*` caps in
[Layout.ixx](../RendererVK/Private/Layout.ixx) — lives in RendererVK.

## `Globals::particleSystem`

[System.ixx:46](Private/System.ixx#L46).

* `initialize()` before world spawns (currently empty, but keep the call — it is the init seam).
* **`update(renderer, dt)` once per frame on the main thread, AFTER `world.update` and before
  present.** It advances rate accumulators and bursts into renderer spawn requests, **re-uploads
  every emitter's GPU config** — which is why a live `.pfx` edit applies retroactively — and ages and
  submits decals. It is the ONLY place renderer emitter state is written.

### Threading

`ParticleEffect::setTransform` / `setVelocity` write only their own instance's slot, so they are
**safe from the parallel entity pass** (one writer per instance) and stay lock-free.

Everything else takes `m_effectMutex` ([System.ixx:112](Private/System.ixx#L112)), because effect
create/destroy runs concurrently from parallel spawn jobs:

| Path | Locking |
|---|---|
| `createEffectInstance` | Builds the whole instance UNLOCKED — `getTexture` and the renderer slot API lock internally — and takes the mutex only for the id mint plus the `m_effects` push. |
| `getEffectDesc` | Double-checked: the `.pfx` file load runs outside the lock. A rare same-path race loads twice and the second `emplace` yields, costing only the extra IO. |
| `getTexture` | **Whole-body lock on purpose.** A racing double-load would mint an orphan texture slot that cannot be freed without `TextureManager::free`'s GPU-idle contract. |
| `setEmitting` | Locked — it is called at component spawn, which may run on a spawn job while another job's `createEffect` grows `m_effects` under `findEffect`. |

Descs are held as `shared_ptr<const>` by each instance, so `invalidateEffect` cannot dangle a live
one.

## `ParticleEffect` — the handle

Move-only RAII, like `PhysicsBody` / `RenderNode`. Destroying it **retires** the instance's live
particles over the next frames (a KILL flag), rather than popping them.

```
setTransform(pos, rot)   setVelocity(v)      // pass-safe
setEmitting(b)  isEmitting()                 // pauses continuous rates; bursts still fire
burst()                                      // queues every emitter's Burst count this frame
```

`createEffect` takes either a cached `.pfx` path or an in-code `ParticleEffectDesc` (copied). One
instance holds **one renderer emitter slot per emitter in its desc**; `MAX_PARTICLE_EMITTERS` is 256,
and running out logs and skips that emitter.

## Simulation — on the GPU

Shaders `particle_begin.cs.glsl` → `particle_emit.cs.glsl` → `particle_sim.cs.glsl`.

* ONE persistent pool, `MAX_PARTICLES` = 256 K at 48 B each, with a dead-index stack and two alive
  lists ping-ponged by frame parity.
* **All dispatches are indirect, and the OUT alive count IS the draw's `instanceCount`** — so
  spawning never re-records command buffers.
* Sim: gravity, drag, noise turbulence, and optional screen-space collision against last frame's
  G-buffer (`Particles → Depth collision`).
* Draw: billboards in scene colour in ONE pipeline — velocity stretch, flipbooks, per-particle
  GI + sun lighting, soft depth fade, and premultiplied blend with per-emitter `additivity`
  (0 = smoke .. 1 = fire). `cullMode = None`, depth test on, **depth write off**.
* It is the `"Particles"` scene stage inside "Scene forward", after Force union blend and before
  Fog apply ([Renderer.cpp:3526](../RendererVK/Private/Renderer.cpp#L3526)).
* Tweaks under `Particles/*`: Enabled, Depth collision, Time scale, Log stats (plus the rain
  occlusion trio, see below).

## Weather volumes (rain / snow)

An emitter with a non-zero `Volume x, y, z` (box half extents) is a WEATHER VOLUME
(`PARTICLE_FLAG_VOLUME`): the emit pass spawns uniformly inside the box, the sim WRAPS a particle
that leaves a face back in through the opposite one (`particleVolumeWrap`) and never ages it out, so
`Count` particles fill the box once (spawned over the first frames - half the frame's spawn cap per
volume) and stay. The draw pins the life fraction at 0.5 (mid colour/size) and replaces the fade
envelope with an XZ edge fade over the outer 20 % of the box, so the side wrap seam never pops.

* `FollowCamera true` — the box rides `Renderer::cameraPos()` + `Offset` with an identity rotation
  (the wrap is per world axis), so the instance transform is ignored and the box never drains behind
  a moving camera. Put the box BELOW the camera (`Offset 0, -10, 0`) so the streaks fall through the
  view.
* `Occlude true` — the shelter test: `update` unions every occluding box and hands it to
  `Renderer::setRainOcclusionVolume`; the sim samples the renderer's top-down RAIN OCCLUSION MAP
  (see RendererVK) and a particle deeper than the roof surface at its XZ (by more than
  "Particles/Rain occlusion bias") restarts at the box top at a FRESH RANDOM XZ - the same XZ would
  drop it straight back onto the roof, and a box top that is itself indoors would pin it there.
  Tweaks `Particles/Rain occlusion*`.
* `WindResponse <1/s>` — the horizontal velocity relaxes onto the renderer's WEATHER WIND
  (`Particles/Wind speed / angle / gust strength / gust size / sheet contrast / sheet size / sheet
  drift`, `Ubo::weatherWind0/1/2`) at that rate; heavy drops ~1, flakes ~4, 0 = ignores wind. The
  local wind is the mean PLUS a 2D gust vector of "gust strength" m/s from two large-scale noise
  fields (`weatherWindAt`, particle.inc.glsl) - absolute, so flurries exist in calm air. The fields
  travel along the wind direction at "sheet drift" plus half the wind speed (a gust front sweeps
  even in light wind), so they pack the drops into moving bands = the density waves of a storm;
  `weatherSheet` additionally multiplies the drawn alpha by a third travelling field (sheet contrast)
  for visible curtains. A storm: speed 15, gust strength 8, sheet contrast 0.7, sheet drift 6.
* No terrain floor: a drop below the ground is hidden by the depth test until it wraps, which is
  correct for every camera that cannot see under the terrain. Splashes at the impact point are the
  natural follow-up (the depth collision already reports the hit).

Testbed toggles: `Particles/Rain` and `Particles/Snow` (ParticleSystem tweaks) create/destroy one
camera-following instance of `Effects/rain.pfx` / `Effects/snow.pfx`. The handles are DETACHED, not
destroyed, in `~ParticleSystem` - the plain-XCU particle system outlives the renderer.

## `.pfx` effects

A `ParticleEffectDesc` is a named set of `ParticleEmitterDesc`s — fire = flames + smoke + embers.
`loadParticleEffect(path, ...)` parses the text asset; the **full grammar is documented at
[Effect.ixx:92](Private/Effect.ixx#L92)**, every entry optional. Demos: `Assets/Effects/fire.pfx`,
`rain.pfx`, `snow.pfx`.

Emitter fields group as **appearance** (texture, flipbook cols/rows/fps, colorStart/End, additivity,
fadeIn, fadeOutStart, softFadeDistance, lit, emissiveFloor), **spawning** (rate, burst, spawnRadius,
spawnShell, coneAngleDeg, speedMin/Max, localOffset, localDirection, inheritVelocity), **weather
volume** (volume, count, followCamera, occlude - see above), **motion** (lifeMin/Max, gravity, drag,
turbulence + frequency + scroll, collide, collisionBounce) and **shape over life** (sizeStart/End,
sizeVariance, velocityStretch, spinMax, randomRotation).

An empty `texturePath` renders a procedural soft round sprite (`PARTICLE_TEX_NONE`). Textures load
through `Renderer::loadEffectTexture` into the bindless array, cached per path.

## Decals

`spawnDecal(DecalDesc, pos, normal)` — pos is the hit point, normal the surface normal. Returns an id,
or 0 if the spawn was dropped.

* Aging pool: `lifetime` 0 = persistent; `removeDecal` turns it finite so it fades out over
  `fadeOutTime`; the oldest **non-persistent** decal is evicted at `MAX_DECALS` = 4096.
* Pushed per frame through the lock-free `Renderer::addDecal`.
* Drawn as ONE instanced draw of unit cubes — the `"Decals"` scene stage, right after static meshes.
  `cullMode = Front`, **depth test and write both off**, premultiplied blend.
* The FS reconstructs the surface from G-buffer depth, projects into decal space (+Z axis),
  angle-fades (`angleFadeDeg` / `angleFadeWidth`) and blends. `DECAL_FLAG_LIT` approximates sun + GI.
* Being pure screen-space, decals wrap static AND skinned geometry.
* Tweak `Decals → Enabled`.

## `ParticleComponent` (lives in Entity)

`Component Particle` in a `.pre`: `Effect <path.pfx>`, optional `Emitting false`.
Demo: `Entities/Debug/particleFire.pre`.

It follows the entity's world transform every update and feeds a **finite-difference velocity**
(`lastPos` → current) to the emitters, which drives velocity inheritance and stretch.
