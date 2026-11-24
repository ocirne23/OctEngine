#ifndef SNOW_INC_GLSL
#define SNOW_INC_GLSL

// THE SNOWPACK (the renderer's SnowPack; Docs/Context/RendererVK/Snow.md): the snow depth on the ground (m) per CLIMATE
// temperature at the shaded height (C) and per CATCH (1 = open flat ground), integrated on the CPU in game time. This
// slot's table, by device address (u_snow_table.xy; the preamble enables GL_EXT_buffer_reference + _uvec2), catch-major:
// [c * SNOW_TABLE_TEMP_BINS + t]. The includer has the frame UBO.

layout (buffer_reference, scalar, buffer_reference_align = 4) readonly buffer SnowPackTable { float d[]; };
// THE STATE of each cell's surface (SnowPack::CellState), after the depths in the same order (u_snow_stateTable.xy): grain
// diameter (mm), meltwater (0..1), grime (0..1), 0.
layout (buffer_reference, scalar, buffer_reference_align = 16) readonly buffer SnowStateTable { vec4 s[]; };

// The bilinear footprint in the table: the first cell and the weights along temperature and catch, clamped to the edges.
void snowTableCoord(float tempC, float catchF, out uint i, out float ft, out float fc)
{
	const float t = clamp((tempC - SNOW_TABLE_TEMP_MIN) * SNOW_TABLE_TEMP_INV_STEP, 0.0, float(SNOW_TABLE_TEMP_BINS - 1u));
	const float c = clamp(catchF * SNOW_TABLE_CATCH_INV_STEP, 0.0, float(SNOW_TABLE_CATCH_BINS - 1u));
	const uint t0 = min(uint(t), SNOW_TABLE_TEMP_BINS - 2u);
	const uint c0 = min(uint(c), SNOW_TABLE_CATCH_BINS - 2u);
	ft = t - float(t0);
	fc = c - float(c0);
	i = c0 * SNOW_TABLE_TEMP_BINS + t0;
}

// Bilinear over the two axes; clamped to the table's edges.
float snowPackDepth(float tempC, float catchF)
{
	uint i;
	float ft, fc;
	snowTableCoord(tempC, catchF, i, ft, fc);
	SnowPackTable table = SnowPackTable(u_snow_table.xy); // a reference cannot be const
	const float lo = mix(table.d[i], table.d[i + 1u], ft);
	const float hi = mix(table.d[i + SNOW_TABLE_TEMP_BINS], table.d[i + SNOW_TABLE_TEMP_BINS + 1u], ft);
	return mix(lo, hi, fc);
}

// The state at a temperature and catch, bilinear as the depth.
vec4 snowPackCell(float tempC, float catchF)
{
	uint i;
	float ft, fc;
	snowTableCoord(tempC, catchF, i, ft, fc);
	SnowStateTable table = SnowStateTable(u_snow_stateTable.xy);
	const vec4 lo = mix(table.s[i], table.s[i + 1u], ft);
	const vec4 hi = mix(table.s[i + SNOW_TABLE_TEMP_BINS], table.s[i + SNOW_TABLE_TEMP_BINS + 1u], ft);
	return mix(lo, hi, fc);
}

// The snow is falling or lying anywhere (a uniform branch: no table or map read otherwise).
bool snowPackLive() { return u_snow_enabled > 0.5 && u_snow_active > 0.5; }

// How white the ground is from the pack alone (0..1): full from "Weather/Snow/Full cover depth". 0 while the pack is empty
// or the snow is off.
float snowPackCover(float tempC, float catchF)
{
	if (!snowPackLive())
		return 0.0;
	const float x = clamp(snowPackDepth(tempC, catchF) * u_snow_invCoverDepth, 0.0, 1.0);
	return x * x * (3.0 - 2.0 * x);
}

// ---- THE SITE MAP (SnowSitePipeline): the top solid surface's world Y + the foliage pass, bindless, toroidal ----
// The includer has u_textures[], or defines both samplers (the grass cull and the wetness bind their own). A reader that
// runs BEFORE the trace in the frame (the culls, the wetness) defines SNOW_SITE_ORIGIN u_snow_siteReadOrigin: the window
// the map holds then.
#ifndef SNOW_SITE_TOP_SAMPLER
#define SNOW_SITE_TOP_SAMPLER u_textures[u_snow_siteTopTex]
#define SNOW_SITE_PASS_SAMPLER u_textures[u_snow_sitePassTex]
// With the bindless array: the canopy map too (snowSiteCatch's smooth shelter under the crowns). A reader that binds its
// own site samplers (the grass cull, the wetness, the caps' prepare, the tracks) takes the site map's pass alone.
#define SNOW_CANOPY
#include "canopy_occlusion.inc.glsl"
#endif
#ifndef SNOW_SITE_ORIGIN
#define SNOW_SITE_ORIGIN u_snow_siteOrigin
#endif

