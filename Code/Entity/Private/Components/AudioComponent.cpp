module Entity;

import Core;
import Core.glm;
import Core.Log;
import Core.Transform;
import :Entity;
import Audio;

void AudioComponent::spawn(Entity& entity, const SpawnInfo& spawnInfo, const Transform& base)
{
    info = &spawnInfo;
    voices.resize(spawnInfo.sounds.size());
}

void AudioComponent::destroy(Entity& entity, const SpawnInfo&)
{
    voices.clear(); // AudioSource RAII stops + releases the playing sounds
}

int AudioComponent::findSound(oc::string_view alias) const
{
    if (!info)
        return -1;
    for (int i = 0; i < (int)info->sounds.size(); ++i)
        if (info->sounds[i].alias == alias)
            return i;
    return -1;
}

const char* audioSelectToken(EAudioSelect select)
{
    switch (select)
    {
    case EAudioSelect::Random:           return "Random";
    case EAudioSelect::RandomNoRepeat:   return "RandomNoRepeat";
    case EAudioSelect::Cycle:            return "Cycle";
    case EAudioSelect::CycleStartRandom: return "CycleStartRandom";
    case EAudioSelect::Single:
    default:                             return "Single";
    }
}

EAudioSelect audioSelectFromToken(oc::string_view token)
{
    if (token == "Random")           return EAudioSelect::Random;
    if (token == "RandomNoRepeat")   return EAudioSelect::RandomNoRepeat;
    if (token == "Cycle")            return EAudioSelect::Cycle;
    if (token == "CycleStartRandom") return EAudioSelect::CycleStartRandom;
    return EAudioSelect::Single;
}

// The entity's world position, composed on demand (updateTree computes world transforms transiently).
static glm::vec3 worldPositionOf(const Entity& entity)
{
    Transform world(entity.pos, entity.scale, entity.rot);
    for (const Entity* p = entity.parent; p; p = p->parent)
        world = composeTransform(Transform(p->pos, p->scale, p->rot), world);
    return world.pos;
}

// Picks the clip to play for this trigger by the sound's Select mode, advancing the voice's selection
// state. RandomNoRepeat draws uniformly from the clips other than the last one played.
int AudioComponent::selectClip(const SoundDesc& sound, Voice& voice) const
{
    const int count = (int)sound.clips.size();
    if (count <= 1)
        return 0;
    switch (sound.select)
    {
    case EAudioSelect::Cycle:
    {
        if (voice.cycleNext == -1)
            voice.cycleNext = 0;
        const int idx = int(voice.cycleNext % uint32(count));
        voice.cycleNext = (voice.cycleNext + 1) % uint32(count);
        return idx;
    }
    case EAudioSelect::CycleStartRandom:
    {
        if (voice.cycleNext == -1)
            voice.cycleNext = glm::min(int(glm::linearRand(0.0f, float(count))), count - 1);
        const int idx = int(voice.cycleNext % uint32(count));
        voice.cycleNext = (voice.cycleNext + 1) % uint32(count);
        return idx;
    }
    case EAudioSelect::RandomNoRepeat:
    {
        if (voice.lastClip < 0)
            return glm::min(int(glm::linearRand(0.0f, float(count))), count - 1);
        int idx = glm::min(int(glm::linearRand(0.0f, float(count - 1))), count - 2); // pick among the other clips
        if (idx >= voice.lastClip)
            ++idx;
        return idx;
    }
    case EAudioSelect::Random:
    default:
        return glm::min(int(glm::linearRand(0.0f, float(count))), count - 1);
    }
}

bool AudioComponent::trigger(Entity& entity, oc::string_view alias, const TriggerOverrides& overrides)
{
    const int idx = findSound(alias);
    if (idx < 0)
    {
        Log::warning(oc::string("Audio: entity '") + entity.getName() + "' has no sound named '" + oc::string(alias) + "'");
        return false;
    }
    const SoundDesc& sound = info->sounds[idx];
    if (sound.clips.empty())
        return false;
    Voice& voice = voices[idx];
    const int clipIdx = selectClip(sound, voice);
    const Clip& clip = sound.clips[clipIdx];
    if (!clip.buffer || !clip.buffer->isValid())
        return false; // load failure already logged by World
    if (!voice.source.isValid())
    {
        voice.source = Globals::audio.createSource();
        if (!voice.source.isValid())
            return false;
    }
    if (voice.currentClip != clipIdx) // reselect the buffer only when the chosen clip changes
    {
        voice.source.setBuffer(*clip.buffer);
        voice.currentClip = clipIdx;
    }
    voice.source.setLooping(clip.loop);
    voice.source.setGain(overrides.volume.value_or(clip.volume));
    voice.source.setPitch(overrides.pitch.value_or(clip.pitch));
    voice.source.setRelative(clip.relative);
    voice.source.setAttenuation(clip.referenceDistance, clip.maxDistance, clip.rolloff);
    voice.follow = !overrides.position.has_value();
    if (voice.follow)
        voice.source.setPosition(worldPositionOf(entity));
    else
        voice.source.setPosition(overrides.position.value());
    voice.source.play();
    voice.lastClip = clipIdx;
    return true;
}

void AudioComponent::stopSound(oc::string_view alias)
{
    for (int i = 0; i < (int)voices.size(); ++i)
        if ((alias.empty() || (info && info->sounds[i].alias == alias)) && voices[i].source.isValid())
            voices[i].source.stop();
}

void AudioComponent::update(Entity& entity, const Transform& world)
{
    for (Voice& voice : voices)
        if (voice.follow && voice.source.isValid() && voice.source.isPlaying())
            voice.source.setPosition(world.pos);
}

const AudioComponent::SpawnInfo* getAudioSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<AudioComponent>(entity))
        return nullptr;

    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_Audio); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const AudioComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

void writeAudioSpawnInfo(const AudioComponent::SpawnInfo& info, AssetNode& out)
{
    const AudioComponent::Clip defaults;
    for (const AudioComponent::SoundDesc& sound : info.sounds)
    {
        AssetNode& soundNode = out.addChild("Sound");
        soundNode.values.emplace_back(sound.alias);
        if (sound.select != EAudioSelect::Single)
            soundNode.set("Select", audioSelectToken(sound.select));
        for (const AudioComponent::Clip& clip : sound.clips)
        {
            AssetNode& pathNode = soundNode.addChild("Path");
            pathNode.values.emplace_back(clip.path);
            if (clip.volume != defaults.volume)     pathNode.set("Volume", clip.volume);
            if (clip.pitch != defaults.pitch)       pathNode.set("Pitch", clip.pitch);
            if (clip.loop != defaults.loop)         pathNode.set("Loop", clip.loop);
            if (clip.relative != defaults.relative) pathNode.set("Relative", clip.relative);
            if (clip.referenceDistance != defaults.referenceDistance) pathNode.set("ReferenceDistance", clip.referenceDistance);
            if (clip.maxDistance != defaults.maxDistance)             pathNode.set("MaxDistance", clip.maxDistance);
            if (clip.rolloff != defaults.rolloff)                     pathNode.set("Rolloff", clip.rolloff);
        }
    }
}
