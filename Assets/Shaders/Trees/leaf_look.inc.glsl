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

// Trilinear value noise in [0, 1] (one lattice cell = 1 unit).
float leafValueNoise(vec3 p)
{
    const vec3 c = floor(p);
    const vec3 f = p - c;
    const vec3 s = f * f * (3.0 - 2.0 * f);
    const ivec3 i = ivec3(c);
    float corners[8];
    for (int k = 0; k < 8; ++k)
    {
        const ivec3 q = i + ivec3(k & 1, (k >> 1) & 1, k >> 2);
        corners[k] = float(hashU(uint(q.x) * 0x8da6b343u ^ uint(q.y) * 0xd8163841u ^ uint(q.z) * 0xcb1ab31fu) & 0xFFFFu) * (1.0 / 65535.0);
    }
    const float x00 = mix(corners[0], corners[1], s.x), x10 = mix(corners[2], corners[3], s.x);
    const float x01 = mix(corners[4], corners[5], s.x), x11 = mix(corners[6], corners[7], s.x);
    return mix(mix(x00, x10, s.y), mix(x01, x11, s.y), s.z);
}

// PATCHES of leaf colour over a crown ("Leaf patch variation" / "Leaf patch size (m)"): one noise for the brightness,
// one toward a sunnier yellow-green or a deeper blue-green - a crown in groups of lighter and darker, yellower and
// greener leaves, not one flat colour. World space (no per-card data reaches the FS); the wind moves a leaf a few cm,
// against patches of metres.
vec3 leafPatchTint(vec3 albedo, vec3 worldPos)
{
    const float amount = u_foliage_leafPatchVariation;
    if (amount <= 0.0)
        return albedo;
    const vec3 p = worldPos * u_foliage_invLeafPatchSize;
    const float bright = leafValueNoise(p) - 0.5;
    const float hue = (leafValueNoise(p * 1.7 + vec3(17.3, 5.1, 9.7)) - 0.5) * 2.0 * amount;
    const vec3 tinted = hue > 0.0 ? mix(albedo, albedo * vec3(1.3, 1.08, 0.55), hue)
                                  : mix(albedo, albedo * vec3(0.75, 0.95, 0.9), -hue);
    return tinted * (1.0 + 1.2 * amount * bright);
}

#endif
