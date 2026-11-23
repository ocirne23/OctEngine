#version 460

#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_EXT_shader_16bit_storage : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier : enable

// THE SNOW CAPS (SnowCapPipeline; Docs/Context/RendererVK/Snow.md "The caps"): one instance per drawn tile (its record by
// gl_InstanceIndex, firstInstance = its LOD's segment), (32 >> lod)^2 quads of 4 vertices of their own (the index buffer:
// quad q = vertices 4q .. 4q + 3 = its corners (0, 0), (1, 0), (0, 1), (1, 1)). A vertex is a site texel's centre at the
// height top + the prepare's word; all 4 vertices of a quad read the same 4 corners, so they agree on its fate:
//   KEPT  - the KEEP bit of the LOD at the quad's min corner (snow_cap_prepare.cs.glsl), and a corner stands raised;
//   BANK  - the quad crosses a step UP (a wall, a crate): the low corners hold the cap - the high corners take the low ones'
//           highest snow where theirs is lower, so the cap runs INTO a wall (the depth test hides that part) and over a
//           buried crate's rim, and leaves no slit at the foot;
//   else  - collapsed to a point.
// A CORNER's height (capCorner) is a function of its lattice point and the LOD only, so every quad and tile agrees on it:
//   THE MORPH (LOD 0 and 1, CDLOD): over the last 20 % of the LOD's band a point the next LOD lacks slides to the next LOD's
//           surface (the mean of its two neighbours along its odd axis; both odd: the quad's 1-2 diagonal), not across a step;
//   THE STITCH: a point on the tile's border whose neighbour tile draws COARSER (the prepare's LOD table) lies on that
//           tile's edge (between its two points there, as it places them) - a tile drawn finer than its distance (a narrow
//           top) never cracks against the next.
// THE NORMAL from the heights at +-stride (one-sided across a drop or a non-object texel); a bank quad takes its face's.

#include "shared.inc.glsl"
layout (binding = 27) uniform sampler2D u_textures[]; // the forward set's bindless array: the site map's top

layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer CapWords { uint w[]; };
layout (buffer_reference, scalar, buffer_reference_align = 16) readonly buffer CapDraws { uvec4 d[]; };
layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer CapLods { uint lod[]; };

layout (location = 0) out vec3 out_pos;
layout (location = 1) out vec3 out_normal;
layout (location = 2) out float out_top; // the top under the vertex (the shadow ray's start floor)

uint capWord(ivec2 lc)
{
	const ivec2 s = lc & ivec2(SNOW_SITE_RES - 1);
	CapWords words = CapWords(u_snow_capWords.xy);
	return words.w[s.y * int(SNOW_SITE_RES) + s.x];
}
float capTop(ivec2 lc) { return texelFetch(u_textures[u_snow_siteTopTex], lc & ivec2(SNOW_SITE_RES - 1), 0).r; }
float capLift(uint word) { return unpackHalf2x16(word & 0xFFFFu).x; }
bool capObject(uint word) { return (word & SNOW_CAP_OBJECT) != 0u; }

// The cap's height at a texel (its word and top), or `valid` false where it holds no cap.
float capHeight(ivec2 lc, out bool valid)
{
	const uint word = capWord(lc);
	valid = capObject(word);
	return capTop(lc) + capLift(word);
}

// The drawn LOD of a tile (the prepare's table, by slot), 0xFFFFFFFF if it is not drawn.
uint capTileLod(ivec2 tile)
{
	const ivec2 slot = tile & ivec2(int(SNOW_CAP_TILES) - 1);
	CapLods lods = CapLods(u_snow_capLods.xy);
	return lods.lod[slot.y * int(SNOW_CAP_TILES) + slot.x];
}

// THE MORPH of the point lc (raw height h) at a LOD: toward the next LOD's surface over the end of its band.
float capMorph(ivec2 lc, float h, uint lod)
{
	if (lod >= 2u)
		return h;
	const int stride = 1 << lod;
	const bvec2 odd = bvec2(((lc.x >> lod) & 1) != 0, ((lc.y >> lod) & 1) != 0); // tiles start at multiples of 32
	if (!odd.x && !odd.y)
		return h;
	const vec2 xz = (vec2(lc) + 0.5) * SNOW_SITE_TEXEL;
	const float band = u_snow_capDetail * float(1u << lod); // LOD 0 ends at "Cap full detail", LOD 1 at twice it
	const float m = smoothstep(0.8 * band, band, distance(xz, u_views_viewPos[VIEW_CENTER].xz));
	if (m <= 0.0)
		return h;
	const ivec2 a = odd.x && odd.y ? ivec2(stride, -stride) : odd.x ? ivec2(stride, 0) : ivec2(0, stride);
	bool va, vb;
	const float ha = capHeight(lc + a, va), hb = capHeight(lc - a, vb);
	const float stepH = u_snow_capMaxStep * float(stride);
	if (!va || !vb || abs(ha - h) > stepH || abs(hb - h) > stepH)
		return h; // not across a step (a wall's top)
	return mix(h, 0.5 * (ha + hb), m);
}

