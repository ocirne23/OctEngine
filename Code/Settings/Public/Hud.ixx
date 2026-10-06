export module Settings.Hud;

import Core;

// "HUD": the in-game HUD overlay (UI's GameHudOverlay).
export struct HudSettings
{
    bool enabled = true;
    float scale = 1.0f;
    float opacity = 0.9f;
    float hotbarSlotSize = 72.0f; // px at scale 1 (the grid is the main build UI - big enough to read)
    float hotbarTextScale = 1.9f; // slot caption size, x the base font
};

export namespace Settings
{
    void registerHud(HudSettings& s);
}