// The CATCH at a surface point (0..1, the table's axis): the share of the snowfall that reaches it - the foliage pass of
// its column, x 0 where a solid surface stands over it (a roof, an overhang: the point lies 0.3 .. 0.8 m or more under
// the column's top). Hardware bilinear (the REPEAT sampler wraps like the toroidal slots), so a drip line or a roof edge
// is a quarter-metre ramp. 1 without the map and outside its window (faded in over the window's outer 5 %). under0 /
// under1: how far under the top the point is sheltered from / fully (a reader whose height is the 8 m data map's, not the
// drawn surface's, needs more).
float snowSiteCatch(vec3 pos, float under0, float under1)
{
	if (u_snow_sitePresent < 0.5)
		return 1.0;
	const vec2 lattice = pos.xz * (1.0 / SNOW_SITE_TEXEL);
	const vec2 rel = (lattice - SNOW_SITE_ORIGIN) * (1.0 / float(SNOW_SITE_RES)) - 0.5;
	const float inside = 1.0 - smoothstep(0.44, 0.49, max(abs(rel.x), abs(rel.y)));
	if (inside <= 0.0)
		return 1.0;
	const vec2 uv = lattice * (1.0 / float(SNOW_SITE_RES));
	const float top = textureLod(SNOW_SITE_TOP_SAMPLER, uv, 0.0).r;
	const vec2 pass = textureLod(SNOW_SITE_PASS_SAMPLER, uv, 0.0).rg; // the pass, the highest foliage over the top
	const float exposed = 1.0 - smoothstep(under0, under1, top - pos.y);
	// Under the foliage only: a point above the column's highest blocking layer (a branch over its crown's shadow card, a
	// mover over a forest) has the open sky.
	const float underFoliage = smoothstep(-0.5, 0.5, top + pass.y * SNOW_SITE_FOLIAGE_RANGE - pos.y);
	// THE CROWNS' SHELTER: the site map's pass has each crown's sharp edge (a bare blob under every tree); toward the canopy
	// map's smooth cover by "Canopy snow blend" - full cover stops "Foliage block" of the snowfall, as one foliage layer.
	float foliage = pass.x;
#ifdef SNOW_CANOPY
	if (u_snow_canopyBlend > 0.0)
		foliage = mix(foliage, 1.0 - u_snow_canopyShelter * canopyTreeCover(pos.xz), u_snow_canopyBlend);
#endif
	return mix(1.0, mix(1.0, foliage, underFoliage) * exposed, inside);
}
float snowSiteCatch(vec3 pos) { return snowSiteCatch(pos, 0.3, 0.8); }

// ---- THE SNOW'S MATERIAL (every path: the splat, the object layer, the caps, the rocks): from the state, not a texture ----
// Physics, not tweaks: the ice's absorption per channel (m^-1, from its imaginary index, gamma = 4 pi k / lambda; good to
// ~2x) and the absorption path per grain diameter. The grime's at full grime is a LOOK value, u_snow_grimeAbs ("Grime
// strength", "Grime brownness": SnowPack::grimeAbsorption - the dust absorbs blue most). The C++ twin is
// SnowPack::surfaceAlbedo.
#define SNOW_ICE_ABS   vec3(0.31, 0.046, 0.016)
#define SNOW_ABS_PATH  13.0

struct SnowState { float grainM; float liquid; float grime; }; // m, 0..1, 0..1

SnowState snowStateFrom(vec4 cell) { return SnowState(max(cell.x, 0.01) * 1e-3, clamp(cell.y, 0.0, 1.0), clamp(cell.z, 0.0, 1.0)); }
// New snow: the climate's permanent snow, and every path while the pack is off.
SnowState snowFreshState() { return SnowState(u_snow_freshGrainM, 0.0, 0.0); }
// The state at a temperature and catch. Every path reads the OPEN snow's (catch 1): the shelter thins the depth and the
// cover, and changes the surface's state little - a second site-map read per snow pixel for it was not worth its fetches.
SnowState snowStateAt(float tempC, float catchF) { return snowPackLive() ? snowStateFrom(snowPackCell(tempC, catchF)) : snowFreshState(); }

// The surface's albedo: the spherical albedo of a weakly absorbing granular medium, exp(-sqrt(gamma x path)). A water film
// joins the grains into larger optical ones (x (1 + 2 liquid)). fresh 0.1 mm: (0.980, 0.991, 0.996).
vec3 snowAlbedo(SnowState s)
{
	const float dEff = s.grainM * (1.0 + 2.0 * s.liquid);
	return exp(-sqrt((SNOW_ICE_ABS + s.grime * u_snow_grimeAbs) * (SNOW_ABS_PATH * dEff)));
}
// The ice facets' GGX alpha: "Grain roughness" dry, toward "Wet snow roughness" with the meltwater.
float snowAlpha(SnowState s) { return mix(u_snow_grainRough, u_snow_wetRough, s.liquid); }
// The mirror facets per m^2 that can glint: "Sparkle density" of the grains (fewer once wet: the film rounds them).
float snowGlintDensity(SnowState s) { return u_snow_glintShare * (1.0 - 0.85 * s.liquid) / (s.grainM * s.grainM); }
// The pixel's footprint on the ground (m): the area of its xz derivatives, floored by a quarter of the long axis (grazing).
float snowFootprint(vec2 dx, vec2 dz) { return max(sqrt(abs(dx.x * dz.y - dx.y * dz.x)), 0.25 * sqrt(max(dot(dx, dx), dot(dz, dz)))); }

