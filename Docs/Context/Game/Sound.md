# Sound

> Part of the Game documentation ([`Docs/Context/Game/CONTEXT.md`](CONTEXT.md)) — read that first.
> The audio engine (buffers, sources, HRTF) is [`Docs/Context/Audio/CONTEXT.md`](../Audio/CONTEXT.md).

`Game:Sound` — `GameSound` (`Sound.ixx` / `Sound.cpp`), a member of `GameMatch` (`m_sound`), plus the synthesizer
(`SoundSynth.cpp`). Tweaks: "Game/Sound" (`Globals::settings.game.sound`).

## The rule: a cue is posted, never played

**No Game code starts a sound.** It posts a CUE; `GameSound::update` — once per windowed frame, at the end of
`GameMatch::updateWindowed` — decides what is heard.

| Source | How |
|---|---|
| Game code, main thread | `GameSound::post(sound, pos, gain, size, delaySec)` / `post2D(sound, gain)`: a static queue (at most 2048 entries; `discardPosted` in the `GameMatch` constructor) |
| The unit and projectile components, any thread | `GameUnitComponent::reportSound` — a bounded queue, see Sound cues in [`Docs/Context/Entity/Components.md`](../Entity/Components.md) |

The cue sites:

| Sound (`EGameSound`) | Site |
|---|---|
| `MeleeHit`, `TurretFire` | `NpcSystem::addBeam` (every role: a client adds the server's beams there) |
| `UnitDeath` | `GameUnitComponent::tickHurtLight` / `kill` |
| `UnitReveal` | `GameUnitComponent::setSolid`, with the reveal smoke (the authority only): a shadow that the light or a field makes solid sizzles |
| `ShotFired`, `LobFired`, `ShotImpact`, `SplashImpact` | `GameProjectileComponent::spawn` / `destroy` |
| `StructurePlaced` | `spawnStructure`, a blueprint only (a load or a join replay spawns built structures: silent) |
| `StructureBuilt` | `investMaterials` (the authority), `mirrorPlace`'s built re-send (a client) |
| `StructureCollapse` | `setRubble(true)`, `removeStructureBookkeeping` (quiet for a cable or a blueprint) |
| `Thunder` | `tickLightning`, delayed by the distance (capped at 6 s) |
| `Alarm` | `GameSound::updateAlarm`: the own team's built structures lost health between two samples (0.5 s) with none removed; 10 s between two alarms |
| `PlayerHurt` | `GameSound::update`: the local player's health fell |
| `BirdSong`, `CricketBout`, `OwlCall`, `WindGust`, `WaterLap` | `GameSound::updateAmbience` / `updateWater` (The ambience is positional, below) |

## The limits

The game can have 30 000 units that attack and die at the same time. In order:

1. **The component queue is bounded** (48 positions per kind between two drains; the rest is counted).
2. **The hearing range:** a world cue farther than "Hearing range (m)" from the EAR is dropped. The ear is the
   player's body (the camera when it is detached); the CAMERA gives the direction. The distance is in the voice's
   GAIN (`attenuation`: inverse from "Full volume distance (m)", faded out over the last 40 % of the range); the
   audio engine's own falloff is off for every game voice.
3. **One start per sound per frame:** the loudest cue of each sound. Then per sound (`c_soundDefs` in `Sound.cpp`):
   `minInterval` between two starts, and `maxVoices` at a time. A sound that has its voices DROPS the new cue: it
   never cuts one of its own playing voices off (tried: a louder cue took the quietest voice of the sound - in a
   battle that cut sounds off many times a second, and every cut is a click).
4. **The voice pool:** all one-shots share "Max one-shot voices" (24). With all busy, a cue 1.25 x louder than the
   quietest voice takes it; else the cue is dropped. A voice is free when its buffer's time is over (no engine query).
5. **Variety:** up to 6 VARIANTS per sound (`c_gameSoundVariants`; a start never takes one of the last two). The
   unit's SIZE (body radius over 0.5 m) makes a big body lower and louder. A variant is NOT the same sound at another
   pitch: see The sounds. On top of that, the random feel below.

## The random feel

Two columns of `c_soundDefs`, `pitchJitter` and `gainJitter`, per sound:

* **A one-shot:** every start is shifted in pitch and in volume by up to that share. The pitch shift is a RATIO (as far
  down as up) from the sum of two rolls, so most starts are near the sound's own pitch and a few are far from it.
  Large for the creatures and the impacts (each throat and each blow is its own), small for the light tower (a
  machine), **0 for the alarm, and no pitch shift for "built": a signal is the same every time.**
* **A loop:** its pitch and volume DRIFT by up to that share for as long as it plays (`GameSound::drift`: three sines
  whose rates share no period, so the drift never repeats in step with the loop). The wind's gusts are then longer
  than its 10 s loop. A building loop drifts by itself, seeded by its structure id: one fire flares while the one
  next to it dies down. Small for the hum (a steady field), large for the fire and the wind.

## Bursts: the cases that are closed

Every path that can start a sound has a bound. The ones that are not in the list above:

| Case | What would happen | The bound |
|---|---|---|
| ONE sound posts thousands of cues in a frame (the melee hits of a horde, through `addBeam`) | it fills the post queue (2048), and every sound posted after it in that frame is lost: a "built", a thunder, a bird's notes | `pushPosted`: a sound keeps 32 posts between two updates; the rest is only counted, and weighs in the battle bed (as the component cues over their bound do) |
| No update for a while (the lobby or the main menu is up, the sounds are being synthesized, a hitch) | the cues of the whole gap play as one late burst, and their counts - read as one frame's - throw the battle bed to its full level | a gap of more than 0.5 s between two updates: `discardPosted` and the battle bed's sum to 0 |
| A match starts; F10 loads a save | a collapse per removed structure, a "placed" per blueprint | `discardPosted` in the `GameMatch` constructor and after the load (it drops the components' cues too). The shots a load removes are destroyed by a job after it: at most one impact sound |
| MANY sounds at once, each at a fair level | the sum goes over full scale at the output and DISTORTS: many quiet sounds become one loud noise | THE LOUDNESS LIMIT: `updateLoudnessLimit` sums everything that plays (as power: the sounds are not in step) and holds it under "Loudness limit" (1.2) with one factor on every game voice (`m_duck`): down at once, up slowly. "Game/Sound/Stats" shows the sum before the limit |
| The audio thread's cost | each spatial voice is an HRTF convolution | at most "Max one-shot voices" (24, up to 48) + 5 beds + "Max building loops" (6, up to 12) + 2 water voices, whatever the game does |

## The beds: what the limits drop is not lost

| Bed | Level |
|---|---|
| **Battle** (`LoopBattle`, 2D) | Every combat cue in range adds its `crowdWeight` x attenuation. A component kind over its bound scales its cues by count / 48, so the positions that came in stand for all. The rate (per second, sampled every 0.25 s) over "Cues/s for a full battle bed" is the level: up fast, down slowly. |
| **Horde** (`LoopHorde`, at the centroid) | Units of another team within "Horde radius (m)" of the ear, weighted by nearness, over "Units for a full horde bed". A SLICED walk of `World::rootEntities()` (1/16 of the list a frame, at least 512), main thread. |
| **Wind / Day / Night** | The far BACKGROUND only (flat stereo): wind always; the day loop (leaves, far birds) and the night loop (a far cricket field) cross-fade with `daylight()` in the sandbox world only. What one can point at is the ambience below. |

A battle of thousands is: two loops, plus at most 24 one-shots of the nearest events.

### The battle focus: the bed and the one-shots must not say the same thing twice

A dense bed PLUS dense one-shots is noise. So while a battle is on (`battleBusy()` = the battle bed's level x "Battle
focus", 0..1), the COMBAT one-shots (the Effects group) step back:

| At full busy | |
|---|---|
| Time between two starts of a sound | 4 x its `minInterval` |
| Voices of one sound | half its `maxVoices` |
| All combat voices together | 40 % of the pool |
| Which cue may sound | only a NEAR one (loudness over 0.3 x the sound's gain: within about 3 x "Full volume distance"); a far one is in the bed |

and the two beds make room for each other and for the one-shots:

* **The horde bed steps back as the battle bed comes up** (x `1 - 0.7 x` the battle level): the horde bed is the horde
  before the fight, the battle bed is the crowd in it. Two crowd loops at full level are one noise.
* **The battle loop is the fight FURTHER AWAY**: one dull, quiet texture (A CROWD IS A TEXTURE, under The sounds).
  The near, bright events are the one-shots. A bed as bright and dense as they are covers them.
* **The beds' level curve is nearly straight** (the level to the power 0.8). A square root made both beds loud from
  the first few cues or units on: a bed that is always near its full level is a drone, not a crowd that grows.

"Battle focus" 0 turns the stepping back off (every combat cue may sound); 2 makes it start at half the level.

## The ambience is positional (the sandbox world)

`GameSound::updateAmbience` / `updateWater`. Every ambient sound in front of the background is a MONO sound at a place
in the world, through the same cue limits as everything else (so in a battle the louder combat cues take their voices).

**PERCHES** (`m_perches`, 18): a place around the ear with one animal that sings from there again and again.

| Kind | Count | Where (from the ear, over the ground) | When | Sound |
|---|---|---|---|---|
| Bird | 8 | 12 - 65 m, 4 - 14 m up | by day (fades in over daylight 0.25 - 0.7), every 5 - 20 s | `singBird`: a run of `BirdSong` NOTES, see below |
| Cricket | 7 | 6 - 40 m, on the ground | by night, a bout every 2.5 - 6 s; **silent while the ear is within 5 m** | `CricketBout` (4 crickets: pitch, tempo, pulses) |
| Owl | 1 | 40 - 80 m, 6 - 12 m up | full night, every 12 - 35 s | `OwlCall` |
| Gust | 2 | 15 - 45 m, 2 - 12 m up | always, every 3 - 10 s | `WindGust` |

* **NO BIRD SONG IS A RECORDING** (`GameSound::singBird`). The six `BirdSong` buffers are single NOTES (a flat
  whistle, a rising chirp, a falling chirp, a tick, a long soft whistle, a rough triple pulse). A song is a run of
  note cues made at run time, each with its own delay, pitch and level (`postVariant(sound, pos, gain, variant, pitch,
  delaySec)`). A bird has a VOICE (`Perch::pitch`, 0.7 - 1.35) and a STYLE (`variant % 6`: stepped whistles, a warble,
  a trill, steps down, one chirp again and again, a low chatter), but every number is rolled again for each song: how
  many notes, every gap, every interval. One hears "that kind of bird", never "that sound again". **70 % of what a
  bird says is a plain CALL** (one to three of its usual chirps); the full song is the rare one. Tried and dropped: a
  whole song per buffer (six songs, each heard again within a minute) and songs in the day loop (the same song at the
  same second of every round). A note's timing is on the frame (a cue starts in `update`), so the fastest run - the
  trill - has a gap of 50 ms or more.
* **A perch keeps its place and its song** (`postVariant`: the cue names its variant, `startVoice` takes no random one)
  while the ear is in reach: walk past a bird and it stays in its tree, behind you. A perch farther than its leave
  distance (85 / 55 / 100 m) gets a new place near the ear and another animal.
* **A gust takes a new place every time**: the wind comes from somewhere else each time.
* The height is over `StructureSystem::groundHeightAt(..., coarse)`. No tree is looked up: a bird is in the air where
  a tree could be.

**THE FEAR** (`m_fear`, 0..1, in `updateBeds`): the animals are scared of the demons, and their silence is the
warning. The scare is the larger of: the enemy units in the horde radius (the horde walk's weighted count, also a
shadow that is not solid yet) over "Enemies that silence the animals" (6), and 3 x the battle bed's level. The fear
goes UP in about 1.5 s and DOWN over "Animals return (s)" (30). A bird's, a cricket's and the owl's chance to make
its sound is x `1 - fear / 0.7` (none from a fear of 0.7), rolled per perch - so they do not stop or come back at
one moment - and the night loop (the far cricket field) goes down to 15 %. **The wind and the gusts do not care**:
what is left is the wind alone.

**WATER** (`m_water`, 2 loop voices): 48 points around the ear (16 directions at 6, 18 and 40 m), 4 tested a frame. A
point is water where `PhysicsWorld::sampleWaterHeight` is over the ground. At the end of a round the nearest wet
point, and the nearest wet point on the other side of the ear, each get a `LoopWater` voice. A voice GLIDES to a new
place (no jump), the two have different pitches (never the same water twice), and single `WaterLap`s sound near the
first every 1.5 - 5 s. No water in 40 m: both fade out.

## Building loops

"Max building loops" (6) voices. Every 0.25 s the nearest built, powered, non-rubble structures with a loop within
60 m get them (`loopSoundOf`): hum (the buildings that make a field: the Emitter, Bastion, Lance, Base; within 30 m),
fire (the Generator), works (the Lumberyard, Quarry, Fabricator). A voice keeps its structure by id; one that lost it
fades out before it takes another. Each building's loop has its own pitch (off the id), so two of one type are never
in phase.

**AT MOST ONE HUM, two fires and two workshops play** (the nearest of each kind). A loop is a steady sound: several
of one kind do not read as several buildings, they add up to one louder, rougher drone. Tried and dropped: a hum on
every field building, store and lens - six hums, each at its own pitch, were a wall of noise over a base. The hum is
also the quietest loop (gain 0.22): it says "a field is here" and no more.

## The sounds

All 28 sounds are SYNTHESIZED (`synthGameSound` in `SoundSynth.cpp`, 48 kHz - the engine's rate) and kept in a
file, so a run synthesizes only what the file does not hold.

### The sound cache

`GameSound::load` (the first windowed frame of a match, main thread) reads `Assets/Local/Sounds/game.snd`: a header
(magic, STAMP, entry count), then per sound and variant a 12 byte record (sound, variant, stereo, rate, sample count)
and the samples as 16 bit.

* **The stamp makes the file invalid by itself**, with no number to bump by hand: it is `synthGameSoundStamp()` - a
  hash of `__TIMESTAMP__` in `SoundSynth.cpp`, the time that source file was last changed - mixed with every sound's
  name and variant count in `c_soundDefs`' order. So an edit of the synthesis, a new or moved sound and another
  variant count each give another stamp, and a file with another stamp is not read at all.
  **A change of the synthesis that is NOT in `SoundSynth.cpp` does not change the stamp** - keep all of it there.
* A sound the file does not hold (the first run, a cut file, a `Sounds/Game/*.wav` that was removed) is synthesized,
  and the whole file is written again with it. The log line "GameSound: synthesized N sounds" says when that happened.
* A sound that comes from a `.wav` is not in the file.
* The buffers are made from the 16 bit samples on EVERY run, also the one that synthesizes: a run with the cache
  sounds the same as the run that made it.
* To force a new synthesis: delete the file.

Without the file the synthesis is a one-time cost of seconds at the first windowed frame of a match. Each sound is a few LAYERS mixed on a
canvas, then a room (`Canvas::reverb`) and `Canvas::finish` (no offset, soft edges, peak 0.9; a sound with a value
that is not finite is made silent and logged).

> **THE RULE: NOISE IS THE SOURCE, A FILTER GIVES IT ITS PITCH.** A pure sine, a sine whose pitch slides, a regular
> tremolo, a saw wave and an FM pair are what makes a synthesized sound read as an old game console. No layer is one.
> Tried and dropped for that reason: the pitch-drop sine thump, sums of decaying sines for struck bodies and bells, a
> saw-wave voice, an FM strike for the light tower, a chord of sines for the field hum.

| Building block | What it is | Used for |
|---|---|---|
| `thump` | a short burst of low noise into a damped low resonance (it rings a cycle or two: a thud, no note, no pitch slide) | the weight of a hit, a shot, an explosion |
| `noise` | white / pink / brown noise through a filter that glides (`Svf`: stable while its frequency moves) | slaps, air, rumble, crackle, the saw, the light tower's bolt |
| `modal` | a STRUCK BODY: noise that dies away, through one resonance per mode (`c_wood`, `c_metal`, `c_crystal`, `c_tube`, `c_drum`, `c_glow`, `rockClack`). The noise drives the resonances while they ring, so a mode is a narrow band of noise, never a sine; `maxQ` bounds how near a tone it gets | the mallet, the chisel, stones, the drum, the light tower's crystal |
| `vocal` | a THROAT: soft pulses whose length and strength change from one to the next, plus breath that gets louder with every pulse, through three wide formants; `VocalKey`s move the pitch, the mouth, the breath and the roughness | the single creature sounds: `makeDeathCry` (the death grunt), the player's grunt (a crowd is not made of these: see A CROWD IS A TEXTURE) |
| `whistle` | a tone with an unsteady pitch and a band of breath at the same pitch - the only tones in the game | birds, crickets, the owl |
| `flutter` | an uneven level (random values with straight lines between), where a regular tremolo would sound like a machine | the saw's teeth, a gurgle |

The field hum is noise through the resonances of
a low chord (air that sings); the alarm is a war drum; "built" is two mallet blows and a soft glow of air.

**The death sound is a GRUNT, not a scream:** 0.2 - 0.4 s, low (about 80 - 130 Hz), more breath than voice, and
quieter than the hits (`c_soundDefs` gain 0.40). Half of it is the body that goes down. **A unit has NO hurt sound**
(tried, dropped: with thousands of units it was a constant chorus that said nothing the hit sound did not).

**The reveal SIZZLE** (`UnitReveal`) goes with the reveal smoke: a thin high hiss that spits unevenly and is gone in
half a second, a breath of steam, a few tiny spits. SUBTLE on purpose: gain 0.16, two voices at most, 0.12 s between
two starts, and no `crowdWeight` (a crowd that walks into a field does not raise the battle bed).

**A LOOP has no seam:** its events mix with a wrap, and every filter, noise bed and the reverb runs twice around with
the same noise, keeping the second round (the state at the loop's start is then the state at its end). A slow curve
in a loop is a `LoopLfo` (sines with whole cycles in the loop).

**The variants differ in CHARACTER** (the `kind` argument = the variant index, plus the variant's own random stream
for every layer's pitch, time and level): a melee hit is blunt, a claw slash, wet, or a crunch; a death grunt falls, is short, is a long breath out, or gurgles;
the light tower's bolt has its own tear and sometimes a second bolt; thunder is near (cracks first) or far.

**A CROWD IS A TEXTURE, NOT A SUM OF EVENTS** (`LoopBattle`, `LoopHorde`). Many voices at once are heard as ONE sound
with a colour and a grain, so the two crowd loops are made as that one sound, from streams that never stop:

| Piece | What it is |
|---|---|
| `Murmur` | one stream of a crowd's voice: breath that pulses at a low, unsteady rate (rough, no tone), through two wide formants that wander (the mouths), let through in bursts (the syllables) |
| `dust` | sparse random impulses, most of them small; a low filter makes them a patter of feet, a few wandering resonances make them a clatter of blows - with no single event to pick out |
| `Wander` | a value that walks between random targets: every level, mouth and resonance above moves by it, so nothing in the texture is steady |
| `closeLoop` | a texture has no whole cycles to close on: it is made longer than the loop and its tail is faded into its head |

The horde (mono, 14 s): the patter, and three murmur streams with their own pitch, mouth and tempo that come forward
and step back by themselves. The battle (stereo, 12 s): per side the clatter, its low weight, and one fast, wide
murmur stream, all in surges; dull and not loud (the fight further away - the near events are the one-shots).

Tried and dropped: recorded-style events mixed into a loop (60 growls; then 130 hits, cries and growls; then steps
with single growls). Each event keeps its own start and shape, so the sum is a clutter of starts or - dense enough
to hide them - a steady drone. A continuous low noise ("roar") under the battle loop was a drone by itself.

**To hear a sound:** "Game/Sound" → "Play a sound" plays the pick once, flat; every pick is another variant.

**A file `Assets/Sounds/Game/<name>.wav`
(`<name>2.wav`, `<name>3.wav` for the variants) replaces a synthesized sound** — the names are the first column of
`c_soundDefs`: `melee_hit`, `unit_reveal`, `unit_death`, `shot_fired`, `lob_fired`, `shot_impact`, `splash_impact`,
`turret_fire`, `structure_placed`, `structure_built`, `structure_collapse`, `thunder`, `alarm`,
`player_hurt`, `bird_song`, `cricket_bout`, `owl_call`, `wind_gust`, `water_lap`, `loop_wind`, `loop_day`,
`loop_night`, `loop_battle`, `loop_horde`, `loop_hum`, `loop_fire`, `loop_works`, `loop_water`. A MONO file is spatialized (HRTF); a stereo file plays flat, so a world sound must be mono.

## Not there

* A client hears no unit death when the server removes the unit before a health of 0 arrives (deaths come from the
  replicated health, not from the despawn).
* No footsteps, no interface clicks, no music.

## Stats

"Game/Sound/Stats": voices playing, cues in per second (before any limit), one-shots started per second.
