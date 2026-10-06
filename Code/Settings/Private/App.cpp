module Settings.App;

import Core;
import Settings.Tweaks;

void Settings::registerAppControls(AppControlsSettings& s)
{
    Tweak::floatVar("Force/Emitter", "Output", &s.output, 0.2f, 10.0f, 0.01f); // stay above iso (0.15)
    Tweak::floatVar("Force/Emitter", "Reach", &s.reach, 0.1f, 100.0f, 0.1f);
    Tweak::floatVar("Force/Emitter", "Focus", &s.focus, 0.0f, 1.0f, 0.01f);
    Tweak::intVar("Force/Emitter", "Team", &s.team, 0, 7, 1);
    Tweak::floatVar("Force/Emitter", "Distribution", &s.distribution, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Force/Emitter", "Width", &s.width, 0.05f, 2.0f, 0.01f);

    Tweak::floatVar("Network/Player", "Move speed", &s.playerMoveSpeed, 0.5f, 30.0f, 0.1f);
    Tweak::floatVar("Network/Player", "Accel", &s.playerAccel, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Network/Player", "Jump speed", &s.playerJumpSpeed, 0.5f, 20.0f, 0.1f);
}
