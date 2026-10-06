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
};

export namespace Settings
{
    void registerAppControls(AppControlsSettings& s);
}
