module Game;

import Core;
import Core.glm;
import Core.Camera;
import Core.Time;
import Core.Log;
import Audio;
import Entity;
import Physics; // the water surface
import File;
import Settings;
import :Sound;
import :Structures;

// The limits and the beds of the game's sound: see the header comment in Sound.ixx.

namespace
{
    enum EGroup : uint8 { GroupEffects, GroupStructures, GroupAmbience, GroupCrowd, GroupInterface };
    // World = attenuated by the distance to the ear; Far = a direction only (the caller's gain holds the distance);
    // Flat = no place.
    enum EPlace : uint8 { PlaceWorld, PlaceFar, PlaceFlat };

    struct SoundDef
    {
        const char* name;   // Sounds/Game/<name>.wav replaces the synthesized sound
        EGroup group;
        EPlace place;
        uint8 variants;     // <= c_gameSoundVariants
        uint8 maxVoices;    // of this sound at a time
        float gain;
        float minInterval;  // s between two starts
        // THE RANDOM FEEL. A one-shot: every start is shifted by up to this share in pitch (as a ratio, most starts near the
        // middle) and in volume. A loop: its pitch and volume DRIFT by up to this share, slowly, for as long as it plays.
        // 0 where a shift would be wrong: a sound with a note of its own.
        float pitchJitter;
        float gainJitter;
        float crowdWeight;  // what one cue at the ear adds to the battle bed
    };
    constexpr SoundDef c_soundDefs[] =
    {
        //                                                    var  voices gain  interval pitch  volume crowd
        { "melee_hit",          GroupEffects,    PlaceWorld, 6, 4, 0.55f, 0.07f, 0.14f, 0.30f, 1.0f },
        { "unit_reveal",        GroupEffects,    PlaceWorld, 5, 2, 0.16f, 0.12f,  0.12f, 0.30f, 0.0f }, // the sizzle with the reveal smoke: SUBTLE, and no part of the battle bed
        { "unit_death",         GroupEffects,    PlaceWorld, 6, 3, 0.40f, 0.14f, 0.16f, 0.25f, 2.0f },
        { "shot_fired",         GroupEffects,    PlaceWorld, 4, 3, 0.50f, 0.06f,  0.12f, 0.25f, 0.5f },
        { "lob_fired",          GroupEffects,    PlaceWorld, 3, 2, 0.70f, 0.10f,  0.08f, 0.20f, 0.5f },
        { "shot_impact",        GroupEffects,    PlaceWorld, 4, 3, 0.50f, 0.06f,  0.14f, 0.30f, 0.5f },
        { "splash_impact",      GroupEffects,    PlaceWorld, 3, 3, 1.00f, 0.12f,  0.10f, 0.20f, 2.0f },
        { "turret_fire",        GroupEffects,    PlaceWorld, 5, 4, 0.35f, 0.06f,0.07f, 0.15f, 1.0f }, // a machine: nearly the same every time
        { "structure_placed",   GroupStructures, PlaceWorld, 4, 2, 0.60f, 0.08f,  0.09f, 0.20f, 0.0f },
        { "structure_built",    GroupStructures, PlaceWorld, 2, 2, 0.60f, 0.25f,  0.00f, 0.10f, 0.0f }, // bells: the note stays
        { "structure_collapse", GroupStructures, PlaceWorld, 3, 2, 1.00f, 0.30f,  0.10f, 0.15f, 0.0f },
        { "bird_song",          GroupAmbience,   PlaceWorld, 6, 8, 0.50f, 0.02f,  0.02f, 0.20f, 0.0f }, // single NOTES: singBird makes the song
        { "cricket_bout",       GroupAmbience,   PlaceWorld, 4, 4, 0.35f, 0.10f,  0.02f, 0.20f, 0.0f },
        { "owl_call",           GroupAmbience,   PlaceWorld, 2, 1, 0.50f, 1.00f,  0.03f, 0.15f, 0.0f },
        { "wind_gust",          GroupAmbience,   PlaceWorld, 4, 3, 0.60f, 0.50f,  0.10f, 0.30f, 0.0f },
        { "water_lap",          GroupAmbience,   PlaceWorld, 4, 2, 0.50f, 0.30f,  0.10f, 0.30f, 0.0f },
        { "thunder",            GroupAmbience,   PlaceFar,   6, 2, 1.00f, 0.50f,  0.10f, 0.15f, 0.0f }, // the variant is the distance (tickLightning)
        { "thunder_crack",      GroupAmbience,   PlaceFar,   4, 3, 0.80f, 0.02f,  0.10f, 0.15f, 0.0f }, // one per later stroke of a flash; variants 0, 1 near, 2, 3 middle
        { "alarm",              GroupInterface,  PlaceFlat,  1, 1, 0.60f, 1.00f,  0.00f, 0.00f, 0.0f },
        { "player_hurt",        GroupInterface,  PlaceFlat,  3, 2, 0.80f, 0.30f,  0.07f, 0.15f, 0.0f },
        { "loop_wind",          GroupAmbience,   PlaceFlat,  1, 1, 0.50f, 0.0f,   0.07f, 0.30f, 0.0f }, // gusts longer than the loop
        { "loop_day",           GroupAmbience,   PlaceFlat,  1, 1, 0.35f, 0.0f,   0.02f, 0.15f, 0.0f }, // the far background only:
        { "loop_night",         GroupAmbience,   PlaceFlat,  1, 1, 0.30f, 0.0f,   0.015f, 0.15f, 0.0f }, // the perches are in front
        { "loop_battle",        GroupCrowd,      PlaceFlat,  1, 1, 0.55f, 0.0f,   0.04f, 0.15f, 0.0f },
        { "loop_horde",         GroupCrowd,      PlaceFar,   1, 1, 0.45f, 0.0f,   0.05f, 0.20f, 0.0f },
        { "loop_hum",           GroupStructures, PlaceWorld, 1, 1, 0.22f, 0.0f,   0.008f, 0.10f, 0.0f }, // a steady field: a little
        { "loop_fire",          GroupStructures, PlaceWorld, 1, 1, 0.70f, 0.0f,   0.04f, 0.25f, 0.0f }, // a fire is never steady
        { "loop_works",         GroupStructures, PlaceWorld, 1, 1, 0.60f, 0.0f,   0.03f, 0.15f, 0.0f }, // the worker's tempo
        { "loop_water",         GroupAmbience,   PlaceWorld, 1, 1, 0.70f, 0.0f,   0.03f, 0.20f, 0.0f },
    };
    static_assert(oc::size(c_soundDefs) == (size_t)EGameSound::Count);

    struct Posted
    {
        glm::vec3 pos{ 0.0f };
        float gain = 1.0f;
        float size = 1.0f;
        double due = 0.0;
        EGameSound sound = EGameSound::Count;
        bool placed = true;
        int8 variant = -1; // >= 0: this variant, not a random one
        float pitch = 1.0f;
    };
    constexpr size_t c_maxPosted = 2048; // a match with no windowed update (a lobby client) never drains: bounded
    oc::vector<Posted> g_posted;         // main thread only

    // The loudest due cue of each sound this frame.
    struct Best
    {
        glm::vec3 pos{ 0.0f };
        float loudness = 0.0f;
        float size = 1.0f;
        bool placed = true;
        int variant = -1;
        float pitch = 1.0f;
    };

