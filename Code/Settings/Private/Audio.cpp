module Settings.Audio;

import Core;
import Settings.Tweaks;

void Settings::registerAudio(AudioSettings& s)
{
    Tweak::floatVar("Audio/System", "Master Volume", &s.masterVolume, 0.0f, 2.0f, 0.01f);
}
