#ifndef LEAF_LOOK_INC_GLSL
#define LEAF_LOOK_INC_GLSL

// THE LEAF LOOK, shared by the tree meshes' leaves (instanced_indirect_tree.fs.glsl) and the foliage cards
// (instanced_indirect_foliage.fs.glsl). Requires the UBO and hashU (shared.inc.glsl).

// The TRANSMITTED sun's colour: the leaf colour ^ "Foliage transmission saturation", shifted WARM by "Foliage
// transmission warmth" (x a yellow: red up, blue down), back at the leaf colour's own luminance - light through a leaf
// passes its pigment about twice (deeper), and the chlorophyll's red edge lets more red through than its front shows:
// a backlit leaf glows yellow-green (the squared colour alone came out a deeper GREEN).
vec3 leafTransmitColor(vec3 albedo)
{
    const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
    const vec3 deep = pow(max(albedo, vec3(1e-4)), vec3(u_foliage_transmissionSaturation))
                    * mix(vec3(1.0), vec3(1.8, 1.0, 0.45), u_foliage_transmissionWarmth);
    return deep * (dot(albedo, LUMA) / max(dot(deep, LUMA), 1e-6));
}

// A lattice corner's TWO values in [0, 1]: the low and the high 16 bits of one hash.
vec2 leafNoiseCorner(ivec3 q)
{
    const uint h = hashU(uint(q.x) * 0x8da6b343u ^ uint(q.y) * 0xd8163841u ^ uint(q.z) * 0xcb1ab31fu);
    return vec2(float(h & 0xFFFFu), float(h >> 16)) * (1.0 / 65535.0);
}

// TWO independent trilinear value noises in [0, 1] (one lattice cell = 1 unit) from ONE lattice: 8 hashes, not 16 (two
// noises on two lattices).
vec2 leafValueNoise2(vec3 p)
{
    const vec3 c = floor(p);
    const vec3 f = p - c;
    const vec3 s = f * f * (3.0 - 2.0 * f);
    const ivec3 i = ivec3(c);
    const vec2 x00 = mix(leafNoiseCorner(i),                leafNoiseCorner(i + ivec3(1, 0, 0)), s.x);
    const vec2 x10 = mix(leafNoiseCorner(i + ivec3(0, 1, 0)), leafNoiseCorner(i + ivec3(1, 1, 0)), s.x);
    const vec2 x01 = mix(leafNoiseCorner(i + ivec3(0, 0, 1)), leafNoiseCorner(i + ivec3(1, 0, 1)), s.x);
    const vec2 x11 = mix(leafNoiseCorner(i + ivec3(0, 1, 1)), leafNoiseCorner(i + ivec3(1, 1, 1)), s.x);
    return mix(mix(x00, x10, s.y), mix(x01, x11, s.y), s.z);
}

// PATCHES of leaf colour over a crown ("Leaf patch variation" / "Leaf patch size (m)"): one noise for the brightness,
// one toward a sunnier yellow-green or a deeper blue-green (the two values of leafValueNoise2) - a crown in groups of lighter and darker, yellower and
// greener leaves, not one flat colour. World space (no per-card data reaches the FS); the wind moves a leaf a few cm,
// against patches of metres.
vec3 leafPatchTint(vec3 albedo, vec3 worldPos)
{
    const float amount = u_foliage_leafPatchVariation;
    if (amount <= 0.0)
        return albedo;
    const vec2 n = leafValueNoise2(worldPos * u_foliage_invLeafPatchSize);
    const float bright = n.x - 0.5;
    const float hue = (n.y - 0.5) * 2.0 * amount;
    const vec3 tinted = hue > 0.0 ? mix(albedo, albedo * vec3(1.3, 1.08, 0.55), hue)
                                  : mix(albedo, albedo * vec3(0.75, 0.95, 0.9), -hue);
    return tinted * (1.0 + 1.2 * amount * bright);
}

#endif