// A corner's final height: the stitch to a coarser neighbour on the tile's border, else the morph.
float capCorner(ivec2 lc, float h, ivec2 tile, uint lod)
{
	const ivec2 rel = lc - tile * int(SNOW_CAP_TILE);
	const bool onX = rel.x == 0 || rel.x == int(SNOW_CAP_TILE), onZ = rel.y == 0 || rel.y == int(SNOW_CAP_TILE);
	if (onX != onZ) // on an edge, not a corner (a tile corner is a point of every LOD)
	{
		const ivec2 across = onX ? ivec2(rel.x == 0 ? -1 : 1, 0) : ivec2(0, rel.y == 0 ? -1 : 1);
		const uint nl = capTileLod(tile + across);
		if (nl != 0xFFFFFFFFu && nl > lod)
		{
			const int S = 1 << nl;
			const int along = onX ? rel.y : rel.x;
			const int r = along & (S - 1);
			if (r != 0)
			{
				const ivec2 dir = onX ? ivec2(0, 1) : ivec2(1, 0);
				const ivec2 pa = lc - dir * r, pb = pa + dir * S;
				bool va, vb;
				const float ha = capHeight(pa, va), hb = capHeight(pb, vb);
				if (va && vb)
					return mix(capMorph(pa, ha, nl), capMorph(pb, hb, nl), float(r) / float(S));
			}
			return capMorph(lc, h, nl);
		}
	}
	return capMorph(lc, h, lod);
}

void main()
{
	CapDraws draws = CapDraws(u_snow_capDraws.xy);
	const uvec4 record = draws.d[gl_InstanceIndex];
	const ivec2 tile = ivec2(record.xy);
	const uint lod = record.z;
	const int stride = 1 << lod;
	const int n = int(SNOW_CAP_TILE) >> lod;
	const int quad = gl_VertexIndex >> 2;
	const int corner = gl_VertexIndex & 3;
	const ivec2 base = tile * int(SNOW_CAP_TILE) + ivec2(quad % n, quad / n) * stride;

	uint w[4];
	float top[4], raw[4];
	bool raised = false;
	for (int k = 0; k < 4; ++k)
	{
		const ivec2 c = base + ivec2(k & 1, k >> 1) * stride;
		w[k] = capWord(c);
		top[k] = capTop(c);
		raw[k] = top[k] + capLift(w[k]);
		raised = raised || (w[k] & SNOW_CAP_RAISED) != 0u;
	}
	const bool kept = (w[0] & (SNOW_CAP_KEEP0 << lod)) != 0u;

	// BANK: split at the lowest corner's top + the step (x the stride: a coarse quad spans more of a slope).
	const float lowTop = min(min(top[0], top[1]), min(top[2], top[3])) + u_snow_capMaxStep * float(stride);
	bool bank = false;
	if (!kept && raised)
	{
		bool lowOk = true, anyHigh = false;
		for (int k = 0; k < 4; ++k)
			if (top[k] <= lowTop)
				lowOk = lowOk && capObject(w[k]);
			else
				anyHigh = true;
		bank = lowOk && anyHigh;
	}
	if (!raised || !(kept || bank))
	{
		gl_Position = vec4(0.0, 0.0, 0.0, 1.0); // a point: the quad draws nothing
		out_pos = vec3(0.0);
		out_normal = vec3(0.0, 1.0, 0.0);
		out_top = 0.0;
		return;
	}

	// The corners' final heights (every vertex the same 4, so the quad stays whole).
	float h[4];
	for (int k = 0; k < 4; ++k)
		h[k] = capCorner(base + ivec2(k & 1, k >> 1) * stride, raw[k], tile, lod);
	const ivec2 lc = base + ivec2(corner & 1, corner >> 1) * stride;
	const vec2 xz = (vec2(lc) + 0.5) * SNOW_SITE_TEXEL;
	float height = h[corner];
	float under = top[corner];
	vec3 normal;
	if (bank)
	{
		float bankH = -1e30;
		for (int k = 0; k < 4; ++k)
			if (top[k] <= lowTop)
				bankH = max(bankH, h[k]);
		float hb[4];
		for (int k = 0; k < 4; ++k)
			hb[k] = top[k] > lowTop ? max(capObject(w[k]) ? h[k] : -1e30, bankH) : h[k];
		if (top[corner] > lowTop)
		{
			height = hb[corner];
			under = min(top[corner], height);
		}
		// The face's normal over the 4 (banked) corners.
		const float span = SNOW_SITE_TEXEL * float(stride);
		normal = normalize(vec3(-0.5 * (hb[1] - hb[0] + hb[3] - hb[2]) / span, 1.0, -0.5 * (hb[2] - hb[0] + hb[3] - hb[1]) / span));
	}
	else
	{
		// THE NORMAL from the raw heights at +-stride.
		const float stepH = u_snow_capMaxStep * float(stride);
		bool vl, vr, vd, vu;
		const float hl = capHeight(lc - ivec2(stride, 0), vl), hr = capHeight(lc + ivec2(stride, 0), vr);
		const float hd = capHeight(lc - ivec2(0, stride), vd), hu = capHeight(lc + ivec2(0, stride), vu);
		const float hc = raw[corner];
		vl = vl && abs(hl - hc) < stepH;
		vr = vr && abs(hr - hc) < stepH;
		vd = vd && abs(hd - hc) < stepH;
		vu = vu && abs(hu - hc) < stepH;
		const float span = SNOW_SITE_TEXEL * float(stride);
		const float gx = vl && vr ? 0.5 * (hr - hl) : vr ? hr - hc : vl ? hc - hl : 0.0;
		const float gz = vd && vu ? 0.5 * (hu - hd) : vu ? hu - hc : vd ? hc - hd : 0.0;
		normal = normalize(vec3(-gx / span, 1.0, -gz / span));
	}

	out_pos = vec3(xz.x, height, xz.y);
	out_normal = normal;
	out_top = under;
	gl_Position = u_mvp * vec4(out_pos, 1.0);
	gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space)
}
