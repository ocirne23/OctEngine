export module Audio:System;

import Core;
import Core.glm;
import Core.Camera;

import :Buffer;
import :Source;
import :Types;

export class AudioSystem final
{
public:

    ~AudioSystem() { shutdown(); }

    bool initialize(); // opens the default output device (48kHz stereo) and creates the Steam Audio HRTF
    void shutdown();

    // Call once per frame with the active camera: places the listener and refreshes every source's
    // HRTF direction, distance attenuation and doppler. Master volume is a Tweak under Audio/System.
	void update(const Camera& camera, const glm::vec3& listenerVelocity = glm::vec3(0.0f));

    AudioBuffer createBuffer(EAudioFormat format, oc::span<const std::byte> pcmData, uint32 sampleRate);
    AudioBuffer loadSound(oc::string_view path); // WAV/FLAC/MP3, relative to Assets/

    AudioSource createSource();

    // Fire-and-forget playback from an internal source pool (steals the oldest slot when full).
    void playOneShot(const AudioBuffer& buffer, const glm::vec3& position, float gain = 1.0f, float pitch = 1.0f);
    void playOneShot2D(const AudioBuffer& buffer, float gain = 1.0f, float pitch = 1.0f);

    bool isInitialized() const { return m_initialized; }

private:

    friend class AudioBuffer;
    friend class AudioSource;

    AudioSource& acquireOneShotSource();
    void detachBuffer(uint64 bufferHandle);  // a buffer is being destroyed: stop + detach sources playing it
    void releaseSource(uint64 sourceHandle);

    bool m_initialized = false;
    float m_masterVolume = 1.0f;
    uint32 m_oneShotSteal = 0;
    oc::vector<AudioSource> m_oneShotPool;
};

export namespace Globals
{
    AudioSystem audio;
}
