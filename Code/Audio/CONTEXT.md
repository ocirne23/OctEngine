# Audio

> Library documentation for `Code/Audio`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Core-only. Two third-party libraries, both PRIVATE — neither API leaks, because every public type
holds an opaque `uint64` handle:

* **miniaudio** — device, mixing, decoding. `MINIAUDIO_IMPLEMENTATION` lives in Audio.cpp.
* **Steam Audio** (`phonon`) — HRTF binaural. Static `phonon(d).lib`.

## `Globals::audio` (`AudioSystem`)

[System.ixx:11](Private/System.ixx#L11).

| Call | Notes |
|---|---|
| `initialize()` | Opens the default output device — 48 kHz stereo — then the Steam Audio context and HRTF (`IPL_SIMDLEVEL_AVX2`, `IPL_HRTFTYPE_DEFAULT`). Registers the `Audio/System → Master Volume` tweak. Returns false and logs on failure. |
| `update(camera[, listenerVelocity])` | **Once per frame, game thread.** Places the listener and recomputes every source's HRTF direction, distance attenuation and doppler. The audio thread reads the results through the node's atomics. |
| `loadSound(path)` | WAV / FLAC / MP3, relative to `Assets/`. |
| `createBuffer(format, pcm, rate)` | Procedural sounds. |
| `createSource()` | A free-standing voice. |
| `playOneShot(buffer, pos, gain, pitch)` / `playOneShot2D` | Fire-and-forget from a pool of `MaxOneShotSources` (32); steals the oldest slot when full. |
| `isInitialized()` | Everything no-ops before initialize. |

### Threading

`m_sourceMutex` ([System.ixx:49](Private/System.ixx#L49)) serializes **source list edits only**. They
come from parallel entity spawn/despawn jobs — an `AudioComponent`'s voices die with its entity — and
from script triggers on workers.

* The miniaudio / Steam Audio teardown of a released source runs **outside** the lock
  (`ma_sound_uninit` is thread-safe against the mixer).
* `update()` walks the list on main, in a window no create or release overlaps.

## Signal path

| Source kind | Route |
|---|---|
| Mono | `ma_sound` → a per-source `BinauralNode` → endpoint |
| Stereo, or `setRelative(true)` | Unspatialized passthrough (the node's `blend` atomic goes to 0) |

`BinauralNode` ([Engine.ixx:37](Private/Engine.ixx#L37)) is a custom miniaudio node: stereo in,
downmixed to mono, `iplBinauralEffectApply`, binaural stereo out. Steam Audio processes **fixed
`IplFrameSize` blocks (256 frames, ~5.3 ms at 48 kHz)** while the node graph asks for arbitrary frame
counts, so input and output are staged through one-block FIFOs inside the node.

The direction and blend are `oc::atomic<float>`s written by `updateSource` on the game thread and
read by the audio thread. **Only the HRTF direct path is used so far** — no occlusion, reflections or
reverb.

## Handles

`AudioBuffer` ([Buffer.ixx:7](Private/Buffer.ixx#L7)) and `AudioSource`
([Source.ixx:10](Private/Source.ixx#L10)) are move-only RAII, like `PhysicsMesh` / `PhysicsBody`.

* **A buffer must outlive every source playing it.** Destroying one stops and detaches those sources
  (`AudioSystem::detachBuffer`).
* Source controls: `setBuffer`, `play` / `pause` / `stop` / `isPlaying`, `setLooping`, `setGain`,
  `setPitch`, `setPosition`, `setVelocity` (doppler only — position is not integrated),
  `setRelative`.
* `setAttenuation(referenceDistance, maxDistance, rolloff)` is inverse-clamped: full volume inside
  the reference distance, no further attenuation past `maxDistance`.

## Gameplay integration (lives in Entity)

`AudioComponent` — `Component Audio` in a `.pre`. Each `Sound <alias>` entry holds one or more `Path`
clips, each with its own `Volume` / `Pitch` / `Loop` / `Relative` / `ReferenceDistance` /
`MaxDistance` / `Rolloff`, plus a `Select` mode that picks a clip per trigger:

`Single` (default) · `Random` · `RandomNoRepeat` · `Cycle` · `CycleStartRandom`

* Buffers are **World-cached and shared** between entities using the same file
  (`AudioComponent::Clip::buffer` is a `shared_ptr<AudioBuffer>`).
* `TriggerOverrides` let a trigger replace position, volume or pitch; **a set position pins the sound
  there instead of following the entity.**
* Playing spatial sounds otherwise follow the entity every update.
* Demo: `Entities/Debug/physicsCube.pre`.

**DSL surface** — `self.audio.trigger(string alias)` and `self.audio.stop(string alias)`, registered
in `registerScriptDslBindings` ([ScriptContext.cpp:338](../Entity/Private/ScriptContext.cpp#L338)).
They lower to the `ctx->audioTrigger` / `ctx->audioStop` ABI thunks; `audioTrigger` takes an
`overrideMask` (bit 0 position, bit 1 volume, bit 2 pitch) that the DSL binding passes as 0.

> The retired NodeEditor's "Trigger Audio" / "Stop Audio" nodes used the same thunks with a real
> `overrideMask`. Those sources still compile but are not reachable from the editor — see Script.
