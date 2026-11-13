module;

#pragma warning(push, 3)
#pragma warning(disable: 4701 4702 4245 4244 4310)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#include <miniaudio/miniaudio.h>
#pragma warning(pop)

#include <phonon/phonon.h>

module Audio;

import Core;
import Core.glm;
import Core.Log;
import Settings;
import Settings.Tweaks;

import :Buffer;
import :Source;
import :System;
import :Types;
import :Engine;

bool AudioSystem::initialize()
{
    assert(!m_initialized);
    ProfileScope scope("AudioSystem::initialize", EProfileCategory::Audio);
    SystemState& state = audioState();

    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.channels = 2;
    engineConfig.sampleRate = SampleRate;
    // EVERY volume change glides over 20 ms. miniaudio's default is 0: a new volume is one step in the signal, and a
    // step is a click. A source whose gain is set every frame (a fade, a sound that follows the listener's distance)
    // then clicks at the frame rate: the whole mix stutters.
    engineConfig.defaultVolumeSmoothTimeInPCMFrames = SampleRate / 50;
    if (ma_engine_init(&engineConfig, &state.engine) != MA_SUCCESS)
    {
        Log::error("Audio: failed to open the default output device");
        return false;
    }

    IPLContextSettings contextSettings{};
    contextSettings.version = STEAMAUDIO_VERSION;
    contextSettings.simdLevel = IPL_SIMDLEVEL_AVX2;
    if (iplContextCreate(&contextSettings, &state.iplContext) != IPL_STATUS_SUCCESS)
    {
        Log::error("Audio: failed to create the Steam Audio context");
        ma_engine_uninit(&state.engine);
        return false;
    }
    IPLHRTFSettings hrtfSettings{};
    hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
    hrtfSettings.volume = 1.0f;
    if (iplHRTFCreate(state.iplContext, &state.iplAudioSettings, &hrtfSettings, &state.hrtf) != IPL_STATUS_SUCCESS)
    {
        Log::error("Audio: failed to create the Steam Audio HRTF");
        iplContextRelease(&state.iplContext);
        ma_engine_uninit(&state.engine);
        return false;
    }
    m_initialized = true;

    // The shared reverb (Reverb.ixx): a node with no input - the spatial sources send to it through SystemState's
    // ring - on the endpoint. update() keeps it at the "Audio/Reverb" tweaks.
    {
        const AudioSettings& audio = Globals::settings.audio;
        state.reverb.configure((float)SampleRate, audio.reverbTime, audio.reverbTone, audio.reverbSize);
        ma_node_config nodeConfig = ma_node_config_init();
        ma_uint32 outChannels = 2;
        nodeConfig.vtable = &g_reverbNodeVTable;
        nodeConfig.pOutputChannels = &outChannels;
        if (ma_node_init(ma_engine_get_node_graph(&state.engine), &nodeConfig, nullptr, &state.reverbNode.base) == MA_SUCCESS)
        {
            ma_node_attach_output_bus(&state.reverbNode.base, 0, ma_engine_get_endpoint(&state.engine), 0);
            state.reverbNodeValid = true;
        }
        else
            Log::error("Audio: failed to create the reverb node - no reverb");
    }

    const float& masterVolume = Globals::settings.audio.masterVolume;
    ma_engine_set_volume(&state.engine, masterVolume);
    Tweak::removeListeners(this); // a re-initialize after shutdown attaches once
    Tweak::onChange(masterVolume, this,
        [this]() { if (m_initialized) ma_engine_set_volume(&audioState().engine, Globals::settings.audio.masterVolume); });

    const ma_device* device = ma_engine_get_device(&state.engine);
    Log::info("Audio: miniaudio + Steam Audio HRTF on '" + oc::string(device ? device->playback.name : "unknown") + "'");
    return true;
}

void AudioSystem::shutdown()
{
    if (!m_initialized)
        return;
    SystemState& state = audioState();
    m_oneShotPool.clear(); // releases its sources from the registry
    for (oc::unique_ptr<SourceState>& source : state.sources)
        destroySourceState(*source); // sources still held by user handles; those handles no-op after this
    state.sources.clear();
    state.buffers.clear();
    if (state.reverbNodeValid)
    {
        ma_node_uninit(&state.reverbNode.base, nullptr);
        state.reverbNodeValid = false;
    }
    m_initialized = false;
    ma_engine_uninit(&state.engine);
    iplHRTFRelease(&state.hrtf);
    iplContextRelease(&state.iplContext);
}

