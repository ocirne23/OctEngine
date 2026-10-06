module Settings.ParticleSystem;

import Core;
import Settings.Tweaks;

void Settings::registerParticleSystem(ParticleSystemSettings& s)
{
    // The registry keeps VIEWS of tweak names, so every name is a literal.
    struct Builtin
    {
        ParticleBuiltinSettings* settings;
        const char* name;
        const char* countName;
        const char* sizeName;
        const char* alphaName;
        const char* sizeVarName;
    };
    const Builtin builtins[] = {
        { &s.rain, "Rain", "Rain count", "Rain size", "Rain alpha", "Rain size variation" },
        { &s.snow, "Snow", "Snow count", "Snow size", "Snow alpha", "Snow size variation" },
        { &s.dust, "Dust", "Dust count", "Dust size", "Dust alpha", "Dust size variation" },
        { &s.underwater, "Underwater", "Underwater count", "Underwater size", "Underwater alpha", "Underwater size variation" },
    };
    for (const Builtin& b : builtins)
    {
        Tweak::boolean("Particles", b.name, &b.settings->enabled);
        Tweak::floatVar("Particles", b.countName, &b.settings->countScale, 0.0f, 4.0f, 0.01f);
        Tweak::floatVar("Particles", b.sizeName, &b.settings->sizeScale, 0.1f, 4.0f, 0.01f);
        Tweak::floatVar("Particles", b.alphaName, &b.settings->alphaScale, 0.0f, 4.0f, 0.01f);
        Tweak::floatVar("Particles", b.sizeVarName, &b.settings->sizeVarScale, 0.0f, 4.0f, 0.01f);
    }
    Tweak::boolean("Particles", "Ocean spray", &s.oceanSpray);
}
