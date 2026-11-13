export module Settings.Audio;

import Core;

// "Audio/System": the AudioSystem pushes the volume into miniaudio on a change.
export struct AudioSettings
{
    float masterVolume = 0.2f;
    // "Audio/Reverb": the one shared reverb every spatial source sends to (AudioSource::setReverbSend).
    float reverbLevel = 0.6f;   // its volume (0 = off)
    float reverbTime = 1.8f;    // s: the tail's nominal time (the tone filter makes the heard tail about half of it)
    float reverbTone = 4500.0f; // Hz: above this the tail dies early
    float reverbSize = 1.0f;    // 0.5 a small yard .. 1.5 a wide valley (how far apart the echoes are)
};

export namespace Settings
{
    void registerAudio(AudioSettings& s);
}