#if defined(SNOW_HALF) || defined(SNOW_OBJECTS) || defined(LIT_SNOW)
// The relief's tangent XY (the snow relief's nor texture, world X / Z) on a base normal: the ONE decode of every path, so the
// terrain, the object layer and the caps tilt alike.
f16vec3 snowNormalOn(f16vec3 baseN, f16vec2 nxy)
{
	const float16_t z = sqrt(max(float16_t(1.0) - dot(nxy, nxy), float16_t(0.0)));
	return normalize(f16vec3(nxy.x, float16_t(0.0), nxy.y) + z * baseN);
}
#endif

// ---- THE GROUND (terrainLayers, the rock FS, the terrain VS's lift): one rule for the cover and the depth ----

// The cover of a depth (m): full from "Weather/Snow/Full cover depth".
float snowCoverFromDepth(float depth)
{
	const float x = clamp(depth * u_snow_invCoverDepth, 0.0, 1.0);
	return x * x * (3.0 - 2.0 * x);
}

// The pack's DEPTH on the ground at a point (m): the table at its temperature and its site catch.
float snowGroundDepth(float tempC, vec3 pos)
{
	return snowPackLive() ? snowPackDepth(tempC, snowSiteCatch(pos)) : 0.0;
}

// The pack's cover on the GROUND at a point.
float snowGroundCover(float tempC, vec3 pos)
{
	return snowCoverFromDepth(snowGroundDepth(tempC, pos));
}

// The slopes that hold snow ("Terrain/Textures/Snow slope start / full"; normalY = the SMOOTH normal's y).
float snowGroundHolds(float normalY)
{
	return 1.0 - smoothstep(u_terrainTex_snowSlopeStart, u_terrainTex_snowSlopeFull, 1.0 - clamp(normalY, 0.0, 1.0));
}

// A two-octave VALUE NOISE over the world XZ (0..1; ~45 m and ~11 m cells), integer-hashed: exact at any world coordinate,
// and the same in every stage (the ground's VS lift and FS white agree).
float snowShoreHash(ivec2 c)
{
	uint h = uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u;
	h = (h ^ (h >> 15u)) * 0x2c1b3c6du;
	return float(h ^ (h >> 12u)) * (1.0 / 4294967296.0);
}
float snowShoreValue(vec2 p)
{
	const ivec2 i = ivec2(floor(p));
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(snowShoreHash(i), snowShoreHash(i + ivec2(1, 0)), f.x), mix(snowShoreHash(i + ivec2(0, 1)), snowShoreHash(i + ivec2(1, 1)), f.x), f.y);
}
float snowShoreNoise(vec2 xz) { return 0.65 * snowShoreValue(xz * (1.0 / 45.0)) + 0.35 * snowShoreValue(xz * (1.0 / 11.0) + 17.0); }

// Dry ground only: the pack melts into the sea, a lake, a river channel (the beds under the water stay bare). Every path
// takes it (the ground's white and lift, the rocks, the objects' layer, the caps, the tracks).
// THE SHORE: by the OCEAN (the local level is the sea's) it thins out over "Shore fade (m)" above the water - the height,
// not the distance (the map has none): a flat beach loses its snow over a wide strip, a cliff over a narrow one. THE RIVER
// BANKS: the floodplain's inner part thins by "River bank fade" toward the channel. Both edges wander by the noise
// ("Shore fade variation"), so neither is a line along the water. Only a point in a band pays the noise.
float snowGroundDry(vec3 pos, float waterLevel, float river)
{
	const float above = pos.y - waterLevel;
	float dry = smoothstep(-0.1, 0.2, above) * (1.0 - smoothstep(0.9, 1.0, river));
	const float band = u_snow_shoreFade * (1.0 - smoothstep(0.05, 1.0, abs(waterLevel - u_terrain_seaLevel)));
	const bool inShore = band > 0.0 && above < band * (1.0 + u_snow_shoreVariation);
	const bool inBank = u_snow_riverBankFade > 0.0 && river > 0.05;
	if (dry > 0.0 && (inShore || inBank))
	{
		const float n = snowShoreNoise(pos.xz) - 0.5; // -0.5 .. 0.5
		if (inShore)
			dry *= smoothstep(0.0, 1.0, above / (band * (1.0 + 2.0 * u_snow_shoreVariation * n)));
		if (inBank)
			dry *= 1.0 - u_snow_riverBankFade * smoothstep(0.2, 0.9, river + 0.5 * u_snow_shoreVariation * n);
	}
	return dry;
}