    EGameSound loopSoundOf(EStructureType type)
    {
        switch (type)
        {
        case EStructureType::Emitter:
        case EStructureType::Bastion:
        case EStructureType::Lance:
        case EStructureType::Base:
            return EGameSound::LoopHum; // the buildings that make a FIELD: the hum is the field's (a store and a lens are silent)
        case EStructureType::Generator:
            return EGameSound::LoopFire;
        case EStructureType::Lumberyard:
        case EStructureType::Quarry:
        case EStructureType::Fabricator:
            return EGameSound::LoopWorks;
        default:
            return EGameSound::Count;
        }
    }

    constexpr float c_noFalloff = 3.0e38f;
}

// THE POST QUEUE IS BOUNDED PER SOUND: between two updates a sound keeps c_maxPostedOfSound cues; one over that is only
// counted (it still weighs in the battle bed, like the components' cues over their bound). Without it ONE sound could
// fill the whole queue - thousands of melee hits in a frame - and every other sound posted after it in that frame was
// lost: a "built", a thunder, the notes of a bird.
namespace
{
    constexpr uint32 c_maxPostedOfSound = 32;
    uint32 g_postedOfSound[(int)EGameSound::Count]; // since the last update, the ones over the bound included

    void pushPosted(const Posted& posted)
    {
        if (++g_postedOfSound[(int)posted.sound] <= c_maxPostedOfSound && g_posted.size() < c_maxPosted)
            g_posted.push_back(posted);
    }
}

void GameSound::post(EGameSound sound, const glm::vec3& pos, float gain, float size, float delaySec)
{
    pushPosted(Posted{ pos, gain, size, Globals::time.getElapsedSec() + (double)delaySec, sound, true });
}

void GameSound::post2D(EGameSound sound, float gain)
{
    pushPosted(Posted{ glm::vec3(0.0f), gain, 1.0f, Globals::time.getElapsedSec(), sound, false });
}

void GameSound::postVariant(EGameSound sound, const glm::vec3& pos, float gain, int variant, float pitch, float delaySec)
{
    pushPosted(Posted{ pos, gain, 1.0f, Globals::time.getElapsedSec() + (double)delaySec, sound, true, (int8)variant, pitch });
}

// Everything that waits to be heard, dropped: the posts and the components' cues. For the moments whose cues are no
// events of the game: a match that starts (the last one's teardown posted a collapse per structure), a load (the
// same, and an impact per shot that the load removed), a long gap between two updates.
void GameSound::discardPosted()
{
    g_posted.clear();
    for (uint32& count : g_postedOfSound)
        count = 0;
    oc::vector<GameUnitComponent::SoundCue> cues;
    uint32 counts[(int)GameUnitComponent::ESoundCue::Count];
    GameUnitComponent::takeSoundCues(cues, counts);
}

float GameSound::rand01()
{
    m_rng = m_rng * 1664525u + 1013904223u;
    return float(m_rng >> 8) * (1.0f / 16777216.0f);
}

// Inverse distance from "Full volume distance", faded to nothing over the last 40 % of "Hearing range".
float GameSound::attenuation(float dist) const
{
    const float full = glm::max(m_settings.fullVolumeDistance, 0.5f);
    const float range = glm::max(m_settings.hearingRange, full + 1.0f);
    return full / glm::max(dist, full) * (1.0f - glm::smoothstep(range * 0.6f, range, dist));
}

// THE SOUND CACHE: `Local/Sounds/game.snd`. The synthesis takes seconds and gives the same samples on every run, so
// its output is kept: a header (magic, stamp, entry count), then per sound and variant a record (sound, variant, stereo,
// rate, sample count) and the samples as 16 bit. The STAMP is the synthesizer's (the time SoundSynth.cpp was changed)
// mixed with every sound's name and variant count, in order - so an edit of the synthesis, a new sound or another
// variant count each make a file with another stamp, and a file with another stamp is not read at all.
// A sound the file does not hold (the first run; a `Sounds/Game/*.wav` that was removed) is synthesized, and the file
// is written again with it. A sound that comes from a .wav is not in the file.
namespace
{
    constexpr uint32 c_cacheMagic = 0x444e5347u; // "GSND"
    constexpr char c_cachePath[] = "Local/Sounds/game.snd";
    struct CacheRecord
    {
        uint8 sound, variant, stereo, pad;
        uint32 rate;
        uint32 samples; // int16 values that follow (both channels of a stereo sound)
    };
    static_assert(sizeof(CacheRecord) == 12);

    uint32 cacheStamp()
    {
        uint32 hash = synthGameSoundStamp();
        for (const SoundDef& def : c_soundDefs)
        {
            for (const char* c = def.name; *c; ++c)
                hash = (hash ^ (uint8)*c) * 16777619u;
            hash = (hash ^ def.variants) * 16777619u;
        }
        return hash;
    }
}

void GameSound::load()
{
    m_loaded = true;
    ProfileScope scope("Game sound load", EProfileCategory::Audio);
    m_buffers.resize((size_t)EGameSound::Count * c_gameSoundVariants);

    // What the cache holds: where each sound's record is in the file's bytes.
    const uint32 stamp = cacheStamp();
    oc::vector<uint8> cached;
    oc::vector<uint32> recordAt((size_t)EGameSound::Count * c_gameSoundVariants, 0u); // byte offset of the record; 0 = not there
    if (FileSystem::exists(c_cachePath, /*allowMainThread*/ true) && FileSystem::readFileBytes(c_cachePath, cached, /*allowMainThread*/ true)
        && cached.size() >= 12)
    {
        uint32 header[3];
        memcpy(header, cached.data(), sizeof(header));
        size_t at = sizeof(header);
        if (header[0] == c_cacheMagic && header[1] == stamp)
            for (uint32 i = 0; i < header[2] && at + sizeof(CacheRecord) <= cached.size(); ++i)
            {
                CacheRecord record;
                memcpy(&record, cached.data() + at, sizeof(record));
                const size_t bytes = (size_t)record.samples * sizeof(int16);
                if (at + sizeof(record) + bytes > cached.size())
                    break; // cut short: what is missing is synthesized
                if (record.sound < (uint8)EGameSound::Count && record.variant < c_gameSoundVariants)
                    recordAt[(size_t)record.sound * c_gameSoundVariants + record.variant] = (uint32)at;
                at += sizeof(record) + bytes;
            }
    }

    // The file as it will be written if anything was synthesized: every cached sound copied, every new one added.
    oc::vector<uint8> written(12);
    uint32 writtenCount = 0, synthesized = 0;
    const auto append = [&](const CacheRecord& record, const void* samples)
    {
        const size_t at = written.size(), bytes = (size_t)record.samples * sizeof(int16);
        written.resize(at + sizeof(record) + bytes);
        memcpy(written.data() + at, &record, sizeof(record));
        memcpy(written.data() + at + sizeof(record), samples, bytes);
        ++writtenCount;
    };

    oc::vector<float> samples;
    oc::vector<int16> packed;
    oc::vector<uint8> fileBytes;
    for (int s = 0; s < (int)EGameSound::Count; ++s)
    {
        const SoundDef& def = c_soundDefs[s];
        for (int v = 0; v < (int)def.variants; ++v)
        {
            AudioBuffer& buffer = m_buffers[(size_t)s * c_gameSoundVariants + v];
            const oc::string path = oc::string("Sounds/Game/") + def.name + (v > 0 ? oc::to_string(v + 1) : oc::string()) + ".wav";
            if (FileSystem::exists(path, /*allowMainThread*/ true) && FileSystem::readFileBytes(path, fileBytes, /*allowMainThread*/ true))
                buffer = Globals::audio.loadSound(path, fileBytes);
            if (!buffer.isValid())
            {
                if (const uint32 at = recordAt[(size_t)s * c_gameSoundVariants + v])
                {
                    CacheRecord record;
                    memcpy(&record, cached.data() + at, sizeof(record));
                    const uint8* data = cached.data() + at + sizeof(record); // at an even offset: the header and every record are 12 bytes
                    buffer = Globals::audio.createBuffer(record.stereo ? EAudioFormat::Stereo16 : EAudioFormat::Mono16,
                        oc::span<const std::byte>(reinterpret_cast<const std::byte*>(data), (size_t)record.samples * sizeof(int16)), record.rate);
                    append(record, data);
                }
                else
                {
                    bool stereo = false;
                    uint32 rate = 48000;
                    synthGameSound((EGameSound)s, v, samples, stereo, rate);
                    packed.resize(samples.size());
                    for (size_t i = 0; i < samples.size(); ++i)
                        packed[i] = (int16)(glm::clamp(samples[i], -1.0f, 1.0f) * 32767.0f);
                    // The buffer is made from the 16 bit samples too: this run sounds like every later one.
                    buffer = Globals::audio.createBuffer(stereo ? EAudioFormat::Stereo16 : EAudioFormat::Mono16,
                        oc::as_bytes(oc::span<const int16>(packed)), rate);
                    append(CacheRecord{ (uint8)s, (uint8)v, (uint8)(stereo ? 1 : 0), 0, rate, (uint32)packed.size() }, packed.data());
                    ++synthesized;
                }
            }
            if (buffer.isValid())
                m_variantCount[s] = (uint8)(v + 1);
        }
    }
    if (synthesized > 0)
    {
        const uint32 header[3] = { c_cacheMagic, stamp, writtenCount };
        memcpy(written.data(), header, sizeof(header));
        const bool saved = FileSystem::writeFileBytes(c_cachePath, written, /*allowMainThread*/ true);
        Log::info(oc::format("GameSound: synthesized {} sounds, {} ({:.1f} MB)", synthesized,
            saved ? "saved to Local/Sounds/game.snd" : "could NOT save Local/Sounds/game.snd", (float)written.size() / (1024.0f * 1024.0f)));
    }
}

