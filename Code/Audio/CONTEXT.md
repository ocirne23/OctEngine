# Audio

> Library documentation for `Code/Audio`.
> Read [`.claude/CLAUDE.md`](../../.claude/CLAUDE.md) first — rules, building, style, dependency direction.

## Backends

* **miniaudio** — device, mixing, decoding (`MINIAUDIO_IMPLEMENTATION` lives in Audio.cpp).
* **Steam Audio** — HRTF binaural, static `phonon(d).lib`.

Both are wrapped in `module Audio`. Neither API leaks: consumers see opaque handles only.

## `Globals::audio`

* `initialize()` opens the default device (48 kHz stereo) plus the Steam Audio context and HRTF.
* `update(camera[, listenerVelocity])` runs once per frame on the game thread. It recomputes HRTF
  direction, distance attenuation and doppler; the audio thread reads atomics.
* Master volume sits under the Audio/System tweaks.

## Signal path

* **Mono** sounds route `ma_sound` → a per-source `BinauralNode` (256-frame `iplBinauralEffectApply`
  blocks behind FIFOs) → endpoint.
* **Stereo**, or any source with `setRelative(true)`, plays unspatialized.
* Only the HRTF direct path is used so far.

## Buffers and sources

`AudioBuffer` / `AudioSource` are RAII handles.

* `loadSound(path)` — WAV, FLAC, MP3.
* `createBuffer(format, pcm, rate)` — procedural sounds.
* A buffer must outlive every source that plays it.
* `playOneShot(buffer, pos, gain, pitch)` / `playOneShot2D` use a 32-source pool that steals the
  oldest source.

## Gameplay integration

Lives in Entity, not here:

* `AudioComponent`.
* The "Trigger Audio" node — a dynamic exec pin per alias, special-cased in NodeDef.ixx and Scene.cpp.
  Connected Position/Volume/Pitch pins override through `ctx->entityTriggerAudio`'s `overrideMask`.
* The "Stop Audio" node.
