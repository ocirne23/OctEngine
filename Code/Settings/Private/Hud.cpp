module Settings.Hud;

import Core;
import Settings.Tweaks;

void Settings::registerHud(HudSettings& s)
{
    Tweak::boolean("HUD", "Enabled", &s.enabled);
    Tweak::floatVar("HUD", "Scale", &s.scale, 0.5f, 3.0f, 0.05f);
    Tweak::floatVar("HUD", "Opacity", &s.opacity, 0.1f, 1.0f, 0.05f);
    Tweak::floatVar("HUD", "Hotbar slot size", &s.hotbarSlotSize, 24.0f, 160.0f, 1.0f);
    Tweak::floatVar("HUD", "Hotbar text scale", &s.hotbarTextScale, 0.5f, 4.0f, 0.05f);
}