void GameSound::update(const Frame& frame, const StructureSystem& structures)
{
    ProfileScope scope("Game sound", EProfileCategory::Audio);
    if (!Globals::audio.isInitialized() || !m_settings.enabled || !frame.camera)
    {
        // Off: every voice goes, and what came in is dropped (the cue queues must not fill).
        m_voices.clear();
        m_loops.clear();
        for (Bed& bed : m_beds)
            bed = Bed{};
        for (WaterVoice& water : m_water)
            water = WaterVoice{};
        discardPosted();
        m_settings.voicesPlaying = 0;
        return;
    }
    if (!m_loaded)
        load();
    // A GAP: no update for half a second (the lobby or the main menu was up, the sounds were being synthesized, a long
    // hitch). Everything that came in meanwhile is old: played now it would be one burst of late sounds, and the cue
    // counts of the whole gap, read as one frame's, would throw the battle bed to its full level.
    const double now = Globals::time.getElapsedSec();
    if (now - m_now > 0.5)
    {
        discardPosted();
        m_crowdAccum = 0.0f;
    }
    m_now = now;
    const float dt = glm::clamp((float)Globals::time.getDeltaSec(), 0.0f, 0.1f);

    // The ear's frame (heardAt): the camera's axes, and the same right / forward laid flat on the ground.
    {
        const glm::mat4 camToWorld = glm::inverse(frame.camera->viewMatrix);
        m_camPos = frame.camera->position;
        m_camRight = glm::normalize(glm::vec3(camToWorld[0]));
        m_camUp = glm::normalize(glm::vec3(camToWorld[1]));
        m_camForward = glm::normalize(-glm::vec3(camToWorld[2]));
        m_ear = frame.ear;
        glm::vec3 forward(m_camForward.x, 0.0f, m_camForward.z);
        if (glm::dot(forward, forward) < 0.01f) // the camera looks straight down: the top of the screen is forward
            forward = glm::vec3(m_camUp.x, 0.0f, m_camUp.z);
        glm::vec3 right(m_camRight.x, 0.0f, m_camRight.z);
        if (glm::dot(forward, forward) > 1e-6f && glm::dot(right, right) > 1e-6f)
        {
            m_earForward = glm::normalize(forward);
            m_earRight = glm::normalize(right);
        }
        for (Voice& v : m_voices) // the player turns while a long sound plays: it stays where it is in the world
            if (v.positional && v.end > m_now)
                v.source.setPosition(heardAt(v.pos));
    }

    if (frame.playerHealth >= 0.0f && m_lastPlayerHealth >= 0.0f && frame.playerHealth < m_lastPlayerHealth - 0.5f)
        post2D(EGameSound::PlayerHurt, glm::clamp((m_lastPlayerHealth - frame.playerHealth) * 0.1f, 0.4f, 1.0f));
    m_lastPlayerHealth = frame.playerHealth;
    updateAlarm(frame, structures);
    if (m_settings.audition > 0) // "Play a sound": once, flat
    {
        if (m_settings.audition <= (int)EGameSound::Count)
        {
            m_lastStart[m_settings.audition - 1] = 0.0; // the pick is never held back by the sound's interval
            post2D((EGameSound)(m_settings.audition - 1));
        }
        m_settings.audition = 0;
    }

    // ---- The cues: the loudest of each sound, the rest into the battle bed ----
    Best best[(int)EGameSound::Count];
    const float range = m_settings.hearingRange;
    const auto consider = [&](EGameSound sound, const glm::vec3& pos, float gain, float size, bool placed, float crowdScale, int variant = -1, float pitch = 1.0f)
    {
        const SoundDef& def = c_soundDefs[(int)sound];
        float loudness = gain * def.gain;
        if (def.place == PlaceWorld && placed)
        {
            const float dist = glm::distance(pos, frame.ear);
            if (dist > range)
                return;
            const float a = attenuation(dist);
            loudness *= a;
            m_crowdAccum += def.crowdWeight * a * crowdScale;
        }
        Best& b = best[(int)sound];
        if (loudness > b.loudness)
            b = Best{ pos, loudness, size, placed && def.place != PlaceFlat, variant, pitch };
    };

    // The components' cues. A kind over its bound was only counted: the cues that did arrive stand for all of them
    // in the battle bed (crowdScale).
    {
        uint32 counts[(int)GameUnitComponent::ESoundCue::Count];
        GameUnitComponent::takeSoundCues(m_cueScratch, counts);
        constexpr EGameSound c_cueSound[] = { EGameSound::UnitReveal, EGameSound::UnitDeath, EGameSound::ShotFired,
            EGameSound::LobFired, EGameSound::ShotImpact, EGameSound::SplashImpact };
        static_assert(oc::size(c_cueSound) == (size_t)GameUnitComponent::ESoundCue::Count);
        for (const uint32 count : counts)
            m_statCues += (int)count;
        for (const GameUnitComponent::SoundCue& cue : m_cueScratch)
        {
            const uint32 count = counts[(int)cue.kind];
            const float crowdScale = count > GameUnitComponent::MaxSoundCues ? (float)count / (float)GameUnitComponent::MaxSoundCues : 1.0f;
            const bool shell = cue.kind == GameUnitComponent::ESoundCue::SplashImpact;
            // A unit's size is its body radius over the grunt's (0.5 m); a shell's is 1.
            const float size = shell || cue.kind >= GameUnitComponent::ESoundCue::ShotFired ? 1.0f : glm::max(cue.size, 0.1f) * 2.0f;
            consider(c_cueSound[(int)cue.kind], cue.pos, 1.0f, size, true, crowdScale);
        }
    }
    // What the game posted.
    {
        size_t keep = 0;
        for (size_t i = 0; i < g_posted.size(); ++i)
        {
            const Posted p = g_posted[i];
            if (p.due > m_now)
            {
                g_posted[keep++] = p;
                continue;
            }
            ++m_statCues;
            // A sound over its bound: the cues that are here stand for all of them in the battle bed.
            const uint32 posted = g_postedOfSound[(int)p.sound];
            consider(p.sound, p.pos, p.gain, p.size, p.placed, posted > c_maxPostedOfSound ? (float)posted / (float)c_maxPostedOfSound : 1.0f,
                p.variant, p.pitch);
        }
        g_posted.resize(keep);
        for (uint32& count : g_postedOfSound)
            count = 0;
    }
    // One start per sound a frame.
    // THE BATTLE FOCUS: while a battle is on (busy = the battle bed's level x "Battle focus"), the bed carries the mass
    // and the one-shots must get FEWER, or the two say the same thing twice and the result is noise. A combat sound
    // (the Effects group) then starts up to 4 x less often, and only for a cue that is near: a far one is in the bed.
    const float busy = battleBusy();
    for (int s = 0; s < (int)EGameSound::Count; ++s)
    {
        const SoundDef& def = c_soundDefs[s];
        const bool combat = def.group == GroupEffects;
        const float interval = def.minInterval * (combat ? 1.0f + 3.0f * busy : 1.0f);
        const float audible = combat ? glm::max(0.004f, def.gain * 0.3f * busy) : 0.004f; // at full busy: within about 3 x "Full volume distance"
        if (best[s].loudness > audible && m_now - m_lastStart[s] >= (double)interval)
            startVoice((EGameSound)s, best[s].pos, best[s].loudness, best[s].size, best[s].placed, best[s].variant, best[s].pitch);
    }

    updateLoudnessLimit(dt);
    updateAmbience(frame, structures, dt); // posts for the next frame
    updateWater(frame, structures, dt);
    updateHorde(frame);
    updateBeds(frame, dt);
    updateStructureLoops(frame, structures, dt);

    m_statTimer += dt;
    if (m_statTimer >= 1.0f)
    {
        int playing = 0;
        for (const Voice& v : m_voices)
            playing += v.end > m_now ? 1 : 0;
        for (const LoopVoice& l : m_loops)
            playing += l.structureId != 0 ? 1 : 0;
        for (const Bed& bed : m_beds)
            playing += bed.playing ? 1 : 0;
        m_settings.voicesPlaying = playing;
        m_settings.cuesPerSecond = (int)((float)m_statCues / m_statTimer);
        m_settings.cuesPlayed = (int)((float)m_statStarts / m_statTimer);
        m_statTimer = 0.0f;
        m_statCues = m_statStarts = 0;
    }
}

