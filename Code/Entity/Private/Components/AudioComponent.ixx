export module Entity:AudioComponent;

import :Entity;
import Core;
import Core.glm;
import Core.Transform;
import File;
import Audio;

// How a Sound alias holding several clips picks which one to play on each trigger.
export enum class EAudioSelect : uint8
{
    Single,           // always play clips[0] (a one-clip sound; the default)
    Random,           // a uniformly random clip each trigger (may repeat)
    RandomNoRepeat,   // uniformly random, but never the same clip twice in a row
    Cycle,            // step through the clips in order, wrapping around
    CycleStartRandom, // Cycle but starting index is randomized
};

export const char* audioSelectToken(EAudioSelect select);
export EAudioSelect audioSelectFromToken(oc::string_view token);

// A set of named, triggerable sounds ("Component Audio" in .pre files): each Sound entry pairs an
// alias with one or more sound-file clips (each with its own playback settings) and a Select mode that
// picks a clip per trigger. Gameplay (or the script "Trigger Audio" node, via ctx->entityTriggerAudio)
// plays one by alias; playing spatial sounds follow the entity unless the trigger supplied a position.
export struct AudioComponent
{
    static constexpr EComponentID getId() { return EComponentID_Audio; }

    // One playable file behind a Sound alias, with its own settings. A `Path` line in the .pre, whose
    // child lines (Volume/Pitch/...) are these fields.
    struct Clip
    {
        oc::string path;                         // sound file, relative to Assets/ (WAV/FLAC/MP3)
        oc::shared_ptr<AudioBuffer> buffer;      // World-cached, shared between entities using the same file
        float volume = 1.0f;
        float pitch = 1.0f;
        bool loop = false;
        bool relative = false;                    // 2D playback (no spatialization/attenuation)
        float referenceDistance = 1.0f;           // inverse-clamped attenuation (see AudioSource::setAttenuation)
        float maxDistance = FLT_MAX;
        float rolloff = 1.0f;
    };

    struct SoundDesc
    {
        oc::string alias;                        // the name gameplay/scripts trigger it by
        EAudioSelect select = EAudioSelect::Single;
        oc::vector<Clip> clips;
    };

    struct SpawnInfo
    {
        oc::vector<SoundDesc> sounds;
    };

    // Per-trigger overrides for the authored settings; unset fields keep the selected clip's values. A
    // set position pins the sound at that world position instead of following the entity.
    struct TriggerOverrides
    {
        oc::optional<glm::vec3> position;
        oc::optional<float> volume;
        oc::optional<float> pitch;
    };

    struct Voice // playback state per SoundDesc (parallel to info->sounds)
    {
        AudioSource source;   // created lazily on first trigger
        int currentClip = -1; // clip index currently loaded into source (-1 = none)
        int lastClip = -1;    // last clip played, for RandomNoRepeat
        int cycleNext = -1;   // next clip index for Cycle
        bool follow = true;   // track the entity's world position while playing
    };

    const SpawnInfo* info = nullptr;
    oc::vector<Voice> voices;

    bool trigger(Entity& entity, oc::string_view alias, const TriggerOverrides& overrides = {});
    void stopSound(oc::string_view alias); // empty alias = stop every sound
    int findSound(oc::string_view alias) const;
    oc::span<const SoundDesc> getSounds() const { return info ? oc::span<const SoundDesc>(info->sounds) : oc::span<const SoundDesc>(); }

    void spawn(Entity& entity, const SpawnInfo& info, const Transform& base);
    void destroy(Entity& entity, const SpawnInfo& info);
    void update(Entity& entity, const Transform& world); // playing follow-sounds track the entity

private:
    int selectClip(const SoundDesc& sound, Voice& voice) const;
};

export const AudioComponent::SpawnInfo* getAudioSpawnInfo(const Entity* entity);

// Serializes an audio spawn recipe into a "Component Audio" node.
export void writeAudioSpawnInfo(const AudioComponent::SpawnInfo& info, AssetNode& out);
