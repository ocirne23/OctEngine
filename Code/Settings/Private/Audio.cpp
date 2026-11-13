module Settings.Audio;

import Core;
import Settings.Tweaks;

void Settings::registerAudio(AudioSettings& s)
{
    Tweak::floatVar("Audio/System", "Master Volume", &s.masterVolume, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Audio/Reverb", "Level", &s.reverbLevel, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Audio/Reverb", "Time (s)", &s.reverbTime, 0.2f, 8.0f, 0.05f);
    Tweak::floatVar("Audio/Reverb", "Tone (Hz)", &s.reverbTone, 500.0f, 16000.0f, 50.0f);
    Tweak::floatVar("Audio/Reverb", "Size", &s.reverbSize, 0.5f, 1.5f, 0.01f);
}