glm::vec3 GameSound::heardAt(const glm::vec3& pos) const
{
    const glm::vec3 offset = pos - m_ear;
    return m_camPos + m_camRight * glm::dot(offset, m_earRight) + m_camUp * (offset.y * m_settings.panHeight)
        + m_camForward * glm::dot(offset, m_earForward);
}

void GameSound::applyDistance(AudioSource& source, float dist) const
{
    // The send is a share of the sound AFTER its gain, which already falls with the distance: so the share must
    // rise with it (the square root: at four times the distance, twice the share), or the reverb would die away as
    // fast as the direct sound and a far sound would be dry.
    const float full = glm::max(m_settings.fullVolumeDistance, 0.5f);
    source.setReverbSend(glm::min(m_settings.reverbSend * std::sqrt(glm::max(dist, full) / full), 1.0f));
    source.setLowpass(18000.0f * std::exp(-dist / glm::max(m_settings.airDistance, 1.0f)));
}

// THE LOUDNESS LIMIT (see m_duck): the power sum of every one-shot, bed, building loop and water voice at its own gain.
// The beds, the loops and the water set their gain every frame and take m_duck there; the one-shots get it here.
void GameSound::updateLoudnessLimit(float dt)
{
    // A one-shot dies away: it counts with the part of its time that is left, not at its full gain to its last
    // moment. (At full gain, every start and every end of a sound was a jump in the sum, and the limit turned
    // everything else down and up with it: the whole mix pumped.)
    float power = 0.0f;
    for (const Voice& v : m_voices)
        if (v.end > m_now)
        {
            const float left = glm::clamp((float)(v.end - m_now) / glm::max(v.length, 0.05f), 0.0f, 1.0f);
            power += v.gain * v.gain * left;
        }
    for (const Bed& bed : m_beds)
        if (bed.playing)
            power += bed.level * bed.level;
    for (const LoopVoice& loop : m_loops)
        if (loop.structureId != 0)
            power += loop.gain * loop.gain;
    for (const WaterVoice& water : m_water)
        if (water.playing)
            power += water.gain * water.gain;
    const float level = std::sqrt(power);
    const float limit = glm::max(m_settings.loudnessLimit, 0.05f);
    const float target = level > limit ? limit / level : 1.0f;
    // SLOW, both ways (down in about a fifth of a second, up in a second or more): it is a hand on the master fader
    // for a mix that stays too loud, not a limiter that follows single sounds. A fast one is heard as pumping.
    const float before = m_duck;
    m_duck += (target - m_duck) * glm::min(dt * (target < m_duck ? 5.0f : 0.8f), 1.0f);
    m_settings.loudness = level;
    if (glm::abs(m_duck - before) < 0.002f)
        return; // no change worth a call (the engine glides over 20 ms; a step under 0.2 % is nothing)
    for (Voice& v : m_voices)
        if (v.end > m_now)
            v.source.setGain(v.gain * m_duck);
}