// THE GROUND'S LIFT (m, up): the terrain VS raises every vertex by the pack's depth there - the same depth terrainLayers
// covers by - so the snow has a surface, the relief and the tessellation ride on it, and what stands on the true ground
// (grass, clutter, rocks, trunks, buildings, feet) sinks into it. "Weather/Snow/Depth" (lockable) off: none. The pack
// only (the climate's permanent snow has no depth).
float snowGroundLift(vec3 pos, float tempC, float normalY, float waterLevel, float river)
{
	if (u_snow_depth < 0.5 || u_terrain_hasSnow < 0.5 || !snowPackLive())
		return 0.0;
	const float holds = snowGroundHolds(normalY);
	if (holds <= 0.0)
		return 0.0;
	return snowGroundDepth(tempC, pos) * holds * snowGroundDry(pos, waterLevel, river);
}

// ---- THE ROCK SHELL (instanced_indirect_rock.vs.glsl): rocks, dead wood and cacti swell by the snow on their top ----
// The site catch with a LOOSER shelter test: the map's top over a rock is the BLAS's level (the finest), so a coarser LOD's
// vertex can lie a few dm under it on its own top. The rock FS's white reads the same catch (no bare bulge).
#define SNOW_ROCK_UNDER0 0.5
#define SNOW_ROCK_UNDER1 1.0
float snowRockCatch(vec3 pos) { return snowSiteCatch(pos, SNOW_ROCK_UNDER0, SNOW_ROCK_UNDER1); }

// How far a rock vertex rises STRAIGHT UP (the VS adds it to y): the pack at its temperature and catch, on a face that holds
// snow (the ground's slope rule, as the FS's white), out of the water, faded out over the last 20 % of "Rock shell range".
// Up, not along the normal: along it, the snow pushed out sideways over a top's rim (an overhang) and a jagged break's
// vertices (a dead trunk's splinters, their normals every which way) moved apart and folded through each other. cut = how
// much the vertex lies on a CUT face (dead wood's bare wood: a break, a splinter, a sawn top; 0 bark, rock, cactus skin):
// there only a nearly flat face lifts (within ~15 degrees, gone by ~30), so the splinters keep their shape.
// "Weather/Snow/Rock shell" (lockable) and "Depth" gate it. The shadow VS and the BLAS keep the bare rock.
float snowRockShell(vec3 pos, float normalY, float tempC, float waterLevel, float cut)
{
	if (u_snow_rockShell < 0.5 || u_snow_depth < 0.5 || u_terrain_hasSnow < 0.5 || !snowPackLive())
		return 0.0;
	const float tilt = 1.0 - clamp(normalY, 0.0, 1.0);
	const float holds = mix(snowGroundHolds(normalY), 1.0 - smoothstep(0.035, 0.13, tilt), cut);
	const float fade = 1.0 - smoothstep(0.8 * u_snow_rockShellRange, u_snow_rockShellRange, distance(pos.xz, u_views_viewPos[VIEW_CENTER].xz));
	if (holds * fade <= 0.0)
		return 0.0;
	return snowPackDepth(tempC, snowRockCatch(pos)) * holds * fade * snowGroundDry(pos, waterLevel, 0.0);
}

// ---- THE DEFORMATION (SnowDeformPipeline, snow_deform.cs.glsl): the tracks, the berms, the craters ----
// The includer defines SNOW_DEFORM_READ and has u_textures[] (the maps' R16G16 views: R = the stamp's snowfall, G = the
// offset). The REPEAT sampler wraps like the toroidal slots; texel k's centre is lattice k + 0.5 (snow_deform.cs.glsl).
#ifdef SNOW_DEFORM_READ
// One texel: the offset from the undisturbed surface (m), a track (< 0) refilled by the snow fallen since its stamp.
float snowDeformDecode(float r, float g)
{
	float offset = g * SNOW_DEFORM_RANGE + SNOW_DEFORM_MIN;
	if (offset < 0.0)
		offset = min(0.0, offset + mod(u_snow_fallNow - round(r * 65535.0), 65536.0) * 0.001);
	return offset;
}
// One tap, bilinear AFTER the decode: the stamp channel is a clock (mod 65536, and 0 on undisturbed texels), so filtered
// in hardware a fresh track's edge would read as long refilled. Two gathers (R, G) of the bilinear footprint, taken at the
// footprint's SHARED CORNER: the shader picks the four texels (floor) and the weights, and at the corner the hardware's
// fixed-point rounding cannot pick another pair (at the texel edges it would read a texel off - spikes on a track wall).
// x = the offset, yz = its bilinear slope per lattice unit (dead code where unused).
vec3 snowDeformTapGrad(uint tex, vec2 lattice)
{
	const vec2 p = lattice - 0.5;
	const vec2 base = floor(p);
	const vec2 f = p - base;
	const vec2 uv = (base + 1.0) * (1.0 / float(SNOW_DEFORM_RES)); // RES a power of two: exact
	const vec4 r = textureGather(u_textures[tex], uv, 0);
	const vec4 g = textureGather(u_textures[tex], uv, 1);
	// Gather order: x = (i0, j1), y = (i1, j1), z = (i1, j0), w = (i0, j0).
	const float o00 = snowDeformDecode(r.w, g.w), o10 = snowDeformDecode(r.z, g.z);
	const float o01 = snowDeformDecode(r.x, g.x), o11 = snowDeformDecode(r.y, g.y);
	return vec3(mix(mix(o00, o10, f.x), mix(o01, o11, f.x), f.y), mix(o10 - o00, o11 - o01, f.y), mix(o01 - o00, o11 - o10, f.x));
}

