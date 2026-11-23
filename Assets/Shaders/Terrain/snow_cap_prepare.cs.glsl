#version 460

// THE SNOW CAPS' PREPARE (SnowCapPipeline; Docs/Context/RendererVK/Snow.md "The caps"): one workgroup per TILE slot (8 m,
// SNOW_CAP_TILE site texels a side) of the site window the map holds (pc_origin), every frame, within "Cap range" + 16 m
// of the centre view. Per own texel ONE WORD (RendererVKLayout::SNOW_CAP_*): bits 0..15 the cap's height over the top
// (fp16, m), OBJECT, RAISED and the KEEP bits of the quads whose min corner it is. The cap VS reads only the words and the
// tops, so every vertex on a texel sees the same height (no cracks). A tile with no object texel writes zero words.
//   THE DEPTH s per object texel (the tile and its apron): the pack at the top's temperature and its catch (the pass: the
//          top is the column's own), x the slope rule (the tops' normal) x the water gate, x the window's and the range's fades.
//   THE EDGE: a texel whose 4-neighbour holds no cap and is not higher (a roof's edge, the window's), or holds one and lies
//          lower than the surface's own plane (extrapolated from the opposite neighbour: a steep roof is no edge) by more
//          than "Cap max step" AND than that neighbour's snow (a step its snow buries is no edge either). A HIGHER neighbour
//          (a wall, a chimney, a crate on the roof) ends nothing: the snow banks against it.
//   THE ROUNDING: d = the exact distance to the nearest edge texel (a separable search, 7 texels), the profile
//          p = 1 - (1 - x)^2 with x = d / min("Cap edge rounding", 2 s) (steep at the rim, flat inside), and the cap's
//          height over the top -sink + g p (s + track + sink): g fades it in from "Caps from depth" to twice it, the TRACKS
//          (the coarse deformation map: the same lattice) press it. Under 1 cm over the top a texel is not RAISED and sinks
//          "Cap edge sink" (no cap lies in a top's plane: no z-fighting with the texture layer).
//   THE QUADS: KEEP0 = the 4 corners are object texels, planar within the step (or the lowest corner's snow) and spread
//          within it + the slope allowance; KEEP1 / KEEP2 = all the KEEP0 quads inside the 2x2 / 4x4 block (a coarse quad
//          never bridges an alley).
//   THE DRAW: a tile with a raised texel (or a quad into a neighbour's snow), its 8 neighbours inside the window (the VS
//          reads across the edge), in range and in the frustum: appended to its LOD's list, the LOD from the nearest point's
//          distance against "Cap full detail" and twice it - FINER where a coarser LOD would drop a raised quad (a narrow
//          top: no pop). The tile's LOD goes into the LOD TABLE (per slot, 0xFFFFFFFF = not drawn): the VS stitches a border
//          vertex to a coarser neighbour's edge.