void AudioSystem::update(const Camera& camera, const glm::vec3& listenerVelocity)
{
    ProfileScope profileScope("Audio", EProfileCategory::Audio);
    if (!m_initialized)
        return;
    const glm::mat4 camToWorld = glm::inverse(camera.viewMatrix);
    SystemState& state = audioState();
    state.listenerPos = camera.position;
    state.listenerFwd = glm::normalize(-glm::vec3(camToWorld[2]));
    state.listenerUp = glm::normalize(glm::vec3(camToWorld[1]));
    state.listenerVel = listenerVelocity;
    for (oc::unique_ptr<SourceState>& source : state.sources)
        updateSource(*source);

    // The reverb follows its tweaks ("Audio/Reverb"): the shape only when it changed, the level always.
    const AudioSettings& audio = Globals::settings.audio;
    if (audio.reverbTime != m_reverbTime || audio.reverbTone != m_reverbTone || audio.reverbSize != m_reverbSize)
    {
        m_reverbTime = audio.reverbTime;
        m_reverbTone = audio.reverbTone;
        m_reverbSize = audio.reverbSize;
        state.reverb.configure((float)SampleRate, m_reverbTime, m_reverbTone, m_reverbSize);
    }
    state.reverbLevel.store(audio.reverbLevel, oc::memory_order_relaxed);
}

AudioBuffer AudioSystem::createBuffer(EAudioFormat format, oc::span<const std::byte> pcmData, uint32 sampleRate)
{
    if (!m_initialized || pcmData.empty() || sampleRate == 0)
        return {};

    const bool stereo = format == EAudioFormat::Stereo8 || format == EAudioFormat::Stereo16 || format == EAudioFormat::StereoFloat32;
    oc::unique_ptr<SoundData> data = oc::make_unique<SoundData>();
    data->channels = stereo ? 2 : 1;
    data->sampleRate = sampleRate;

    switch (format)
    {
    case EAudioFormat::Mono8:
    case EAudioFormat::Stereo8:
    {
        const uint8* samples = reinterpret_cast<const uint8*>(pcmData.data());
        data->frames.resize(pcmData.size());
        for (size_t i = 0; i < pcmData.size(); ++i)
            data->frames[i] = (float(samples[i]) - 128.0f) / 128.0f;
        break;
    }
    case EAudioFormat::Mono16:
    case EAudioFormat::Stereo16:
    {
        const int16* samples = reinterpret_cast<const int16*>(pcmData.data());
        const size_t count = pcmData.size() / sizeof(int16);
        data->frames.resize(count);
        for (size_t i = 0; i < count; ++i)
            data->frames[i] = float(samples[i]) / 32768.0f;
        break;
    }
    case EAudioFormat::MonoFloat32:
    case EAudioFormat::StereoFloat32:
    {
        const size_t count = pcmData.size() / sizeof(float);
        data->frames.resize(count);
        memcpy(data->frames.data(), pcmData.data(), count * sizeof(float));
        break;
    }
    }
    if (data->frames.empty())
        return {};

    AudioBuffer buffer;
    buffer.m_handle = reinterpret_cast<uint64>(data.get());
    buffer.m_durationSec = float(data->frameCount()) / float(sampleRate);
    audioState().buffers.push_back(oc::move(data));
    return buffer;
}

