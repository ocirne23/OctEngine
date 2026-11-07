export module Settings.App;

import Core;

// The testbed controls (App's InputControls): the key N/M test force emitter and the key C player.
export struct AppControlsSettings
{
    // "Force/Emitter"
    float output = 1.0f;          // must exceed the iso threshold (default 0.15) or no bubble exists
    float reach = 4.0f;           // TOTAL extent: the bubble spans pos .. pos + dir * reach
    float focus = 0.5f;           // shape pinch: 0.5 = sphere spanning the line, 0 = cone pointed at
                                  // the emitter, 1 = cone pointed at the target
    int team = 0;
    float distribution = 0.5f;    // where the output density sits along the line (0 = emitter end,
                                  // 1 = target end); budget-conserving bump
    float width = 1.0f;           // lateral scale (reach untouched): 1 = round, < 1 = narrower/sharper
    // "Network/Player"
    float playerMoveSpeed = 8.0f; // m/s horizontal target (keep under Network/Validation "Max speed")
    float playerAccel = 60.0f;    // m/s^2 velocity steering
    float playerJumpSpeed = 6.0f;
    // "Player/Locomotion" (the local key C capsule: camera-locked strafe, see Game:PlayerLocomotion)
    float walkClipSpeed = 1.6f;   // m/s the walk clip covers at playback rate 1 (stride matching)
    float runClipSpeed = 4.5f;    // m/s the run clip covers at playback rate 1
    float bodyTurnRate = 20.0f;   // 1/s: how fast the body yaw follows the camera yaw
    float legTurnSpeed = 450.0f;  // deg/s: how fast the hips turn toward the move direction
    float maxHipTwist = 110.0f;   // deg: the hips never turn further from the body yaw than this
    float idleTurnAngle = 60.0f;  // deg: standing still, the feet stay planted until the camera turns this far
    float leanPerAccel = 1.2f;    // deg of upper-body lean per m/s^2 of horizontal acceleration
    float maxLean = 12.0f;        // deg
};

export namespace Settings
{
    void registerAppControls(AppControlsSettings& s);
}
