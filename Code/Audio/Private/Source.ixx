export module Audio:Source;

import Core;
import Core.glm;

import :Buffer;

// RAII handle to a playable sound instance. Mono sounds are spatialized through the Steam Audio
// HRTF (position set via setPosition); call setRelative(true) for 2D/UI playback instead.
export class AudioSource final
{
public:

    AudioSource() = default;
    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;
    AudioSource(AudioSource&& other) noexcept : m_handle(other.m_handle) { other.m_handle = 0; }
    AudioSource& operator=(AudioSource&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            m_handle = other.m_handle;
            other.m_handle = 0;
        }
        return *this;
    }
    ~AudioSource() { destroy(); }

    bool isValid() const { return m_handle != 0; }
    void destroy();

    void setBuffer(const AudioBuffer& buffer); // stops the source if it was playing another buffer

    void play(); // restarts from the beginning if already playing, resumes if paused
    void pause();
    void stop();
    bool isPlaying() const;

    void setLooping(bool loop);
    void setGain(float gain);   // linear, 1 = as recorded
    void setPitch(float pitch); // playback rate multiplier, 1 = as recorded
    void setPosition(const glm::vec3& position);
    void setVelocity(const glm::vec3& velocity); // for doppler only, position is not integrated
    void setRelative(bool relative);             // listener-relative: no HRTF/attenuation (2D/UI sounds)

    // Inverse-clamped distance attenuation: full volume inside referenceDistance, no further
    // attenuation beyond maxDistance, rolloff scales how fast gain drops in between.
    void setAttenuation(float referenceDistance, float maxDistance, float rolloff = 1.0f);

    // THE DISTANCE, beyond the gain (mono sources only: they are the ones with a place).
    // setReverbSend: how much of the sound goes to the shared reverb ("Audio/Reverb"), on top of its direct sound; 0 =
    // none (the default). A far sound gets more: it is mostly its reverb.
    // setLowpass: the highs the air took on the way; 20000 or more = open (the default).
    void setReverbSend(float send);
    void setLowpass(float hz);

private:

    friend class AudioSystem;
    explicit AudioSource(uint64 handle) : m_handle(handle) {}

    uint64 m_handle = 0; // SourceState*, 0 = invalid
};
