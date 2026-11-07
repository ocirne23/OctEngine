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

    Tweak::floatVar("Player/Locomotion", "Walk clip speed", &s.walkClipSpeed, 0.2f, 5.0f, 0.05f);
    Tweak::floatVar("Player/Locomotion", "Run clip speed", &s.runClipSpeed, 0.5f, 15.0f, 0.05f);
    Tweak::floatVar("Player/Locomotion", "Body turn rate", &s.bodyTurnRate, 1.0f, 100.0f, 0.5f);
    Tweak::floatVar("Player/Locomotion", "Leg turn speed", &s.legTurnSpeed, 30.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Player/Locomotion", "Max hip twist", &s.maxHipTwist, 0.0f, 150.0f, 1.0f);
    Tweak::floatVar("Player/Locomotion", "Idle turn angle", &s.idleTurnAngle, 0.0f, 150.0f, 1.0f);
    Tweak::floatVar("Player/Locomotion", "Lean per accel", &s.leanPerAccel, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Player/Locomotion", "Max lean", &s.maxLean, 0.0f, 45.0f, 0.5f);
}