layout (local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "ubo.inc.glsl"
#define TERRAIN_HEIGHT_BINDING 1
#include "terrain_height.inc.glsl"
layout (binding = 2) uniform sampler2D u_siteTop;
layout (binding = 3) uniform sampler2D u_sitePass;
layout (binding = 4, r8ui) uniform readonly uimage2D u_siteClass;
layout (binding = 5) uniform sampler2D u_deformCoarse;
layout (binding = 6, std430) writeonly buffer CapWords { uint w_words[]; };
// The draw records (SNOW_CAP_LODS segments of TILES^2 uvec4), then the LOD table (TILES^2 uint, by slot).
layout (binding = 7, std430) writeonly buffer CapDraws { uvec4 w_draws[]; };
layout (binding = 8, std430) buffer CapCommands { uint b_commands[]; }; // SNOW_CAP_LODS x VkDrawIndexedIndirectCommand
layout (binding = 9, std430) writeonly buffer CapLods { uint w_lods[]; };
#define SNOW_SITE_TOP_SAMPLER u_siteTop
#define SNOW_SITE_PASS_SAMPLER u_sitePass
#include "snow.inc.glsl"
#include "push.generated.glsl"

#define N int(SNOW_CAP_TILE)
#define TILES uint(SNOW_CAP_TILES)
#define A int(SNOW_CAP_APRON)
#define L (N + 2 * A)       // the loaded square's side
#define REACH 7             // the edge search (texels): "Cap edge rounding" <= 1.75 m
#define FAR_SQ 1000000.0

shared float s_top[L * L];
shared float s_depth[L * L];             // the snow's depth (m) on the loaded object texels, 0 elsewhere
shared uint s_object[(L * L + 31) / 32]; // a bit per loaded texel
shared uint s_edge[(L * L + 31) / 32];
shared float s_rowD2[N * (N + 2 * REACH)]; // the row search's squared distance (own columns x rows -REACH .. N + REACH)
shared uint s_keep0[N];                   // a bit per own column, per own row
shared uint s_any, s_lost;                // an own object texel; the coarse LODs that would drop a raised quad (bit 0: 1, bit 1: 2)
shared uint s_minTop, s_maxTop, s_maxLift, s_maxDepth; // ordered floats

bool bitAt(uint i, bool edge) { return ((edge ? s_edge[i >> 5u] : s_object[i >> 5u]) >> (i & 31u) & 1u) != 0u; }
uint orderedFloat(float f) { const uint u = floatBitsToUint(f); return (u & 0x80000000u) != 0u ? ~u : u | 0x80000000u; }
float unorderedFloat(uint u) { return uintBitsToFloat((u & 0x80000000u) != 0u ? u & 0x7FFFFFFFu : ~u); }

// The coarse deformation at a lattice texel (the same 0.25 m lattice as the site map), decoded and refilled
// (snow.inc.glsl snowDeformDecode). 0 outside its window.
float capTrack(ivec2 lc)
{
	if (u_snow_deformPresent < 0.5)
		return 0.0;
	const ivec2 rel = lc - ivec2(u_snow_deformCoarseOrigin);
	if (any(lessThan(rel, ivec2(0))) || any(greaterThanEqual(rel, ivec2(SNOW_DEFORM_RES))))
		return 0.0;
	const vec2 rg = texelFetch(u_deformCoarse, lc & ivec2(SNOW_DEFORM_RES - 1), 0).rg;
	float offset = rg.g * SNOW_DEFORM_RANGE + SNOW_DEFORM_MIN;
	if (offset < 0.0)
		offset = min(0.0, offset + mod(u_snow_fallNow - round(rg.r * 65535.0), 65536.0) * 0.001);
	return offset;
}

// How far a texel's neighbour may lie off the top on a roof's own pitch (m): one texel x the steepest slope that still holds
// snow (the slope rule's zero), at most 4:1.
float capSlopeAllowance()
{
	const float n = clamp(1.0 - u_terrainTex_snowSlopeFull, 0.05, 1.0);
	return SNOW_SITE_TEXEL * min(sqrt(max(1.0 - n * n, 0.0)) / n, 4.0);
}

// KEEP0 of the quad whose min corner is the loaded texel li (its corners up to the next row / column).
bool capKeep0(uint li, float step, float slope)
{
	const uint i10 = li + 1u, i01 = li + uint(L), i11 = li + uint(L) + 1u;
	if (!bitAt(li, false) || !bitAt(i10, false) || !bitAt(i01, false) || !bitAt(i11, false))
		return false;
	const float t00 = s_top[li], t10 = s_top[i10], t01 = s_top[i01], t11 = s_top[i11];
	const float lo = min(min(t00, t10), min(t01, t11));
	const float lowDepth = lo == t00 ? s_depth[li] : lo == t10 ? s_depth[i10] : lo == t01 ? s_depth[i01] : s_depth[i11];
	const float tol = max(step, lowDepth); // a step the lowest corner's snow buries
	return abs(t00 - t10 - t01 + t11) < tol && max(max(t00, t10), max(t01, t11)) - lo < tol + 1.4142 * slope;
}

void main()
{
	const uint tid = gl_LocalInvocationIndex;
	// THE TILE of this slot inside the window held: the one tile coord with that slot whose 32 texels lie in it.
	const ivec2 slot = ivec2(gl_WorkGroupID.x % TILES, gl_WorkGroupID.x / TILES);
	const ivec2 firstTile = (pc_origin + N - 1) >> 5; // ceil(origin / 32) (arithmetic shift: floor for negatives too)
	const ivec2 tile = firstTile + ((slot - firstTile) & ivec2(int(TILES) - 1));
	const ivec2 tileMin = tile * N;
	if (any(greaterThan(tileMin + N, pc_origin + int(SNOW_SITE_RES))))
		return; // a partial tile at the window's far edge: never drawn, nor a drawn tile's neighbour
	const vec2 rectMin = vec2(tileMin) * SNOW_SITE_TEXEL, rectMax = vec2(tileMin + N) * SNOW_SITE_TEXEL;
	const vec2 camXZ = u_views_viewPos[VIEW_CENTER].xz;
	const float nearest = distance(clamp(camXZ, rectMin, rectMax), camXZ);
	if (nearest > u_snow_capRange + 16.0)
		return; // its words are not read this frame (a drawn tile is within the range, its neighbours within 12 m of it)

	// 1. LOAD the tile + its apron: the tops and which are object tops that hold a cap.
	if (tid < (L * L + 31) / 32)
	{
		s_object[tid] = 0u;
		s_edge[tid] = 0u;
	}
	if (tid < uint(N))
		s_keep0[tid] = 0u;
	if (tid == 0u)
	{
		s_any = 0u;
		s_lost = 0u;
		s_minTop = 0xFFFFFFFFu;
		s_maxTop = 0u;
		s_maxLift = 0u;
		s_maxDepth = 0u;
	}
	barrier();
	for (uint i = tid; i < uint(L * L); i += 256u)
	{
		const ivec2 lc = tileMin - A + ivec2(int(i) % L, int(i) / L);
		const ivec2 rel = lc - pc_origin;
		const bool inside = all(greaterThanEqual(rel, ivec2(0))) && all(lessThan(rel, ivec2(SNOW_SITE_RES)));
		const ivec2 s = lc & ivec2(SNOW_SITE_RES - 1);
		const float top = inside ? texelFetch(u_siteTop, s, 0).r : SNOW_SITE_NO_TOP;
		const uint cls = inside ? imageLoad(u_siteClass, s).r : 0u;
		s_top[i] = top;
		s_depth[i] = 0.0;
		if ((cls & (SNOW_SITE_CLASS_MASK | SNOW_SITE_STALE)) == SNOW_SITE_CLASS_OBJECT && top > SNOW_SITE_NO_TOP * 0.5)
		{
			atomicOr(s_object[i >> 5u], 1u << (i & 31u));
			const ivec2 own = lc - tileMin;
			if (all(greaterThanEqual(own, ivec2(0))) && all(lessThan(own, ivec2(N))))
				s_any = 1u; // (a benign race: every writer writes 1)
		}
	}
	barrier();
	const uint anyObject = s_any;
	if (anyObject == 0u || !snowPackLive() || u_snow_depth < 0.5 || u_terrain_hasSnow < 0.5 || !terrainHeightMapPresent())
	{
		for (uint i = tid; i < uint(N * N); i += 256u)
		{
			const ivec2 s = (tileMin + ivec2(int(i) % N, int(i) / N)) & ivec2(SNOW_SITE_RES - 1);
			w_words[s.y * SNOW_SITE_RES + s.x] = 0u;
		}
		return;
	}
	const float step = u_snow_capMaxStep;
	const float slope = capSlopeAllowance();

	// 2. THE DEPTH on every loaded object texel (the edges and the quads need the neighbours' snow).
	for (uint i = tid; i < uint(L * L); i += 256u)
	{
		if (!bitAt(i, false))
			continue;
		const ivec2 p = ivec2(int(i) % L, int(i) / L);
		const ivec2 lc = tileMin - A + p;
		const vec2 xz = (vec2(lc) + 0.5) * SNOW_SITE_TEXEL;
		const float top = s_top[i];
		// The tops' slope (one-sided across a step or a non-object neighbour; flat on the outer ring): the slope rule's normal.
		vec2 g = vec2(0.0);
		if (all(greaterThanEqual(p, ivec2(1))) && all(lessThanEqual(p, ivec2(L - 2))))
		{
			const uint li = i;
			const float tl = s_top[li - 1u], tr = s_top[li + 1u], td = s_top[li - uint(L)], tu = s_top[li + uint(L)];
			const float tol = step + slope;
			const bool hl = bitAt(li - 1u, false) && abs(tl - top) < tol, hr = bitAt(li + 1u, false) && abs(tr - top) < tol;
			const bool hd = bitAt(li - uint(L), false) && abs(td - top) < tol, hu = bitAt(li + uint(L), false) && abs(tu - top) < tol;
			g.x = hl && hr ? 0.5 * (tr - tl) : hr ? tr - top : hl ? top - tl : 0.0;
			g.y = hd && hu ? 0.5 * (tu - td) : hu ? tu - top : hd ? top - td : 0.0;
			g *= 1.0 / SNOW_SITE_TEXEL;
		}
		const float normalY = inversesqrt(1.0 + dot(g, g));
		const float catchF = texelFetch(u_sitePass, lc & ivec2(SNOW_SITE_RES - 1), 0).r;
		const vec4 climate = terrainClimateAt(xz);
		const vec2 rel = (vec2(lc) + 0.5 - vec2(pc_origin)) * (1.0 / float(SNOW_SITE_RES)) - 0.5;
		const float window = 1.0 - smoothstep(0.38, 0.43, max(abs(rel.x), abs(rel.y)));
		const float range = 1.0 - smoothstep(0.8 * u_snow_capRange, u_snow_capRange, distance(xz, camXZ));
		s_depth[i] = snowPackDepth(terrainTemperatureAt(climate, top), catchF) * snowGroundHolds(normalY)
			* snowGroundDry(top, terrainDataAt(xz).y, 0.0) * window * range;
	}
	barrier();

	// 3. THE EDGES over the loaded square but its outer ring (whose neighbours are not loaded).
	for (uint i = tid; i < uint(L * L); i += 256u)
	{
		const ivec2 p = ivec2(int(i) % L, int(i) / L);
		if (any(lessThan(p, ivec2(1))) || any(greaterThan(p, ivec2(L - 2))) || !bitAt(i, false))
			continue;
		const float top = s_top[i];
		bool edge = false;
		const int nb[4] = { int(i) - 1, int(i) + 1, int(i) - L, int(i) + L };
		const int op[4] = { int(i) + 1, int(i) - 1, int(i) + L, int(i) - L };
		for (int k = 0; k < 4; ++k)
		{
			const float t = s_top[nb[k]];
			if (!bitAt(uint(nb[k]), false))
				edge = edge || t < top + step;
			else
			{
				// Lower than the surface's own plane (from the opposite side, on a roof's pitch), by more than the step and
				// the lower neighbour's snow.
				const float o = s_top[op[k]];
				const float expect = bitAt(uint(op[k]), false) && abs(o - top) < step + slope ? 2.0 * top - o : top;
				edge = edge || min(top, expect) - t > max(step, s_depth[nb[k]]);
			}
		}
		if (edge)
			atomicOr(s_edge[i >> 5u], 1u << (i & 31u));
	}
	barrier();

	// 4. THE ROW SEARCH: per own column and per row in -REACH .. N + REACH, the squared distance (texels) to the nearest edge
	// texel in the row within REACH.
	for (uint i = tid; i < uint(N * (N + 2 * REACH)); i += 256u)
	{
		const int x = int(i) % N + A, y = int(i) / N + A - REACH;
		float best = FAR_SQ;
		for (int dx = -REACH; dx <= REACH; ++dx)
			if (bitAt(uint(y * L + x + dx), true))
				best = min(best, float(dx * dx));
		s_rowD2[i] = best;
	}
	barrier();

	// 5. THE WORDS of the own texels (4 per thread), the KEEP0 quads, the tile's height range (its quads' corners: the own
	// texels and the first ring after them).
	uint words[4];
	bool raisedOwn[4];
	for (int j = 0; j < 4; ++j)
	{
		const int i = int(tid) + 256 * j;
		const ivec2 own = ivec2(i % N, i / N);
		const ivec2 p = own + A;
		const uint li = uint(p.y * L + p.x);
		words[j] = 0u;
		raisedOwn[j] = false;
		// The ring after the tile (column N, row N): its tops and snow bound the box (a quad reaches it).
		if (own.x == N - 1 || own.y == N - 1)
		{
			const uint ring[3] = { li + 1u, li + uint(L), li + uint(L) + 1u };
			for (int r = 0; r < 3; ++r)
				if (bitAt(ring[r], false))
				{
					atomicMin(s_minTop, orderedFloat(s_top[ring[r]]));
					atomicMax(s_maxTop, orderedFloat(s_top[ring[r]]));
					atomicMax(s_maxDepth, orderedFloat(s_depth[ring[r]]));
				}
		}
		if (!bitAt(li, false))
			continue;
		const ivec2 lc = tileMin + own;
		const vec2 xz = (vec2(lc) + 0.5) * SNOW_SITE_TEXEL;
		const float top = s_top[li];
		const float depth = s_depth[li];

		// The edge distance (m).
		float d2 = FAR_SQ;
		for (int dy = -REACH; dy <= REACH; ++dy)
			d2 = min(d2, s_rowD2[(own.y + dy + REACH) * N + own.x] + float(dy * dy));
		const float edgeDist = sqrt(d2) * SNOW_SITE_TEXEL;

		const float fadeIn = smoothstep(u_snow_capFromDepth, 2.0 * u_snow_capFromDepth, depth);
		const float x = clamp(edgeDist / max(min(u_snow_capRounding, 2.0 * depth), 1e-3), 0.0, 1.0);
		const float profile = 1.0 - (1.0 - x) * (1.0 - x);
		float track = 0.0;
		if (u_snow_deformPresent > 0.5)
		{
			// snowDeformApply's clamp and snowDeformFade's distance fade (both under SNOW_DEFORM_READ, which needs u_textures[]).
			const float trackRange = max(u_snow_trackDetailRange, 1e-3);
			track = clamp(capTrack(lc), -depth, 0.5 * depth) * (1.0 - smoothstep(trackRange, 2.0 * trackRange, distance(xz, camXZ)));
		}
		const float rawLift = -u_snow_capSink + fadeIn * profile * (depth + track + u_snow_capSink);
		const bool raised = rawLift > 0.01; // under 1 cm the cap could lie in the top's plane: it sinks instead
		const float lift = raised ? rawLift : -u_snow_capSink;
		raisedOwn[j] = raised;
		words[j] = (packHalf2x16(vec2(lift, 0.0)) & 0xFFFFu) | SNOW_CAP_OBJECT | (raised ? SNOW_CAP_RAISED : 0u);
		atomicMin(s_minTop, orderedFloat(top));
		atomicMax(s_maxTop, orderedFloat(top));
		atomicMax(s_maxLift, orderedFloat(lift));
		atomicMax(s_maxDepth, orderedFloat(depth));
		if (capKeep0(li, step, slope))
			atomicOr(s_keep0[own.y], 1u << uint(own.x));
	}
	barrier();

	// 6. KEEP1 / KEEP2 from the KEEP0 bits, the words out, and whether a coarse LOD would drop a raised quad.
	for (int j = 0; j < 4; ++j)
	{
		const int i = int(tid) + 256 * j;
		const ivec2 own = ivec2(i % N, i / N);
		uint word = words[j];
		const bool keep0 = ((s_keep0[own.y] >> uint(own.x)) & 1u) != 0u;
		// The 2x2 / 4x4 block that holds this quad: are all its KEEP0 quads there?
		const ivec2 b1 = own & ~1, b2 = own & ~3;
		const uint m1 = 3u << uint(b1.x), m2 = 15u << uint(b2.x);
		const bool keep1 = (s_keep0[b1.y] & m1) == m1 && (s_keep0[b1.y + 1] & m1) == m1;
		bool keep2 = true;
		for (int r = 0; r < 4; ++r)
			keep2 = keep2 && (s_keep0[b2.y + r] & m2) == m2;
		if ((word & SNOW_CAP_OBJECT) != 0u)
		{
			word |= keep0 ? SNOW_CAP_KEEP0 : 0u;
			word |= keep1 && all(equal(own, b1)) ? SNOW_CAP_KEEP0 << 1u : 0u;
			word |= keep2 && all(equal(own, b2)) ? SNOW_CAP_KEEP0 << 2u : 0u;
			if (keep0 && raisedOwn[j])
				atomicOr(s_lost, (keep1 ? 0u : 1u) | (keep2 ? 0u : 2u));
		}
		const ivec2 s = (tileMin + own) & ivec2(SNOW_SITE_RES - 1);
		w_words[s.y * SNOW_SITE_RES + s.x] = word;
	}
	barrier();

	// 7. THE DRAW: thread 0, from the tile's height range.
	if (tid != 0u)
		return;
	const float maxLift = unorderedFloat(s_maxLift);
	const float maxDepth = s_maxDepth == 0u ? 0.0 : unorderedFloat(s_maxDepth);
	// A KEEP0 quad on the last column or row reaches into the next tile, whose snow it may carry.
	bool crosses = s_keep0[N - 1] != 0u;
	for (int y = 0; y < N - 1 && !crosses; ++y)
		crosses = ((s_keep0[y] >> uint(N - 1)) & 1u) != 0u;
	if (s_maxTop == 0u || !(maxLift > 0.01 || (crosses && maxDepth > u_snow_capFromDepth)))
		return; // nothing raised
	if (any(lessThan(tileMin - N, pc_origin)) || any(greaterThan(tileMin + 2 * N, pc_origin + int(SNOW_SITE_RES))) || nearest > u_snow_capRange)
		return;
	// The box of its vertices: the texel centres (a half texel in), the corners' tops, their highest snow (a berm: x 1.5).
	const float halfTexel = 0.5 * SNOW_SITE_TEXEL;
	const vec3 boxMin = vec3(rectMin.x + halfTexel, unorderedFloat(s_minTop) - u_snow_capSink - 0.1, rectMin.y + halfTexel);
	const vec3 boxMax = vec3(rectMax.x + halfTexel, unorderedFloat(s_maxTop) + max(maxLift, 1.5 * maxDepth) + step + 0.1, rectMax.y + halfTexel);
	for (int k = 0; k < 6; ++k)
	{
		const vec4 plane = u_frustumPlanes[k];
		const vec3 far = mix(boxMin, boxMax, greaterThan(plane.xyz, vec3(0.0))); // the corner farthest along the normal
		if (dot(vec4(far, 1.0), plane) < 0.0)
			return;
	}
	uint lod = nearest < u_snow_capDetail ? 0u : nearest < 2.0 * u_snow_capDetail ? 1u : 2u;
	if (lod == 2u && (s_lost & 2u) != 0u)
		lod = 1u;
	if (lod == 1u && (s_lost & 1u) != 0u)
		lod = 0u;
	const uint index = atomicAdd(b_commands[lod * 5u + 1u], 1u); // the instance count
	w_draws[lod * TILES * TILES + index] = uvec4(uvec2(tile), lod, 0u);
	w_lods[gl_WorkGroupID.x] = lod;
}