void GameSound::startVoice(EGameSound sound, const glm::vec3& pos, float loudness, float size, bool positional, int givenVariant, float givenPitch)
{
    const SoundDef& def = c_soundDefs[(int)sound];
    if (m_variantCount[(int)sound] == 0)
        return;
    const size_t maxVoices = (size_t)glm::clamp(m_settings.maxVoices, 1, 48);
    if (m_voices.size() > maxVoices)
        m_voices.resize(maxVoices);

    // A big body is louder and lower.
    loudness *= glm::clamp(std::sqrt(size), 0.7f, 1.6f);
    Voice* freeVoice = nullptr;
    Voice* quietest = nullptr;
    int same = 0, combatVoices = 0;
    for (Voice& v : m_voices)
    {
        if (v.end <= m_now)
        {
            if (!freeVoice)
                freeVoice = &v;
            continue;
        }
        if (v.sound == sound)
            ++same;
        if (c_soundDefs[(int)v.sound].group == GroupEffects)
            ++combatVoices;
        if (!quietest || v.loudness < quietest->loudness)
            quietest = &v;
    }
    // THE BATTLE FOCUS (see update): while a battle is on, a combat sound has half its voices, and all combat sounds
    // together at most 40 % of the pool. A sound that has its voices DROPS the new cue: it never cuts one of its own
    // playing voices off (a sound cut in the middle is a click, and many of them are noise).
    const bool combat = def.group == GroupEffects;
    const float busy = combat ? battleBusy() : 0.0f;
    const int sameCap = glm::max((int)((float)def.maxVoices * (1.0f - 0.5f * busy) + 0.5f), 1);
    if (same >= sameCap)
        return;
    if (combat && combatVoices >= glm::max((int)((float)maxVoices * glm::mix(1.0f, 0.4f, busy)), 4))
        return;
    Voice* voice = nullptr;
    if (freeVoice)
        voice = freeVoice;
    else if (m_voices.size() < maxVoices)
        voice = &m_voices.emplace_back();
    else if (quietest && loudness > quietest->loudness * 1.25f)
        voice = quietest;
    else
        return;

    if (!voice->source.isValid())
        voice->source = Globals::audio.createSource();
    if (!voice->source.isValid())
        return;
    // VARIETY: never one of the last two variants (with three or more), and every start has its own pitch and volume.
    const int variants = m_variantCount[(int)sound];
    int variant = (int)(rand01() * (float)variants) % variants;
    if (givenVariant >= 0)
        variant = givenVariant % variants; // a perch's own song
    else
    {
        for (int tries = 0; tries < 2; ++tries)
            if ((variants > 1 && variant == (int)m_lastVariant[(int)sound]) || (variants > 2 && variant == (int)m_prevVariant[(int)sound]))
                variant = (variant + 1) % variants;
        m_prevVariant[(int)sound] = m_lastVariant[(int)sound];
        m_lastVariant[(int)sound] = (uint8)variant;
    }
    loudness *= 1.0f + (rand01() * 2.0f - 1.0f) * def.gainJitter;
    const AudioBuffer& buffer = bufferOf(sound, variant);

    const float volumes[] = { m_settings.effectsVolume, m_settings.structureVolume, m_settings.ambienceVolume,
        m_settings.crowdVolume, m_settings.interfaceVolume };
    // The pitch shift is a RATIO (as far down as up), and the sum of two rolls: most starts are near the sound's own
    // pitch, a few are far from it.
    const float pitch = glm::clamp(std::exp2((rand01() + rand01() - 1.0f) * std::log2(1.0f + def.pitchJitter))
        * glm::clamp(std::pow(glm::max(size, 0.05f), -0.4f), 0.55f, 1.3f) * givenPitch, 0.3f, 3.0f);
    // THE ORDER MATTERS: every value first, the buffer last. The engine glides every volume change over 20 ms, except
    // the FIRST one of a new sound - and setBuffer makes the new sound and sets its volume from what the source holds.
    // With the gain set after it, the sound would start at the source's LAST gain and glide to its own: a loud start
    // on a quiet sound.
    AudioSource& source = voice->source;
    voice->gain = loudness * volumes[def.group];
    source.setLooping(false);
    source.setRelative(!positional);
    if (positional)
    {
        source.setAttenuation(1.0f, c_noFalloff, 0.0f); // the distance is in the gain already
        source.setPosition(heardAt(pos));
    }
    voice->pos = pos;
    voice->positional = positional;
    if (positional && def.place == PlaceWorld)
        applyDistance(source, glm::distance(pos, m_ear));
    else
    {
        // A far-field sound (thunder) has its distance in its own samples: a fixed share to the reverb, no filter.
        // A sound with no place stays out of the room.
        source.setReverbSend(positional ? 0.3f : 0.0f);
        source.setLowpass(20000.0f);
    }
    source.setGain(voice->gain * m_duck);
    source.setPitch(pitch);
    source.setBuffer(buffer);
    source.play();
    voice->sound = sound;
    voice->loudness = loudness;
    voice->length = buffer.getDurationSec() / pitch;
    voice->end = m_now + (double)voice->length + 0.03;
    m_lastStart[(int)sound] = m_now;
    ++m_statStarts;
}

void GameSound::setBed(int index, EGameSound sound, float gain, float dt, float pitch)
{
    Bed& bed = m_beds[index];
    bed.gain += (gain - bed.gain) * glm::min(dt * 3.0f, 1.0f);
    if (bed.gain < 0.003f && gain <= 0.0f)
    {
        if (bed.playing)
            bed.source.stop();
        bed.playing = false;
        bed.gain = 0.0f;
        return;
    }
    if (!bed.playing)
    {
        if (!bed.source.isValid())
        {
            bed.source = Globals::audio.createSource();
            if (!bed.source.isValid())
                return;
            bed.source.setLooping(true);
            bed.source.setRelative(c_soundDefs[(int)sound].place == PlaceFlat);
            bed.source.setAttenuation(1.0f, c_noFalloff, 0.0f);
            bed.source.setGain(0.0f); // before the buffer: a loop comes up from nothing (see startVoice on the order)
            bed.source.setBuffer(bufferOf(sound, 0));
        }
        bed.source.play();
        bed.playing = true;
    }
    const SoundDef& def = c_soundDefs[(int)sound];
    bed.level = bed.gain * def.gain;
    bed.source.setGain(bed.level * m_duck * (1.0f + drift((float)index * 3.7f, 0.23f) * def.gainJitter));
    bed.source.setPitch(pitch * (1.0f + drift((float)index * 5.3f + 1.0f, 0.11f) * def.pitchJitter));
}

// A slow -1..1 curve that never repeats in step with a loop: three sines whose rates share no period.
float GameSound::drift(float seed, float rate) const
{
    const float t = (float)m_now * rate;
    return 0.5f * std::sin(t + seed) + 0.3f * std::sin(t * 2.31f + seed * 1.7f) + 0.2f * std::sin(t * 5.07f + seed * 0.6f);
}

void GameSound::updateBeds(const Frame& frame, float dt)
{
    // The battle bed's level: the rate of weighted combat cues, sampled four times a second. Up fast, down slowly.
    m_crowdTimer += dt;
    if (m_crowdTimer >= 0.25f)
    {
        const float target = glm::clamp(m_crowdAccum / m_crowdTimer / glm::max(m_settings.crowdFull, 1.0f), 0.0f, 1.0f);
        m_crowdLevel = glm::mix(m_crowdLevel, target, target > m_crowdLevel ? 0.6f : 0.15f);
        m_crowdAccum = 0.0f;
        m_crowdTimer = 0.0f;
    }
    // THE FEAR: the animals know the demons are there before the player sees them. Enemies in the horde radius (a few
    // are enough: "Enemies that silence the animals") or a fight near the ear. Up fast, down slowly - and the wind
    // goes on, so what is left is the wind alone.
    const float scare = glm::max(glm::clamp(m_hordeNear / glm::max(m_settings.fearUnits, 0.1f), 0.0f, 1.0f), glm::clamp(m_crowdLevel * 3.0f, 0.0f, 1.0f));
    if (scare > m_fear)
        m_fear = glm::min(m_fear + dt / 1.5f, scare);
    else
        m_fear = glm::max(m_fear - dt / glm::max(m_settings.fearReturn, 1.0f), scare);

    const float ambience = m_settings.ambienceVolume;
    const float crowd = m_settings.crowdVolume;
    const float day = glm::clamp(frame.daylight, 0.0f, 1.0f);
    setBed(BedWind, EGameSound::LoopWind, ambience * (frame.outdoors ? 1.0f : 0.5f), dt);
    setBed(BedDay, EGameSound::LoopDay, frame.outdoors ? ambience * day : 0.0f, dt); // leaves only: no animal in it
    // The night loop is a far cricket field: it thins out with the fear, down to the night air.
    setBed(BedNight, EGameSound::LoopNight, frame.outdoors ? ambience * (1.0f - day) * (1.0f - 0.85f * m_fear) : 0.0f, dt);
    // The level curve is nearly straight (power 0.8). A square root made both beds loud from the first few cues or
    // units on: a bed that is always nearly at full level is a drone, not a crowd that grows.
    setBed(BedBattle, EGameSound::LoopBattle, m_crowdLevel > 0.01f ? crowd * std::pow(m_crowdLevel, 0.8f) : 0.0f, dt, 0.92f + 0.16f * m_crowdLevel);
    if (m_hordePosValid && m_beds[BedHorde].source.isValid())
    {
        m_beds[BedHorde].source.setPosition(heardAt(m_hordePos));
        applyDistance(m_beds[BedHorde].source, glm::distance(m_hordePos, m_ear));
    }
    // The horde bed is the horde BEFORE the fight (they come). In the fight the battle bed is the crowd: two crowd
    // loops at full level are one noise, so the horde bed steps back as the battle bed comes up.
    setBed(BedHorde, EGameSound::LoopHorde, m_hordeLevel > 0.01f ? crowd * std::pow(m_hordeLevel, 0.8f) * (1.0f - 0.7f * m_crowdLevel) : 0.0f, dt);
}

