module Physics;

import Core;
import Core.Log;

import :Layers;

// Layer name -> category bit registry; index in the vector = bit index. Session-local: bits are only
// compared against each other at runtime, so allocation order between runs doesn't matter.
// Namespace scope, not a function-local static: the build is /Zc:threadSafeInit-, so a local static
// first reached from two threads at once is a race, and bit() is called both from spawn on main and
// from the terrain collider's tile-build jobs. Nothing else runs during static init, so the vector
// is simply up before the first bit() call.
static oc::vector<oc::string> g_layerNames = { "Default" };

static oc::vector<oc::string>& layerNames() { return g_layerNames; }

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
