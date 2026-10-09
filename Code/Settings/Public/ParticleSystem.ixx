export module Settings.ParticleSystem;

import Core;

// One built-in ambient effect (a camera-following volume .pfx): the "Particles/<Name>" toggle and the
// multipliers on top of the .pfx.
export struct ParticleBuiltinSettings
{
    bool enabled = false;
    float countScale = 1.0f;   // fill count + rate
    float sizeScale = 1.0f;
    float alphaScale = 1.0f;
    float sizeVarScale = 1.0f; // multiplier on the .pfx SizeVariance (the product clamps to 1)
};

// "Particles": ParticleSystem's built-in effects.
export struct ParticleSystemSettings
{
    ParticleBuiltinSettings rain{ false, 1.0f, 1.0f, 0.5f };
    ParticleBuiltinSettings snow{ false, 1.0f, 1.0f, 0.5f };
    ParticleBuiltinSettings dust{ true, 1.0f, 0.5f, 0.2f, 2.0f };
    ParticleBuiltinSettings underwater{ true, 2.0f, 1.0f, 0.05f };
    bool oceanSpray = true; // one Effects/ocean_spray.pfx instance the renderer's spray producer spawns into
    bool riverMist = true;  // one Effects/river_mist.pfx instance the renderer's river mist producer spawns into
};

export namespace Settings
{
    void registerParticleSystem(ParticleSystemSettings& s);
}
