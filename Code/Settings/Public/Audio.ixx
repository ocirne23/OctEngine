export module Settings.Audio;

import Core;

// "Audio/System": the AudioSystem pushes the volume into miniaudio on a change.
export struct AudioSettings
{
    float masterVolume = 1.0f;
};

export namespace Settings
{
    void registerAudio(AudioSettings& s);
}
