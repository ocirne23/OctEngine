export module Game:Sound;

import Core;
import Core.glm;
import Core.Camera;
import Audio;
import Entity;
import Settings;
import :Structures;

// THE GAME'S SOUND. Every sound is a GameSound cue: Game code posts it (GameSound::post, main thread), the unit and
// projectile components report theirs from the entity pass (GameUnitComponent::reportSound), and update() - once per
// windowed frame - decides what is HEARD. Nothing plays at the place a cue is posted.
//
// THE LIMITS (30 000 units may attack and die in one second):
//   - a cue beyond "Hearing range" of the EAR is dropped before anything else;
//   - per sound, ONE start a frame (the loudest cue), at most `minInterval` apart, at most `maxVoices` at a time;
//   - all one-shots share "Max one-shot voices"; when all are busy a louder cue takes the quietest voice;
//   - what the limits drop is not lost: every combat cue adds to the BATTLE BED, one loop whose volume follows the
//     rate of combat cues near the ear. Many fights are one loop plus a few clear one-shots, never thousands of voices.
// The HORDE BED is the same idea for presence: one loop at the centroid of the enemy units near the ear (a sliced walk
// of the World's root list). BUILDINGS: "Max building loops" loop voices go to the nearest buildings that have a loop.
//
// THE AMBIENCE (the sandbox world) is POSITIONAL: the flat beds (wind, day, night) are only the far background. In front
// of them, PERCHES - places in the world around the ear, each with one animal that has its own song (a sound variant)
// and sings from there again and again: birds by day, crickets and an owl by night. A perch stays where it is while the
// player walks, and moves only when the player has left it behind. Wind GUSTS come from a new place every time. WATER:
// a scan of points around the ear finds the nearest water in two directions; a loop voice stands on each, with single
// laps on top.
//
// THE SOUNDS are synthesized at the first update (SoundSynth.cpp). A file `Sounds/Game/<name>.wav` (`<name>2.wav`,
// `<name>3.wav` for the variants) replaces a synthesized sound: the names are in c_soundDefs (Sound.cpp).
export enum class EGameSound : uint8
{
    // One-shots in the world
    MeleeHit, UnitReveal, UnitDeath, ShotFired, LobFired, ShotImpact, SplashImpact, TurretFire,
    StructurePlaced, StructureBuilt, StructureCollapse,
    // The ambient one-shots in the world: GameSound places them itself, around the ear (THE AMBIENCE, below)
    BirdSong, CricketBout, OwlCall, WindGust, WaterLap,
    // One-shots with a direction but no distance (the far field)
    Thunder, WaveHorn,
    // One-shots with no place
    Alarm, PlayerHurt,
    // Loops
    LoopWind, LoopDay, LoopNight, LoopBattle, LoopHorde, LoopHum, LoopFire, LoopWorks, LoopWater,
    Count,
};
export constexpr int c_gameSoundVariants = 6; // buffers per one-shot at most

// SoundSynth.cpp: one sound's samples (interleaved when stereo), peak-normalized. `rate` is the sample rate.
void synthGameSound(EGameSound sound, int variant, oc::vector<float>& out, bool& stereo, uint32& rate);

export class GameSound final
{
public:
    // Queue a one-shot at a place. Main thread only. delaySec: it becomes due that much later (thunder).
    static void post(EGameSound sound, const glm::vec3& pos, float gain = 1.0f, float size = 1.0f, float delaySec = 0.0f);
    static void post2D(EGameSound sound, float gain = 1.0f);
    // One given variant at a given pitch, delaySec from now: a bird's song is a run of these (one per note).
    static void postVariant(EGameSound sound, const glm::vec3& pos, float gain, int variant, float pitch = 1.0f, float delaySec = 0.0f);
    static void discardPosted();

    struct Frame
    {
        const Camera* camera = nullptr;
        glm::vec3 ear{ 0.0f };     // distances are measured from here (the player; the camera gives the direction)
        float daylight = 1.0f;     // 0 = night .. 1 = day
        bool outdoors = false;     // the sandbox world: the day and night beds play
        int localTeam = 0;         // the alarm's team; every other team's unit is the horde
        float playerHealth = -1.0f; // < 0 = no player body
    };
    void update(const Frame& frame, const StructureSystem& structures);

private:
    void load();
    void drainCues(const Frame& frame);
    void startVoice(EGameSound sound, const glm::vec3& pos, float loudness, float size, bool positional, int variant = -1, float pitch = 1.0f);
    void updateAmbience(const Frame& frame, const StructureSystem& structures, float dt);
    void updateWater(const Frame& frame, const StructureSystem& structures, float dt);
    void updateBeds(const Frame& frame, float dt);
    void updateHorde(const Frame& frame);
    void updateStructureLoops(const Frame& frame, const StructureSystem& structures, float dt);
    void updateAlarm(const Frame& frame, const StructureSystem& structures);
    float attenuation(float dist) const;
    float battleBusy() const; // 0..1: how much the combat one-shots step back for the battle bed
    float drift(float seed, float rate) const; // a slow -1..1 curve: a loop's pitch and volume follow it
    const AudioBuffer& bufferOf(EGameSound sound, int variant) const { return m_buffers[(int)sound * c_gameSoundVariants + variant]; }

