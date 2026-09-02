# Audio
Library documentation for `Code/Audio`. Read `.claude/CLAUDE.md` first (rules, building, style, dependency direction).

* miniaudio (device, mixing, decoding; `MINIAUDIO_IMPLEMENTATION` in Audio.cpp) + Steam Audio (HRTF binaural, static phonon(d).lib), wrapped in `module Audio`; neither API leaks (opaque handles)
* `Globals::audio`: `initialize()` opens the default device (48kHz stereo) + Steam Audio context/HRTF; `update(camera[, listenerVelocity])` once per frame — recomputes HRTF direction, distance attenuation, doppler on the game thread (audio thread reads atomics). Master volume under Audio/System
* Mono sounds route ma_sound → per-source `BinauralNode` (256-frame `iplBinauralEffectApply` blocks behind FIFOs) → endpoint; stereo or `setRelative(true)` plays unspatialized. Only the HRTF direct path is used so far
* `AudioBuffer`/`AudioSource`: RAII handles. `loadSound(path)` (WAV/FLAC/MP3) or `createBuffer(format, pcm, rate)` for procedural sounds; buffers must outlive sources playing them. `playOneShot(buffer, pos, gain, pitch)` / `playOneShot2D` — 32-source pool, steals oldest
* Gameplay integration lives in Entity: `AudioComponent` + the "Trigger Audio" node (dynamic exec pin per alias, special-cased in NodeDef.ixx/Scene.cpp; connected Position/Volume/Pitch pins override via `ctx->entityTriggerAudio`'s overrideMask) and "Stop Audio"
