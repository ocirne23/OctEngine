// A tree's SHADOW BILLBOARD at a RAY HIT (MATERIAL_FLAG_BILLBOARD, Layout.ixx; Procedural TreeSystem). The card has no
// albedo texture. Its albedo slot is a ONE-CHANNEL mask shown in all four channels, so `.a` reads it: 0 = empty, >= the
// cutoff (0.5) = solid, and from TREE_CARD_BARK to 1 the LEAF SHARE of the texel. Its normal slot is a 2 x 1 texture of
// the species' two colours - the bark's mean, then the leaves' - as their square roots (8 bits hold a dark colour
// better). A hit's albedo = the two mixed by the leaf share; its normal = the crown shape's (treeCardNormal).
// An EMPTY texel takes the leaf colour: the rays that shade a hit run with the opaque flag (the whole card is solid to
// them), and a crown's gaps are among its leaves.
// The includer declares u_textures[] and enables GL_EXT_nonuniform_qualifier.
#ifndef TREE_CARD_INC_GLSL
#define TREE_CARD_INC_GLSL

// diffuseNormalTexIdx = the material's packed word (the colours' texture in its high half), mask = its albedo tap's `.a`.
vec3 treeCardAlbedo(uint diffuseNormalTexIdx, float mask)
{
	const float leafShare = mask < 0.5 ? 1.0 : clamp((mask - TREE_CARD_BARK) * (1.0 / (1.0 - TREE_CARD_BARK)), 0.0, 1.0);
	// The two texels' centres lie at u = 0.25 and 0.75: the filter mixes them between.
	const vec3 c = textureLod(u_textures[nonuniformEXT(diffuseNormalTexIdx >> 16)], vec2(0.25 + 0.5 * leafShare, 0.5), 0.0).rgb;
	return c * c;
}

// A hit's NORMAL: the three flat cards have none worth shading with (one card faces the sun, the next one is edge-on to
// it). It is the normal of the tree's CROWN SHAPE where the RAY enters it, so it depends on the ray alone and the three
// cards agree. The shape: an ellipsoid in the cards' box above its centre, a vertical cylinder below it (a trunk and a
// crown's lower half face sideways, not down). A ray that passes the shape takes its closest point: the silhouette.
// origin / dir = the ray in the tree's OBJECT space (dir at any length), centre = the card mesh's bound centre (on the
// trunk axis), radii = a card vertex's tangent (Procedural billboardMesh). Returns an OBJECT-space normal that faces the
// ray, not unit.
vec3 treeCardNormal(vec3 origin, vec3 dir, vec3 centre, vec3 radii)
{
	const vec3 invR = 1.0 / max(radii, vec3(1e-3));
	const vec3 o = (origin - centre) * invR, d = dir * invR; // the unit sphere's space
	const float invDd = 1.0 / max(dot(d, d), 1e-12);
	const vec3 q = o - d * (dot(o, d) * invDd);                 // the ray's closest point to the centre
	const vec3 s = q - d * sqrt(max(1.0 - dot(q, q), 0.0) * invDd); // its entry (a passing ray: q itself)
	const vec3 n = vec3(s.x, max(s.y, 0.0), s.z) * invR;       // the shape's gradient
	// A ray along the axis from below has none: it takes the ray's own direction.
	if (dot(n, n) < 1e-10)
		return -dir;
	return dot(n, dir) > 0.0 ? -n : n;
}

#endif