// The raw offset at xz (m) and its slope along world X / Z: the FINE window inside it, the COARSE one outside (crossfaded
// over the fine window's outer 5 %), faded to 0 over the coarse window's outer 5 % (the fades' own slopes are left out).
// 0 without the maps.
vec3 snowDeformAtGrad(vec2 xz)
{
	if (u_snow_deformPresent < 0.5)
		return vec3(0.0);
	const vec2 lc = xz * (1.0 / SNOW_DEFORM_COARSE_TEXEL);
	const vec2 relC = (lc - u_snow_deformCoarseOrigin) * (1.0 / float(SNOW_DEFORM_RES)) - 0.5;
	const float inC = 1.0 - smoothstep(0.44, 0.49, max(abs(relC.x), abs(relC.y)));
	if (inC <= 0.0)
		return vec3(0.0);
	const vec2 lf = xz * (1.0 / SNOW_DEFORM_FINE_TEXEL);
	const vec2 relF = (lf - u_snow_deformFineOrigin) * (1.0 / float(SNOW_DEFORM_RES)) - 0.5;
	const float inF = 1.0 - smoothstep(0.44, 0.49, max(abs(relF.x), abs(relF.y)));
	vec3 d = vec3(0.0);
	if (inF < 1.0)
		d = snowDeformTapGrad(u_snow_deformCoarseTex, lc) * vec3(1.0, vec2(1.0 / SNOW_DEFORM_COARSE_TEXEL));
	if (inF > 0.0)
		d = mix(d, snowDeformTapGrad(u_snow_deformFineTex, lf) * vec3(1.0, vec2(1.0 / SNOW_DEFORM_FINE_TEXEL)), inF);
	return d * inC;
}
float snowDeformAt(vec2 xz) { return snowDeformAtGrad(xz).x; }

// The deformation DRAWN where the undisturbed snow is `lift` deep: a press never under the ground, a berm at most half the
// depth (none on bare ground).
float snowDeformApply(float offset, float lift)
{
	return clamp(offset, -lift, 0.5 * lift);
}

// THE GEOMETRY's distance fade (the VS's lift, the TES's residual): full within "Track detail range", none past twice it.
// Farther out the vertices are coarser than the maps, so a point-sampled track would show as dotted pits. A function of xz
// only (the centre view's distance), so the VS's control points, the TES's edges and both patches of an edge agree.
float snowDeformFade(vec2 xz)
{
	const float range = max(u_snow_trackDetailRange, 1e-3);
	return 1.0 - smoothstep(range, 2.0 * range, distance(xz, u_views_viewPos[VIEW_CENTER].xz));
}

// The drawn deformation's slope along world X / Z, for a normal (NOT faded: past the geometry's fade the tracks still shade).
// e = the tessellation's footprint: at or under a fine texel the analytic bilinear slope of one tap (0 where the clamp
// holds); coarser, forward differences over e, which low-pass the map instead of aliasing it.
// drawn = the drawn deformation at xz (m; < 0 in a track), from the same taps.
vec2 snowDeformGradient(vec2 xz, float e, float lift, out float drawn)
{
	drawn = 0.0;
	if (u_snow_deformPresent < 0.5 || lift <= 0.0)
		return vec2(0.0);
	if (e <= SNOW_DEFORM_FINE_TEXEL)
	{
		const vec3 d = snowDeformAtGrad(xz);
		drawn = snowDeformApply(d.x, lift);
		return d.x > -lift && d.x < 0.5 * lift ? d.yz : vec2(0.0);
	}
	const float d0 = snowDeformApply(snowDeformAt(xz), lift);
	drawn = d0;
	return vec2(snowDeformApply(snowDeformAt(xz + vec2(e, 0.0)), lift) - d0, snowDeformApply(snowDeformAt(xz + vec2(0.0, e)), lift) - d0) / e;
}

