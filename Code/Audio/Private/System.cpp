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
import Core.Tweaks;

import :Buffer;
import :Source;
import :System;
import :Types;
import :Engine;

bool AudioSystem::initialize()
{
    assert(!m_initialized);
    SystemState& state = audioState();

    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.channels = 2;
    engineConfig.sampleRate = SampleRate;
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

    ma_engine_set_volume(&state.engine, m_masterVolume);
    Tweak::floatVar("Audio/System", "Master Volume", &m_masterVolume, 0.0f, 2.0f, 0.01f,
        [this]() { if (m_initialized) ma_engine_set_volume(&audioState().engine, m_masterVolume); });

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

AudioBuffer AudioSystem::loadSound(oc::string_view path)
{
    if (!m_initialized)
        return {};
    const oc::string pathStr(path);
    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0); // native channels/rate
    ma_decoder decoder;
    if (ma_decoder_init_file(pathStr.c_str(), &decoderConfig, &decoder) != MA_SUCCESS)
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
        std::as_bytes(oc::span(frames)), sampleRate);
}

AudioSource AudioSystem::createSource()
{
    if (!m_initialized)
        return {};
    SystemState& state = audioState();
    state.sources.push_back(oc::make_unique<SourceState>());
    return AudioSource(reinterpret_cast<uint64>(state.sources.back().get()));
}

void AudioSystem::releaseSource(uint64 sourceHandle)
{
    SystemState& state = audioState();
    SourceState* handleState = toState(sourceHandle);
    for (size_t i = 0; i < state.sources.size(); ++i)
    {
        if (state.sources[i].get() == handleState)
        {
            destroySourceState(*handleState);
            state.sources.erase(state.sources.begin() + i);
            return;
        }
    }
}

void AudioSystem::detachBuffer(uint64 bufferHandle)
{
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
