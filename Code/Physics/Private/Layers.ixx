export module Physics:Layers;

import Core;

// Named collision layers. A name maps to one of 64 category bits, allocated on first use;
// "Default" is bit 0 (the categoryBits/maskBits defaults below). Two shapes collide when each one's
// mask contains the other's category (unless a non-zero matching groupIndex overrides: negative =
// never collide, positive = always).
export namespace PhysicsLayers
{
    uint64 bit(oc::string_view name);
    constexpr uint64 All = ~0ull;
}
