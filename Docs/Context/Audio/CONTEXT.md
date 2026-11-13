# Audio

> Library documentation for `Code/Audio`.
> Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

Links Core and Settings (the master volume tweak). Two third-party libraries, both PRIVATE —
neither API leaks, because every public type holds an opaque `uint64` handle:

* **miniaudio** — device, mixing, decoding. `MINIAUDIO_IMPLEMENTATION` lives in Engine.cpp.
* **Steam Audio** (`phonon`) — HRTF binaural. Static `phonon(d).lib`.

## `Globals::audio` (`AudioSystem`)

[System.ixx:11](../../../Code/Audio/Private/System.ixx#L11).

| Call | Notes |
|---|---|
| `initialize()` | Opens the default output device — 48 kHz stereo — then the Steam Audio context and HRTF (`IPL_SIMDLEVEL_AVX2`, `IPL_HRTFTYPE_DEFAULT`). Applies the `Audio/System → Master Volume` tweak (`Globals::settings.audio.masterVolume`, registered in `Code/Settings`) and attaches its listener. Returns false and logs on failure. |
| `update(camera[, listenerVelocity])` | **Once per frame, game thread.** Places the listener and recomputes every source's HRTF direction, distance attenuation and doppler. The audio thread reads the results through the node's atomics. |
| `loadSound(path, fileBytes)` | Decodes a WAV / FLAC / MP3 file's bytes. Audio does not link File: the caller reads them through `FileSystem` (so packs work); `path` names the sound in the log. |
| `createBuffer(format, pcm, rate)` | Procedural sounds. |
| `createSource()` | A free-standing voice. |
| `playOneShot(buffer, pos, gain, pitch)` / `playOneShot2D` | Fire-and-forget from a pool of `MaxOneShotSources` (32): the first idle slot, else a round-robin steal. |
| `isInitialized()` | Everything no-ops before initialize. |

### Threading

`m_sourceMutex` ([System.ixx:50](../../../Code/Audio/Private/System.ixx#L50)) serializes **source list edits only**. They
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

`BinauralNode` ([Engine.ixx:37](../../../Code/Audio/Private/Engine.ixx#L37)) is a custom miniaudio node: stereo in,
downmixed to mono, `iplBinauralEffectApply`, binaural stereo out. Steam Audio processes **fixed
`IplFrameSize` blocks (256 frames, ~5.3 ms at 48 kHz)** while the node graph asks for arbitrary frame
counts, so input and output are staged through one-block FIFOs inside the node.

**The node's FIFOs outlive a sound.** A source keeps its node when it gets another buffer, and the FIFOs then still
hold the last few milliseconds of the sound before. `setBuffer` raises `BinauralNode::flush`; the audio thread drops
both FIFOs at its next block, so a new sound never starts with the end of the old one.

**Every volume change GLIDES over 20 ms** (`engineConfig.defaultVolumeSmoothTimeInPCMFrames`). miniaudio's default is
0: a new volume is one step in the signal, and a step is a click - a source whose gain is set every frame (a fade,
a follow) then clicks at the frame rate, and the mix stutters. Two consequences:

* **The FIRST volume of a new sound is immediate, every later one glides.** `setBuffer` makes the new sound and sets
  its volume from what the source holds, so **set every value (gain, pitch, position, relative) BEFORE `setBuffer`**.
  A gain set after it makes the sound start at the source's last gain and glide to its own: a loud start on a quiet
  sound. `playOneShot` and `GameSound` follow this order.
* A gain that is set again every frame never arrives exactly; it is always within one glide of the value.

The direction and blend are `oc::atomic<float>`s written by `updateSource` on the game thread and
read by the audio thread. **Steam Audio does the HRTF direct path only** — no occlusion and no reflections; the
reverb is the engine's own:

## The shared reverb and the distance filter

**ONE reverb for every spatial source** (`FdnReverb`, [Reverb.ixx](../../../Code/Audio/Private/Reverb.ixx)), tweaks
"Audio/Reverb" (Level, Time, Tone, Size). One reverb for all, not one baked into each sound, is what puts sounds in
the same place: a far sound is mostly its reverb, a near one mostly itself.

* **The reverb** is a feedback delay network: four allpasses smear the input, then eight delay lines of unlike
  lengths, each low-passed (the highs die first), all mixed into each other through a Hadamard matrix and fed back.
  The echoes double on every trip, so after about 60 ms there is a tail and no single echo. (Not parallel combs:
  those ring at their own pitches.) Plain data, no allocation: the audio thread runs it; `configure` (the game
  thread, from `AudioSystem::update` when a tweak changed) only writes numbers.
* **The bus** has no node inputs. Each `BinauralNode`, per HRTF block, adds its dry mono block x `send` into
  `SystemState::sendRing` at its own `sendWrite`; the reverb node (`ReverbNode`: no input bus, a stereo output on the
  endpoint) reads from `sendRead`, clears what it read, runs the reverb, plays it. A send lands `SendLead` (2048
  frames, 43 ms: also the pre-delay) ahead of the read, so the order of the nodes inside one device callback does not
  matter. A node is put back at the lead only when it is far behind (it was silent) or far ahead - never per block:
  a node emits its 256-frame blocks in a rhythm that is not the device's, and a re-sync per block would leave gaps.
  All of it is the audio thread's (one thread), so the ring has no lock.
* **Per source** (`AudioSource`, mono sources only): `setReverbSend(send)` - 0 by default, so a source that does not
  ask for the room stays dry - and `setLowpass(hz)`, a one-pole low pass on the dry block before the HRTF and the
  send (the air takes a far sound's highs; 20000 = open).

## Handles

`AudioBuffer` ([Buffer.ixx:7](../../../Code/Audio/Private/Buffer.ixx#L7)) and `AudioSource`
([Source.ixx:10](../../../Code/Audio/Private/Source.ixx#L10)) are move-only RAII, like `PhysicsMesh` / `PhysicsBody`.

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

**The GAME's sounds do not use `AudioComponent` or the one-shot pool.** `GameSound` (Code/Game) owns its own
`AudioSource` voices and buffers (`createBuffer` from synthesized PCM, or `loadSound`), with the engine's distance
falloff off (`setAttenuation(1, max, 0)`: the distance is in the gain) — see
[`Docs/Context/Game/Sound.md`](../Game/Sound.md).

**DSL surface** — `self.audio.trigger(string alias)` and `self.audio.stop(string alias)`, registered
in `registerScriptDslBindings` ([ScriptContext.cpp:336](../../../Code/Entity/Private/ScriptContext.cpp#L336)).
They lower to the `ctx->audioTrigger` / `ctx->audioStop` ABI thunks; `audioTrigger` takes an
`overrideMask` (bit 0 position, bit 1 volume, bit 2 pitch) that the DSL binding passes as 0. Only the
legacy NodeEditor's "Trigger Audio" node (`UI/Private/NodeEditor`) passes a real mask.
