#ifndef LEAF_LOOK_INC_GLSL
#define LEAF_LOOK_INC_GLSL

// THE LEAF LOOK, shared by the tree meshes' leaves (instanced_indirect_tree.fs.glsl) and the voxel tier
// (instanced_indirect_tree_voxel.fs.glsl). Requires the UBO and hashU (shared.inc.glsl).

// A lattice corner's TWO values in [0, 65535]: the low and the high 16 bits of one hash of its key.
vec2 leafNoiseCorner(uint key)
{
    const uint h = hashU(key);
    return vec2(float(h & 0xFFFFu), float(h >> 16));
}

// TWO independent trilinear value noises in [0, 1] (one lattice cell = 1 unit) from ONE lattice: 8 hashes, not 16 (two
// noises on two lattices). A corner's key = x * A ^ y * B ^ z * C: the cell's own three products, and + A / B / C for the
// corner one step on (3 multiplies for the 8 corners).
vec2 leafValueNoise2(vec3 p)
{
    const vec3 c = floor(p);
    const vec3 f = p - c;
    const vec3 s = f * f * (3.0 - 2.0 * f);
    const uvec3 STEP = uvec3(0x8da6b343u, 0xd8163841u, 0xcb1ab31fu);
    const uvec3 k0 = uvec3(ivec3(c)) * STEP;
    const uvec3 k1 = k0 + STEP;
    const uvec4 xy = uvec4(k0.x ^ k0.y, k1.x ^ k0.y, k0.x ^ k1.y, k1.x ^ k1.y);
    const vec2 x00 = mix(leafNoiseCorner(xy.x ^ k0.z), leafNoiseCorner(xy.y ^ k0.z), s.x);
    const vec2 x10 = mix(leafNoiseCorner(xy.z ^ k0.z), leafNoiseCorner(xy.w ^ k0.z), s.x);
    const vec2 x01 = mix(leafNoiseCorner(xy.x ^ k1.z), leafNoiseCorner(xy.y ^ k1.z), s.x);
    const vec2 x11 = mix(leafNoiseCorner(xy.z ^ k1.z), leafNoiseCorner(xy.w ^ k1.z), s.x);
    return mix(mix(x00, x10, s.y), mix(x01, x11, s.y), s.z) * (1.0 / 65535.0);
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
    const float bright = 1.0 + (1.2 * amount) * (n.x - 0.5);
    const float hue = (n.y - 0.5) * (2.0 * amount);
    // hue > 0: toward albedo x (1.3, 1.08, 0.55), else toward albedo x (0.75, 0.95, 0.9) - as ONE factor on the colour
    // (mix(a, a x k, h) = a x (1 + (k - 1) h)), not two tinted colours and a blend of each.
    const vec3 slope = hue > 0.0 ? vec3(0.3, 0.08, -0.45) : vec3(0.25, 0.05, 0.1);
    return albedo * ((1.0 + slope * hue) * bright);
}

#endif