// THE TRACK'S OWN SUN SHADOW ("Weather/Snow/Track sun shadow"; the shadow map and the TLAS hold no lift): 4 steps toward
// the sun (u_snow_sunStep: its xz direction, its rise per metre) through the drawn deformation, 3 - 30 cm out. The
// undisturbed snow follows the terrain, so the terrain's slope cancels out of the offsets. Returns the sun's visibility on
// the track floor (faded out with the geometry); wallPath = the sun's path through the wall's top (m, the wall glow).
float snowTrackSunShadow(vec2 xz, float drawn0, float lift, out float wallPath)
{
	const float tAt[4] = float[](0.03, 0.08, 0.16, 0.30);
	float occ = -1.0;
	for (int i = 0; i < 4; ++i)
		occ = max(occ, snowDeformApply(snowDeformAt(xz + u_snow_sunStep.xy * tAt[i]), lift) - (drawn0 + tAt[i] * u_snow_sunStep.z));
	wallPath = max(occ, 0.0) * u_snow_sunStep.w;
	return 1.0 - smoothstep(0.0, 0.02, occ) * snowDeformFade(xz);
}
#endif

// ---- THE SNOW BRDF'S SETUP (LIT_SNOW: the lit core's snow sun BRDF, snow_brdf.inc.glsl): the share, the glints ----
// The includer defines LIT_SNOW before the lit core (instanced_indirect_lit.inc.glsl: the globals g_snowW, g_snowGlint,
// g_snowSunK) and calls snowLitSetup before computeLitColor - the object layer does it in snowApplyUp.
#ifdef LIT_SNOW
#ifndef INSTANCED_INDIRECT_LIT_INC_GLSL
#error "LIT_SNOW: include instanced_indirect_lit.inc.glsl before snow.inc.glsl"
#endif
#define SNOW_GLINT_LEVEL0 10 // the finest glint cell: 2^-10 m (~1 mm)

// A cell's facet count from a uniform u: RANDOMIZED ROUNDING, floor(lambda + u) - the mean is lambda exactly (so far away
// the glints still converge into the lobe), and for a sparse cell (lambda << 1) it is a Poisson draw's 0 or 1. Less
// variance than Poisson at lambda ~ 1 .. 16 (fewer doubled glints). A Poisson inverse CDF (a 5-step loop with exp per cell,
// 8 cells) cost the lit FS 16 B of spill.
float snowFacetCount(float u, float lambda)
{
	return floor(lambda + u);
}

// One lattice level: the facet count of the cell the point lies in over the expected count. Exact integer cells at any
// world distance: floor(xz) x 2^n + the cell inside the metre. The draw is keyed by the cell and the half vector's slope
// BIN (offset per cell, so the cells do not all change their facet set at once): a still view is a still pattern. The
// NEAREST cell, not the 4 around it blended: a cell is about a pixel (the level follows the footprint), and the blend's 8
// cells x 3 hashes cost the lit FS its spill; TAA / DLSS smooth a cell's edge.
float snowGlintLevel(vec2 xz, int l, float lambda, vec2 slope)
{
	const int n = SNOW_GLINT_LEVEL0 - l; // cells per metre = 2^n, n >= 1
	const ivec2 c = ivec2(floor(xz)) * (1 << n) + ivec2(fract(xz) * float(1 << n)); // * not <<: defined for negative xz
	const uint h = hashU((uint(c.x) * 1973u) ^ (uint(c.y) * 9277u) ^ (uint(l) * 26699u));
	const ivec2 b = ivec2(floor(slope + vec2(float(h & 0xFFFFu), float(h >> 16u)) * (1.0 / 65536.0)));
	const uint hu = hashU(h ^ (uint(b.x) * 73856093u) ^ (uint(b.y) * 19349663u));
	const float u = (float(hu >> 8u) + 0.5) * (1.0 / 16777216.0);
	return snowFacetCount(u, lambda) / lambda;
}