AudioBuffer AudioSystem::loadSound(oc::string_view path, oc::span<const uint8> fileBytes)
{
    if (!m_initialized)
        return {};
    const oc::string pathStr(path);
    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0); // native channels/rate
    ma_decoder decoder;
    if (fileBytes.empty() || ma_decoder_init_memory(fileBytes.data(), fileBytes.size(), &decoderConfig, &decoder) != MA_SUCCESS)
    {
        Log::error("Audio: could not open '" + pathStr + "'");
        return {};
    }
    ma_format format = ma_format_unknown;
    ma_uint32 channels = 0, sampleRate = 0;
    ma_decoder_get_data_format(&decoder, &format, &channels, &sampleRate, nullptr, 0);
    if (channels != 1 && channels != 2)
    {
        Log::error("Audio: '" + pathStr + "' has " + oc::to_string(channels) + " channels, only mono/stereo is supported");
        ma_decoder_uninit(&decoder);
        return {};
    }

    oc::vector<float> frames;
    float chunk[4096];
    const ma_uint64 chunkFrames = 4096 / channels;
    for (;;)
    {
        ma_uint64 framesRead = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder, chunk, chunkFrames, &framesRead);
        frames.insert(frames.end(), chunk, chunk + framesRead * channels);
        if (result != MA_SUCCESS || framesRead < chunkFrames)
            break;
    }
    ma_decoder_uninit(&decoder);
    if (frames.empty())
    {
        Log::error("Audio: '" + pathStr + "' contains no audio data");
        return {};
    }
    return createBuffer(channels == 2 ? EAudioFormat::StereoFloat32 : EAudioFormat::MonoFloat32,
        oc::as_bytes(oc::span<const float>(frames)), sampleRate);
}

AudioSource AudioSystem::createSource()
{
    if (!m_initialized)
        return {};
    SystemState& state = audioState();
    oc::unique_ptr<SourceState> source = oc::make_unique<SourceState>(); // built outside the lock
    SourceState* raw = source.get();
    const std::lock_guard lock(m_sourceMutex);
    state.sources.push_back(oc::move(source));
    return AudioSource(reinterpret_cast<uint64>(raw));
}

void AudioSystem::releaseSource(uint64 sourceHandle)
{
    SystemState& state = audioState();
    SourceState* handleState = toState(sourceHandle);
    oc::unique_ptr<SourceState> owned; // taken out under the lock, torn down after it
    {
        const std::lock_guard lock(m_sourceMutex);
        for (size_t i = 0; i < state.sources.size(); ++i)
        {
            if (state.sources[i].get() == handleState)
            {
                owned = oc::move(state.sources[i]);
                state.sources.erase(state.sources.begin() + i);
                break;
            }
        }
    }
    if (owned)
        destroySourceState(*owned);
}

void AudioSystem::detachBuffer(uint64 bufferHandle)
{
    const std::lock_guard lock(m_sourceMutex);
    SystemState& state = audioState();
    const SoundData* data = reinterpret_cast<const SoundData*>(bufferHandle);
    for (oc::unique_ptr<SourceState>& source : state.sources)
        if (source->data == data)
            clearSourceSound(*source);
    oc::erase_if(state.buffers, [&](const oc::unique_ptr<SoundData>& b) { return b.get() == data; });
}

AudioSource& AudioSystem::acquireOneShotSource()
{
    for (AudioSource& source : m_oneShotPool)
        if (!source.isPlaying())
            return source;
    if (m_oneShotPool.size() < MaxOneShotSources)
        return m_oneShotPool.emplace_back(createSource());
    return m_oneShotPool[m_oneShotSteal++ % MaxOneShotSources]; // all busy: steal round-robin
}

void AudioSystem::playOneShot(const AudioBuffer& buffer, const glm::vec3& position, float gain, float pitch)
{
    if (!m_initialized || !buffer.isValid())
        return;
    AudioSource& source = acquireOneShotSource();
    if (!source.isValid())
        return;
    SourceState& s = *toState(source.m_handle);
    s.relative = false;
    s.position = position;
    s.gain = gain;
    s.pitch = pitch;
    s.looping = false;
    source.setBuffer(buffer);
    source.play();
}

void AudioSystem::playOneShot2D(const AudioBuffer& buffer, float gain, float pitch)
{
    if (!m_initialized || !buffer.isValid())
        return;
    AudioSource& source = acquireOneShotSource();
    if (!source.isValid())
        return;
    SourceState& s = *toState(source.m_handle);
    s.relative = true;
    s.gain = gain;
    s.pitch = pitch;
    s.looping = false;
    source.setBuffer(buffer);
    source.play();
}