    GameSoundSettings& m_settings = Globals::settings.game.sound;
    bool m_loaded = false;
    oc::vector<AudioBuffer> m_buffers; // [sound * c_gameSoundVariants + variant]; an invalid one = no such variant
    uint8 m_variantCount[(int)EGameSound::Count] = {};
    uint8 m_lastVariant[(int)EGameSound::Count] = {};
    uint8 m_prevVariant[(int)EGameSound::Count] = {}; // the one before the last: a new start takes neither
    double m_lastStart[(int)EGameSound::Count] = {}; // real seconds
    double m_now = 0.0;
    uint32 m_rng = 0x9e3779b9u;
    float rand01();

    struct Voice
    {
        AudioSource source;
        EGameSound sound = EGameSound::Count;
        float loudness = 0.0f;
        double end = 0.0; // real seconds: free from then on
    };
    oc::vector<Voice> m_voices;

    // The beds: one looping source each (2D, or the horde's at its centroid).
    struct Bed
    {
        AudioSource source;
        float gain = 0.0f;
        bool playing = false;
    };
    enum { BedWind, BedDay, BedNight, BedBattle, BedHorde, BedCount };
    Bed m_beds[BedCount];
    void setBed(int bed, EGameSound sound, float gain, float dt, float pitch = 1.0f);
    float m_crowdAccum = 0.0f;  // weighted combat cues since the last rate sample
    float m_crowdTimer = 0.0f;
    float m_crowdLevel = 0.0f;  // 0..1, smoothed
    // The horde walk: a slice of the World's root list a frame; a full cycle publishes the count and the centroid.
    uint32 m_hordeCursor = 0;
    float m_hordeWeight = 0.0f;
    glm::vec3 m_hordeSum{ 0.0f };
    float m_hordeLevel = 0.0f;  // 0..1, the last cycle's
    float m_hordeNear = 0.0f;   // the last cycle's enemy units in the radius, weighted by nearness
    // THE FEAR of the animals, 0..1: up within a second or two when enemies come near or a fight starts, down slowly
    // ("Animals return (s)") after they are gone. Birds, crickets and the owl fall silent with it; the wind does not.
    float m_fear = 0.0f;
    glm::vec3 m_hordePos{ 0.0f };
    bool m_hordePosValid = false;

    struct LoopVoice
    {
        AudioSource source;
        uint32 structureId = 0; // 0 = free
        EGameSound sound = EGameSound::Count;
        glm::vec3 pos{ 0.0f };
        float gain = 0.0f;
        float pitch = 1.0f; // the building's own, off its id
        bool wanted = false;
    };
    oc::vector<LoopVoice> m_loops;
    float m_loopTimer = 0.0f;
    struct LoopCandidate { uint32 id; EGameSound sound; glm::vec3 pos; float dist; };
    oc::vector<LoopCandidate> m_loopCandidates; // kept: refilled at every pick

    // ---- The ambience (see the header) ----
    enum EPerch : uint8 { PerchBird, PerchCricket, PerchOwl, PerchGust };
    struct Perch
    {
        glm::vec3 pos{ 0.0f };
        float timer = 0.0f;   // s to its next sound
        EPerch kind = PerchBird;
        uint8 variant = 0;    // a cricket's or an owl's own sound; a bird's STYLE of song (singBird)
        float pitch = 1.0f;   // a bird's own voice: every note it sings is at this pitch, times the note's
        bool placed = false;
    };
    void singBird(const Perch& perch);
    static constexpr int c_perches = 18; // 8 birds, 7 crickets, 1 owl, 2 gusts
    Perch m_perches[c_perches];
    bool m_perchesInit = false;

    static constexpr int c_waterProbes = 48; // 16 directions x 3 distances
    struct WaterVoice
    {
        AudioSource source;
        glm::vec3 pos{ 0.0f }, target{ 0.0f };
        float gain = 0.0f;
        bool wanted = false, playing = false;
    };
    WaterVoice m_water[2];
    glm::vec3 m_waterHit[c_waterProbes];
    bool m_waterWet[c_waterProbes] = {};
    int m_waterProbe = 0;
    float m_waterLapTimer = 2.0f;

    float m_alarmTimer = 0.0f;
    float m_alarmCooldown = 0.0f;
    float m_alarmHealth = -1.0f; // the own team's built structures' health sum at the last sample (< 0 = none yet)
    int m_alarmCount = 0;
    float m_lastPlayerHealth = -1.0f;

    oc::vector<GameUnitComponent::SoundCue> m_cueScratch;
    float m_statTimer = 0.0f;
    int m_statCues = 0, m_statStarts = 0;
};
