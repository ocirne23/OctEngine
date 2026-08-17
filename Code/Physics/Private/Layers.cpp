module Physics;

import Core;
import Core.Log;

import :Layers;

// Layer name -> category bit registry; index in the vector = bit index. Session-local: bits are only
// compared against each other at runtime, so allocation order between runs doesn't matter.
static oc::vector<oc::string>& layerNames()
{
    static oc::vector<oc::string> names = { "Default" };
    return names;
}

uint64 PhysicsLayers::bit(oc::string_view name)
{
    oc::vector<oc::string>& names = layerNames();
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name)
            return 1ull << i;
    if (names.size() >= 64)
    {
        Log::error("Physics: out of collision layer bits, '" + oc::string(name) + "' falls back to Default");
        return 1ull;
    }
    names.emplace_back(name);
    return 1ull << (names.size() - 1);
}