float GameSound::battleBusy() const
{
    return glm::clamp(m_crowdLevel * m_settings.battleFocus, 0.0f, 1.0f);
}

// THE HORDE: how many units of another team stand near the ear, and where. A slice of the World's root list a frame
// (every unit is a root); the list only changes on the main thread, where this runs.
void GameSound::updateHorde(const Frame& frame)
{
    const oc::vector<EntityPtr>& roots = Globals::world.rootEntities();
    const uint32 count = (uint32)roots.size();
    const float radius = glm::max(m_settings.hordeRadius, 1.0f);
    const uint32 slice = glm::max(count / 16u, 512u);
    uint32 i = glm::min(m_hordeCursor, count);
    for (const uint32 end = glm::min(i + slice, count); i < end; ++i)
    {
        Entity* e = roots[i].get();
        const GameUnitComponent* unit = getComponent<GameUnitComponent>(e);
        if (!unit || unit->puppet || !unit->alive() || (int)unit->team == frame.localTeam)
            continue;
        const float dist = glm::distance(e->pos, frame.ear);
        if (dist >= radius)
            continue;
        const float w = 1.0f - dist / radius;
        m_hordeWeight += w;
        m_hordeSum += e->pos * w;
    }
    m_hordeCursor = i;
    if (i < count)
        return;
    m_hordeLevel = glm::clamp(m_hordeWeight / glm::max(m_settings.hordeFull, 1.0f), 0.0f, 1.0f);
    m_hordeNear = m_hordeWeight;
    if (m_hordeWeight > 0.0f)
    {
        const glm::vec3 centroid = m_hordeSum / m_hordeWeight;
        m_hordePos = m_hordePosValid ? glm::mix(m_hordePos, centroid, 0.5f) : centroid;
        m_hordePosValid = true;
    }
    m_hordeWeight = 0.0f;
    m_hordeSum = glm::vec3(0.0f);
    m_hordeCursor = 0;
}

// THE BUILDING LOOPS: "Max building loops" voices, on the nearest built, powered buildings that have a loop. Picked
// four times a second; a voice that lost its building fades out before it takes another.
void GameSound::updateStructureLoops(const Frame& frame, const StructureSystem& structures, float dt)
{
    const size_t maxLoops = (size_t)glm::clamp(m_settings.maxLoops, 0, 12);
    if (m_loops.size() != maxLoops)
        m_loops.resize(maxLoops);
    m_loopTimer -= dt;
    if (m_loopTimer <= 0.0f)
    {
        m_loopTimer = 0.25f;
        const float range = glm::min(m_settings.hearingRange, 60.0f);
        m_loopCandidates.clear();
        const int count = structures.structureCount();
        for (int i = 0; i < count; ++i)
        {
            const StructureSystem::Ref& s = structures.structures()[i];
            const EGameSound sound = loopSoundOf(s.type);
            if (sound == EGameSound::Count || s.state->blueprint || s.state->rubble || !s.state->powered)
                continue;
            const float dist = glm::distance(s.entity->pos, frame.ear);
            if (dist < (sound == EGameSound::LoopHum ? range * 0.5f : range)) // the hum is a near sound: half the reach
                m_loopCandidates.push_back(LoopCandidate{ s.state->structureId, sound, s.entity->pos, dist });
        }
        // The nearest first, and AT MOST ONE HUM, two fires and two workshops: a loop is a steady sound, and several of
        // one kind do not read as several buildings - they add up to one louder, rougher drone (six field hums, each at
        // its own pitch, were a wall of noise over every base). One hum says "a field is here".
        oc::sort(m_loopCandidates.begin(), m_loopCandidates.end(),
            [](const LoopCandidate& a, const LoopCandidate& b) { return a.dist < b.dist; });
        int hums = 0, fires = 0, works = 0;
        size_t kept = 0;
        for (const LoopCandidate& c : m_loopCandidates)
        {
            int& ofKind = c.sound == EGameSound::LoopHum ? hums : c.sound == EGameSound::LoopFire ? fires : works;
            if (ofKind >= (c.sound == EGameSound::LoopHum ? 1 : 2) || kept >= maxLoops)
                continue;
            ++ofKind;
            m_loopCandidates[kept++] = c;
        }
        m_loopCandidates.resize(kept);
        for (LoopVoice& voice : m_loops)
            voice.wanted = false;
        for (LoopCandidate& c : m_loopCandidates)
            for (LoopVoice& voice : m_loops)
                if (voice.structureId == c.id)
                {
                    voice.wanted = true;
                    c.id = 0; // it has its voice
                }
        for (const LoopCandidate& c : m_loopCandidates)
        {
            if (c.id == 0)
                continue;
            for (LoopVoice& voice : m_loops)
            {
                if (voice.structureId != 0)
                    continue; // still fading out for its old building
                if (!voice.source.isValid())
                    voice.source = Globals::audio.createSource();
                if (!voice.source.isValid())
                    break;
                voice.structureId = c.id;
                voice.sound = c.sound;
                voice.pos = c.pos;
                voice.gain = 0.0f;
                voice.wanted = true;
                // Two buildings of one type must not play in phase: a pitch per building, off its id.
                voice.pitch = 0.94f + 0.12f * (float)((c.id * 2654435761u) >> 24) / 255.0f;
                voice.source.setLooping(true);
                voice.source.setRelative(false);
                voice.source.setAttenuation(1.0f, c_noFalloff, 0.0f);
                voice.source.setPosition(c.pos);
                voice.source.setGain(0.0f); // every value before the buffer (see startVoice on the order)
                voice.source.setPitch(voice.pitch);
                voice.source.setBuffer(bufferOf(c.sound, 0));
                voice.source.play();
                break;
            }
        }
    }
    for (LoopVoice& voice : m_loops)
    {
        if (voice.structureId == 0)
            continue;
        const float target = voice.wanted
            ? attenuation(glm::distance(voice.pos, frame.ear)) * c_soundDefs[(int)voice.sound].gain * m_settings.structureVolume : 0.0f;
        voice.gain += (target - voice.gain) * glm::min(dt * 4.0f, 1.0f);
        if (!voice.wanted && voice.gain < 0.005f)
        {
            voice.source.stop();
            voice.structureId = 0;
            continue;
        }
        // Each building's loop drifts by itself (its id is the seed): one fire flares while the one next to it dies down.
        const SoundDef& def = c_soundDefs[(int)voice.sound];
        const float seed = (float)(voice.structureId % 1024u);
        voice.source.setPosition(heardAt(voice.pos));
        applyDistance(voice.source, glm::distance(voice.pos, m_ear));
        voice.source.setGain(voice.gain * m_duck * (1.0f + drift(seed, 0.3f) * def.gainJitter));
        voice.source.setPitch(voice.pitch * (1.0f + drift(seed + 11.0f, 0.13f) * def.pitchJitter));
    }
}