// THE GLINTS: X = the effective facet density over the smooth one at the sun's half vector (E[X] = 1, so far away the
// glints converge into the smooth lobe). lambda = the facets per cell whose normal reflects the sun into the eye:
// rho x cell area x D x NoH x the half vectors' solid angle (the sun's disc / 4 HoL + the facet cone). fp32 throughout (it
// goes down to ~1e-7). The cell level follows the pixel's footprint (two levels blended).
float snowGlint(vec2 xz, f16vec3 N, f16vec3 V, float16_t alpha, float rho, float footprint)
{
	const f16vec3 Lh = f16vec3(u_sunDirection.xyz);
	if (dot(N, Lh) <= float16_t(0.0))
		return 1.0;
	const f16vec3 H = halfVectorH(Lh, V);
	const float NoH = max(float(dot(N, H)), 0.0), HoL = max(float(dot(H, Lh)), 1e-3);
	const float a2 = float(alpha) * float(alpha), dd = NoH * NoH * (a2 - 1.0) + 1.0;
	const float p = (a2 / (PI * dd * dd)) * NoH * (u_snow_glintConeSun / HoL + u_snow_glintConeFacet);
	const float lod = max(log2(max(footprint, 1e-6)) + float(SNOW_GLINT_LEVEL0), 0.0);
	const float l0 = floor(lod);
	if (l0 >= float(SNOW_GLINT_LEVEL0 - 1))
		return 1.0;
	const float lambda0 = rho * exp2(2.0 * (l0 - float(SNOW_GLINT_LEVEL0))) * p;
	if (lambda0 > 16.0 || lambda0 <= 0.0)
		return 1.0; // converged (or no facets): no hashes
	const vec2 slope = vec2(H.xz) / max(float(H.y), 0.2) * u_snow_glintInvBin;
	const float t = lod - l0;
	const float x0 = snowGlintLevel(xz, int(l0), lambda0, slope);
	const float x1 = lambda0 * 4.0 <= 16.0 ? snowGlintLevel(xz, int(l0) + 1, lambda0 * 4.0, slope) : 1.0;
	return mix(mix(x0, x1, t), 1.0, smoothstep(8.0, 16.0, mix(lambda0, 4.0 * lambda0, t))); // no ring where the hashes stop
}

// THE SETUP, just before computeLitColor (the object layer: in snowApplyUp): the snow's share of the surface (after its
// blend), the smooth base normal's sun cosine (the diffusion's target) and the glints. glintRho = 0: none (a mover, a leaf).
// Plain ALU: any control flow.
// "Sparkle strength" scales the facets' whole lobe (the light the grains reflect inside them), near and far alike: the
// glints' X keeps its mean of 1, so the averaged snow does not change with the distance. Without "Sparkle" the plain lobe.
// THE SNOW'S DIFFUSE SUN (snow_brdf.inc.glsl snowSunDiffuseK) is formed here too, as one scalar (doSunLight multiplies it by
// the albedo and the sun). A relief flank turned from the sun takes it while its smooth base (baseN) faces it.
void snowLitSetup(vec2 anchorXZ, f16vec3 N, f16vec3 baseN, vec3 V, float16_t share, float16_t alpha, float glintRho, float footprint, f16vec3 albedo)
{
	g_snowW = share;
	if (share > float16_t(0.0))
	{
		const f16vec3 Lh = f16vec3(u_sunDirection.xyz), Vh = f16vec3(V);
		g_snowSunK = snowSunDiffuseK(dot(N, Lh), dot(baseN, Lh), max(dot(N, Vh), float16_t(0.0)), dot(Lh, Vh),
			dot(albedo, f16vec3(0.2126, 0.7152, 0.0722)));
	}
	if (u_snow_glintOn > 0.5 && share > float16_t(0.0))
	{
		float X = 1.0;
#ifndef SNOW_NO_GLINTS // the includer's surface never glints (the voxel crowns): the strength alone
		if (share > float16_t(0.004) && glintRho > 0.0 && g_sunShadowFirst > 0.0 && u_sunLit > 0.5)
			X = snowGlint(anchorXZ, N, f16vec3(V), alpha, glintRho, footprint);
#endif
		g_snowGlint = float16_t(min(u_snow_glintGain * X, 65504.0));
	}
}
#endif

// ---- THE OBJECT LAYER (snowApply): the snow on the up-facing surfaces of the lit objects ----
// The includer defines SNOW_OBJECTS and has the lit core's bindings: u_textures[], the terrain-data map
// (terrain_height.inc.glsl: the climate at the point) and the frame UBO (the splat's snow slot: the relief). SNOW_TEX(tex,
// uv, dx, dy) is the relief fetch: explicit gradients, taken before snowApply's per-pixel branch (default textureGrad; the
// voxel trees have no derivatives).
#ifdef SNOW_OBJECTS
#ifndef SNOW_TEX
#define SNOW_TEX(tex, uv, dx, dy) textureGrad(tex, uv, dx, dy)
#endif

