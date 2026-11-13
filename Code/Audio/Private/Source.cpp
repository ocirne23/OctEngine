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

import :Buffer;
import :Source;
import :System;
import :Engine;

void AudioSource::destroy()
{
    if (m_handle == 0)
        return;
    if (Globals::audio.isInitialized())
        Globals::audio.releaseSource(m_handle);
    m_handle = 0;
}

void AudioSource::setBuffer(const AudioBuffer& buffer)
{
    SourceState& s = *toState(m_handle);
    clearSourceSound(s);
    const SoundData* data = reinterpret_cast<const SoundData*>(buffer.m_handle);
    if (!data || data->frames.empty())
        return;

    SystemState& state = audioState();

    ma_audio_buffer_config config = ma_audio_buffer_config_init(ma_format_f32, data->channels, data->frameCount(), data->frames.data(), nullptr);
    config.sampleRate = data->sampleRate;
    if (ma_audio_buffer_init(&config, &s.buffer) != MA_SUCCESS)
        return;
    if (ma_sound_init_from_data_source(&state.engine, &s.buffer, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &s.sound) != MA_SUCCESS)
    {
        ma_audio_buffer_uninit(&s.buffer);
        return;
    }
    s.soundValid = true;
    s.data = data;
    s.spatial = data->channels == 1;

    if (s.spatial)
    {
        if (!s.nodeValid)
        {
            IPLBinauralEffectSettings effectSettings{ state.hrtf };
            if (iplBinauralEffectCreate(state.iplContext, &state.iplAudioSettings, &effectSettings, &s.node.effect) == IPL_STATUS_SUCCESS)
            {
                ma_node_config nodeConfig = ma_node_config_init();
                ma_uint32 inChannels = 2, outChannels = 2;
                nodeConfig.vtable = &g_binauralNodeVTable;
                nodeConfig.pInputChannels = &inChannels;
                nodeConfig.pOutputChannels = &outChannels;
                if (ma_node_init(ma_engine_get_node_graph(&state.engine), &nodeConfig, nullptr, &s.node.base) == MA_SUCCESS)
                {
                    ma_node_attach_output_bus(&s.node.base, 0, ma_engine_get_endpoint(&state.engine), 0);
                    s.nodeValid = true;
                }
                else
                {
                    iplBinauralEffectRelease(&s.node.effect);
                }
            }
        }
        if (s.nodeValid)
        {
            iplBinauralEffectReset(s.node.effect);
            s.node.flush.store(true, oc::memory_order_relaxed);
            ma_node_attach_output_bus(&s.sound, 0, &s.node.base, 0);
        }
        else
        {
            s.spatial = false; // HRTF unavailable: fall back to plain playback
        }
    }
    ma_sound_set_looping(&s.sound, s.looping ? MA_TRUE : MA_FALSE);
    updateSource(s);
}

void AudioSource::play()
{
    SourceState& s = *toState(m_handle);
    if (!s.soundValid)
        return;
    if (ma_sound_is_playing(&s.sound))
        ma_sound_seek_to_pcm_frame(&s.sound, 0);
    ma_sound_start(&s.sound);
}

void AudioSource::pause()
{
    SourceState& s = *toState(m_handle);
    if (s.soundValid)
        ma_sound_stop(&s.sound);
}

void AudioSource::stop()
{
    SourceState& s = *toState(m_handle);
    if (!s.soundValid)
        return;
    ma_sound_stop(&s.sound);
    ma_sound_seek_to_pcm_frame(&s.sound, 0);
}

bool AudioSource::isPlaying() const
{
    if (m_handle == 0)
        return false;
    SourceState& s = *toState(m_handle);
    return s.soundValid && ma_sound_is_playing(&s.sound) == MA_TRUE;
}

void AudioSource::setLooping(bool loop)
{
    SourceState& s = *toState(m_handle);
    s.looping = loop;
    if (s.soundValid)
        ma_sound_set_looping(&s.sound, loop ? MA_TRUE : MA_FALSE);
}

void AudioSource::setGain(float gain)
{
    SourceState& s = *toState(m_handle);
    s.gain = gain;
    updateSource(s);
}

void AudioSource::setPitch(float pitch)
{
    SourceState& s = *toState(m_handle);
    s.pitch = pitch;
    updateSource(s);
}

void AudioSource::setPosition(const glm::vec3& position)
{
    SourceState& s = *toState(m_handle);
    s.position = position;
    updateSource(s);
}

void AudioSource::setVelocity(const glm::vec3& velocity)
{
    SourceState& s = *toState(m_handle);
    s.velocity = velocity;
    updateSource(s);
}

void AudioSource::setRelative(bool relative)
{
    SourceState& s = *toState(m_handle);
    s.relative = relative;
    updateSource(s);
}

void AudioSource::setReverbSend(float send)
{
    toState(m_handle)->node.send.store(glm::max(send, 0.0f), oc::memory_order_relaxed);
}

void AudioSource::setLowpass(float hz)
{
    const float coefficient = hz >= 20000.0f ? 1.0f : 1.0f - std::exp(-6.2831853f * glm::max(hz, 20.0f) / float(SampleRate));
    toState(m_handle)->node.lowpass.store(coefficient, oc::memory_order_relaxed);
}

void AudioSource::setAttenuation(float referenceDistance, float maxDistance, float rolloff)
{
    SourceState& s = *toState(m_handle);
    s.refDist = glm::max(referenceDistance, 0.001f);
    s.maxDist = maxDistance;
    s.rolloff = rolloff;
    updateSource(s);
}