// THE PERCHES (the sandbox world): each one is a place in the world with one animal - or, a gust, with none - that
// makes its sound from there. A bird and a cricket keep their place and their song (the variant) while the player is
// in reach; one that the player left behind gets a new place near the ear. A gust takes a new place every time.
void GameSound::updateAmbience(const Frame& frame, const StructureSystem& structures, float dt)
{
    if (!frame.outdoors)
        return;
    struct Kind
    {
        EGameSound sound;
        float nearDist, farDist; // where a new place goes, from the ear
        float leave;             // farther than this: a new place
        float lowHeight, highHeight; // over the ground
        float minPause, maxPause;    // s between two sounds
    };
    constexpr Kind c_kinds[] = {
        { EGameSound::BirdSong,    12.0f, 65.0f, 85.0f,  4.0f, 14.0f, 5.0f, 20.0f },
        { EGameSound::CricketBout, 6.0f,  40.0f, 55.0f,  0.1f, 0.3f,  2.5f, 6.0f }, // the bout is 1 - 3 s of it
        { EGameSound::OwlCall,     40.0f, 80.0f, 100.0f, 6.0f, 12.0f, 12.0f, 35.0f },
        { EGameSound::WindGust,    15.0f, 45.0f, 60.0f,  2.0f, 12.0f, 3.0f, 10.0f } };
    if (!m_perchesInit)
    {
        m_perchesInit = true;
        for (int i = 0; i < c_perches; ++i)
        {
            Perch& perch = m_perches[i];
            perch.kind = i < 8 ? PerchBird : i < 15 ? PerchCricket : i < 16 ? PerchOwl : PerchGust;
            perch.timer = rand01() * c_kinds[perch.kind].maxPause; // not all at once
        }
    }
    const float day = glm::clamp(frame.daylight, 0.0f, 1.0f);
    for (Perch& perch : m_perches)
    {
        const Kind& kind = c_kinds[perch.kind];
        const glm::vec2 away(perch.pos.x - frame.ear.x, perch.pos.z - frame.ear.z);
        if (!perch.placed || glm::length(away) > kind.leave)
        {
            const float angle = rand01() * glm::two_pi<float>();
            const float dist = glm::mix(kind.nearDist, kind.farDist, std::sqrt(rand01())); // even over the area
            const float x = frame.ear.x + std::cos(angle) * dist, z = frame.ear.z + std::sin(angle) * dist;
            perch.pos = glm::vec3(x, structures.groundHeightAt(x, z, frame.ear.y, true) + glm::mix(kind.lowHeight, kind.highHeight, rand01()), z);
            perch.variant = (uint8)(rand01() * 120.0f); // another animal: another song (startVoice takes it modulo the count)
            perch.pitch = 0.7f + 0.65f * rand01();      // a bird's voice: small birds high, big ones low
            perch.placed = true;
        }
        perch.timer -= dt;
        if (perch.timer > 0.0f)
            continue;
        perch.timer = glm::mix(kind.minPause, kind.maxPause, rand01());
        // Birds sing by day (most at full light), crickets and the owl by night. A cricket is silent while someone
        // stands next to it. The wind does not care.
        float chance = 1.0f;
        if (perch.kind == PerchBird)
            chance = glm::smoothstep(0.25f, 0.7f, day);
        else if (perch.kind == PerchCricket)
            chance = (1.0f - glm::smoothstep(0.2f, 0.5f, day)) * (glm::distance(perch.pos, frame.ear) > 5.0f ? 1.0f : 0.0f);
        else if (perch.kind == PerchOwl)
            chance = day < 0.15f ? 1.0f : 0.0f;
        // THE FEAR: an animal is less and less likely to make a sound, and none does from a fear of 0.7. They do not
        // stop at one moment and do not come back at one moment: each one rolls for itself.
        if (perch.kind != PerchGust)
            chance *= glm::clamp(1.0f - m_fear / 0.7f, 0.0f, 1.0f);
        if (rand01() < chance)
        {
            if (perch.kind == PerchBird)
                singBird(perch);
            else
                postVariant(kind.sound, perch.pos, 0.6f + 0.4f * rand01(), perch.variant);
        }
        if (perch.kind == PerchGust)
            perch.placed = false;
    }
}

// A BIRD SINGS: no song is a recording. The `BirdSong` buffers are single NOTES (0 a flat whistle, 1 a rising chirp,
// 2 a falling chirp, 3 a tick, 4 a long soft whistle, 5 a rough triple pulse), and a song is a run of note cues made
// here, each with its own time, pitch and level. The bird has a voice (perch.pitch) and a STYLE (perch.variant % 6) -
// which notes, how fast, which way the pitch goes - but every number is rolled again each time: how many notes, every
// gap, every interval. So one hears "that kind of bird", never "that sound again".
// MOST of what a bird says is a plain CALL (one to three of its usual chirps); the full song is the rare one.
void GameSound::singBird(const Perch& perch)
{
    const int style = perch.variant % 6;
    const float level = 0.55f + 0.45f * rand01();
    float t = 0.0f;
    const auto roll = [&](float low, float high) { return low + (high - low) * rand01(); };
    const auto note = [&](int variant, float pitch, float gain)
    {
        postVariant(EGameSound::BirdSong, perch.pos, level * gain, variant, perch.pitch * pitch, t);
    };
    const auto count = [&](int low, int high) { return low + glm::min((int)(rand01() * (float)(high - low + 1)), high - low); };

    if (rand01() > 0.3f)
    {
        // A call: the bird's usual chirp (rising, falling or flat, by its style), once to three times, never even.
        const int usual = style % 3 == 0 ? 1 : style % 3 == 1 ? 2 : 0;
        const float pitch = roll(0.95f, 1.08f);
        for (int k = 0, n = count(1, 3); k < n; ++k)
        {
            note(usual, pitch * roll(0.97f, 1.03f), roll(0.7f, 1.0f));
            t += roll(0.11f, 0.32f);
        }
        return;
    }
    switch (style)
    {
    case 0: // whistled notes that step to another pitch each time
    {
        float pitch = roll(0.95f, 1.1f);
        constexpr float c_steps[] = { 0.79f, 0.84f, 0.89f, 1.0f, 1.12f, 1.19f, 1.26f };
        for (int k = 0, n = count(2, 4); k < n; ++k)
        {
            note(rand01() < 0.6f ? 0 : 4, pitch, roll(0.8f, 1.0f));
            t += roll(0.16f, 0.36f);
            pitch = glm::clamp(pitch * c_steps[count(0, 6)], 0.7f, 1.5f);
        }
        break;
    }
    case 1: // a warble: quick chirps, up and down, at any pitch
        for (int k = 0, n = count(4, 10); k < n; ++k)
        {
            note(rand01() < 0.5f ? 1 : 2, roll(0.8f, 1.35f), roll(0.6f, 1.0f));
            t += roll(0.065f, 0.12f);
        }
        break;
    case 2: // a trill: ticks at one speed (another speed each time) whose pitch and level drift
    {
        const float gap = roll(0.05f, 0.075f);
        float pitch = roll(1.3f, 1.7f);
        const float slide = roll(0.985f, 1.015f);
        for (int k = 0, n = count(6, 18); k < n; ++k)
        {
            note(3, pitch, glm::min(0.5f + 0.1f * (float)k, 1.0f));
            t += gap * roll(0.93f, 1.07f);
            pitch *= slide;
        }
        break;
    }
    case 3: // notes that step down, each a little later than the last
    {
        float pitch = roll(1.2f, 1.45f), gap = roll(0.13f, 0.18f);
        for (int k = 0, n = count(3, 5); k < n; ++k)
        {
            note(k + 1 == n ? 4 : 0, pitch, 1.0f - 0.1f * (float)k);
            t += gap;
            gap *= roll(1.05f, 1.25f);
            pitch *= roll(0.82f, 0.92f);
        }
        break;
    }
    case 4: // one rising chirp, again and again - how many, how fast: another answer each time
    {
        const float pitch = roll(0.9f, 1.1f), gap = roll(0.15f, 0.27f);
        for (int k = 0, n = count(2, 6); k < n; ++k)
        {
            note(1, pitch * roll(0.98f, 1.02f), roll(0.8f, 1.0f));
            t += gap * roll(0.9f, 1.12f);
        }
        break;
    }
    default: // a low chatter: rough pulses and ticks, uneven
        for (int k = 0, n = count(5, 13); k < n; ++k)
        {
            note(rand01() < 0.6f ? 5 : 3, roll(0.6f, 0.82f), roll(0.5f, 1.0f));
            t += roll(0.06f, 0.13f);
        }
        break;
    }
}