// How much snow lies on an object's surface point (0..1): the climate's permanent snow or the pack - at the point's own
// height, on a face that holds it ("Terrain/Textures/Snow slope start / full" on upN, the NORMAL-MAPPED normal's y: snow
// fills the up-facing crevices first). shelter = the site catch (false for leaves: a crown is its own roof). cell = the
// pack's state there (snowPackCell; the fresh state while the pack is off), read only past the early-outs.
float snowObjectCover(vec3 pos, float upN, bool shelter, out vec4 cell)
{
	cell = vec4(u_snow_freshGrainM * 1e3, 0.0, 0.0, 0.0);
	if (u_snow_objects < 0.5 || u_terrain_hasSnow < 0.5 || u_terrain_splatBase < 0.0 || !terrainHeightMapPresent())
		return 0.0;
	const float holds = 1.0 - smoothstep(u_terrainTex_snowSlopeStart, u_terrainTex_snowSlopeFull, 1.0 - clamp(upN, 0.0, 1.0));
	if (holds <= 0.0)
		return 0.0;
	const vec4 climate = terrainClimateAt(pos.xz);
	const float tempC = terrainTemperatureAt(climate, pos.y);
	const float cold = 1.0 - smoothstep(u_terrainTex_snowTempFull, u_terrainTex_snowTempNone, tempC);
	const float permanent = cold * smoothstep(0.0, u_terrainTex_snowAridity, climate.w);
	if (permanent <= 0.0 && !snowPackLive())
		return 0.0;
	const float catchF = shelter ? snowSiteCatch(pos) : 1.0;
	if (snowPackLive())
		cell = snowPackCell(tempC, 1.0); // the open snow's state (snowStateAt), as every path reads it
	// The pack only out of the water and thinned by the shore (snowGroundDry, as on the ground and the rocks).
	const float dry = snowPackLive() ? snowGroundDry(pos, terrainDataAt(pos.xz).y, 0.0) : 0.0;
	// The permanent snow stops where the pack's catch does (a roof keeps both off; a crown thins neither by much). The slope
	// rule thins the pack's DEPTH, as on the ground and in the caps (a cap's rim then matches the layer on a steep roof).
	const float pack = snowPackLive() ? snowCoverFromDepth(snowPackDepth(tempC, catchF) * holds) : 0.0;
	return max(permanent * clamp(catchF * 3.0, 0.0, 1.0) * holds, pack * dry);
}

// The snow relief's tangent XY on a world-XZ projection (the splat's snow slot, its nor texture; [-1, 1], world X / Z):
// for snowNormalOn. The terrain and the caps read the same tap.
f16vec2 snowObjectRelief(vec2 xz, vec2 dPosX, vec2 dPosZ)
{
	const uint slot = uint(u_terrain_numGround + u_terrain_numRock) + (u_terrain_hasBeach > 0.5 ? 1u : 0u);
	const uvec4 v = u_terrain_splatTex[slot >> 1u];
	const uint tex = (slot & 1u) == 0u ? v.x : v.z;
	const float scale = u_terrainTex_uvScaleSnow;
	return f16vec2(SNOW_TEX(u_textures[tex >> 16u], xz * scale, dPosX * scale, dPosZ * scale).xy) * float16_t(2.0) - float16_t(1.0);
}

// The layer: cover the surface by snowObjectCover x scale - the snow's albedo (its state's) and its facets' roughness
// replace the surface's, its relief tilts the normal, and the metal goes. Call BEFORE computeLitColor: nothing new stays
// live across the light loop. Called in uniform control flow: the relief's gradients are taken here, before the per-pixel
// branch. upN = how much the surface faces up for the slope rule (snowApply: the normal-mapped N.y; the voxel crowns pass
// their own). LIT_SNOW: the snow BRDF's setup too - V the view vector, anchorXZ the glints' world point (a swaying
// branch's rest position), glints false for what moves (its glints would crawl) and the leaves.
void snowApplyUp(vec3 pos, float upN, inout f16vec3 N, inout f16vec3 albedo, inout float16_t rough, inout float16_t metal,
	bool shelter, float scale, vec3 V, vec2 anchorXZ, bool glints)
{
#ifdef SNOW_NO_DERIVATIVES
	const vec2 dPosX = vec2(0.0), dPosZ = vec2(0.0);
#else
	const vec2 dPosX = dFdx(pos.xz), dPosZ = dFdy(pos.xz);
#endif
	vec4 cell;
	const float cover = snowObjectCover(pos, upN, shelter, cell) * scale;
	if (cover <= 0.004)
		return;
	const SnowState st = snowStateFrom(cell);
	const float16_t c = float16_t(cover);
	const f16vec3 baseN = N;
	albedo = mix(albedo, f16vec3(snowAlbedo(st)), c);
	rough = mix(rough, float16_t(snowAlpha(st)), c);
	metal *= float16_t(1.0) - c;
	N = normalize(mix(N, snowNormalOn(N, snowObjectRelief(anchorXZ, dPosX, dPosZ)), c)); // the relief where the glints are
#ifdef LIT_SNOW
	snowLitSetup(anchorXZ, N, baseN, V, c, rough, glints ? snowGlintDensity(st) : 0.0, snowFootprint(dPosX, dPosZ), albedo);
#endif
}
void snowApply(vec3 pos, inout f16vec3 N, inout f16vec3 albedo, inout float16_t rough, inout float16_t metal, bool shelter, float scale,
	vec3 V, vec2 anchorXZ, bool glints)
{
	snowApplyUp(pos, float(N.y), N, albedo, rough, metal, shelter, scale, V, anchorXZ, glints);
}
#endif

#endif