// THE WATER: 48 points around the ear (16 directions, at 6, 18 and 40 m), four of them tested a frame: a point is water
// where the water surface is over the ground. When the round is over, the nearest wet point, and the nearest one on the
// other side of the ear from it, each get a loop voice (it glides to a new place, it does not jump); single laps
// sound near the first.
void GameSound::updateWater(const Frame& frame, const StructureSystem& structures, float dt)
{
    const SoundDef& def = c_soundDefs[(int)EGameSound::LoopWater];
    const bool active = frame.outdoors && Globals::physics.isWaterActive();
    if (active)
    {
        constexpr float c_ring[3] = { 6.0f, 18.0f, 40.0f };
        for (int n = 0; n < 4; ++n)
        {
            const int i = m_waterProbe;
            const float angle = glm::two_pi<float>() * (float)(i % 16) / 16.0f;
            const float x = frame.ear.x + std::cos(angle) * c_ring[i / 16], z = frame.ear.z + std::sin(angle) * c_ring[i / 16];
            const float ground = structures.groundHeightAt(x, z, -FLT_MAX, true);
            const float water = Globals::physics.sampleWaterHeight(x, z);
            m_waterWet[i] = ground != -FLT_MAX && water > ground + 0.1f;
            m_waterHit[i] = glm::vec3(x, water, z);
            if (++m_waterProbe < c_waterProbes)
                continue;
            m_waterProbe = 0;
            int nearest = -1, other = -1;
            for (int k = 0; k < c_waterProbes; ++k)
                if (m_waterWet[k] && (nearest < 0 || k / 16 < nearest / 16)) // the rings are in the order of their distance
                    nearest = k;
            if (nearest >= 0)
            {
                const float nearAngle = glm::two_pi<float>() * (float)(nearest % 16) / 16.0f;
                for (int k = 0; k < c_waterProbes; ++k)
                {
                    const float a = glm::two_pi<float>() * (float)(k % 16) / 16.0f;
                    if (m_waterWet[k] && std::cos(a - nearAngle) < 0.0f && (other < 0 || k / 16 < other / 16))
                        other = k;
                }
            }
            const int picks[2] = { nearest, other };
            for (int v = 0; v < 2; ++v)
            {
                m_water[v].wanted = picks[v] >= 0;
                if (picks[v] >= 0)
                {
                    m_water[v].target = m_waterHit[picks[v]];
                    if (!m_water[v].playing)
                        m_water[v].pos = m_water[v].target;
                }
            }
        }
    }
    for (int v = 0; v < 2; ++v)
    {
        WaterVoice& voice = m_water[v];
        if (!active)
            voice.wanted = false;
        voice.pos += (voice.target - voice.pos) * glm::min(dt * 1.5f, 1.0f);
        const float target = voice.wanted ? attenuation(glm::distance(voice.pos, frame.ear)) * def.gain * m_settings.ambienceVolume : 0.0f;
        voice.gain += (target - voice.gain) * glm::min(dt * 2.0f, 1.0f);
        if (voice.gain < 0.003f && !voice.wanted)
        {
            if (voice.playing)
                voice.source.stop();
            voice.playing = false;
            continue;
        }
        if (!voice.playing)
        {
            if (!voice.source.isValid())
            {
                voice.source = Globals::audio.createSource();
                if (!voice.source.isValid())
                    continue;
                voice.source.setLooping(true);
                voice.source.setRelative(false);
                voice.source.setAttenuation(1.0f, c_noFalloff, 0.0f);
                voice.source.setPosition(voice.pos);
                voice.source.setGain(0.0f); // every value before the buffer (see startVoice on the order)
                voice.source.setBuffer(bufferOf(EGameSound::LoopWater, 0));
            }
            voice.source.play();
            voice.playing = true;
        }
        voice.source.setPosition(heardAt(voice.pos));
        applyDistance(voice.source, glm::distance(voice.pos, m_ear));
        voice.source.setGain(voice.gain * m_duck * (1.0f + drift(40.0f + (float)v * 9.0f, 0.3f) * def.gainJitter));
        // The two voices play one loop: another pitch each, so they are never the same water.
        voice.source.setPitch((v == 0 ? 1.0f : 0.88f) * (1.0f + drift(50.0f + (float)v * 7.0f, 0.13f) * def.pitchJitter));
    }
    // Single laps, near the first voice: the water is never the loop alone.
    m_waterLapTimer -= dt;
    if (m_waterLapTimer <= 0.0f)
    {
        m_waterLapTimer = 1.5f + 3.5f * rand01();
        if (m_water[0].wanted)
            post(EGameSound::WaterLap, m_water[0].target + glm::vec3(rand01() * 8.0f - 4.0f, 0.0f, rand01() * 8.0f - 4.0f), 0.6f + 0.4f * rand01());
    }
}

// THE ALARM: the own team's built structures lost health since the last sample (twice a second), with none removed -
// a demolish takes health out of the sum too, and is no attack.
void GameSound::updateAlarm(const Frame& frame, const StructureSystem& structures)
{
    const float dt = (float)Globals::time.getDeltaSec();
    m_alarmCooldown = glm::max(m_alarmCooldown - dt, 0.0f);
    m_alarmTimer -= dt;
    if (m_alarmTimer > 0.0f)
        return;
    m_alarmTimer = 0.5f;
    float health = 0.0f;
    int count = 0;
    for (const StructureSystem::Ref& s : structures.structures())
    {
        if ((int)s.state->team != frame.localTeam || s.state->blueprint || s.state->rubble || isCableOrCrossing(s.type))
            continue;
        health += s.state->health;
        ++count;
    }
    if (m_alarmHealth >= 0.0f && count >= m_alarmCount && health < m_alarmHealth - 0.5f && m_alarmCooldown <= 0.0f)
    {
        post2D(EGameSound::Alarm);
        m_alarmCooldown = 10.0f;
    }
    m_alarmHealth = health;
    m_alarmCount = count;
}
