// The part of the terrain fragment shaders that the ground (instanced_indirect_terrain.fs.glsl) and the
// surface-water film (terrain_film.fs.glsl) share: the wetness, the mirror sky, the splat and the baked fields.
// Include it AFTER instanced_indirect_lit.inc.glsl and the in_pos / in_terrainFields inputs.

// Terrain wetness clipmap (binding 18, written by terrain_wetness.cs.glsl): the decaying memory of
// where water touched the ground - the swash tongue, rain. Applied on top of the splat in main().
#define TERRAIN_WET_BINDING 18
#include "terrain_wetness.inc.glsl"
#include "reflection_fog.inc.glsl"

// Sky for the film's reflection - the same per-frame bake of mirrorSkyRadiance the ocean's
// reflectedSkyRadiance fetches, so the film reflects the same sky the water next to it does. No sun
// disc: the GGX glint is its reflection.
vec3 terrainReflectedSkyRadiance(vec3 dir)
{
	return textureLod(u_skyMap, vec3(skyMapUV(dir), SKY_MAP_LAYER_MIRROR), 0.0).rgb;
}

// The splat itself (TerrainFields / TerrainSample / terrainSplat) is terrain_splat.inc.glsl - shared with
// the ocean shader, which evaluates it at refraction-ray hits so the seabed IS this terrain.
// RELIEF: the height blend + the parallax march, here only (it needs screen derivatives). All of it runs
// before computeLitColor and nothing of it but the shifted texture position outlives the splat.
#define TERRAIN_SPLAT_RELIEF
#include "terrain_splat.inc.glsl"

// Baked terrain fields at one point (terrain-data cascades), mild-climate fallbacks without a map.
// altitude is the MACRO band: height far above it = mountain crag; height ~ altitude = flatland.
// Evaluated PER VERTEX in instanced_indirect_terrain.vs.glsl (every field is band-limited far below any
// LOD's vertex lattice, and temperature is linear in height, so interpolation is exact - see the comment
// there) and read from the in_terrainFields interpolant. This dropped ~6-10 texel fetches per pixel.
TerrainFields terrainFields()
{
	TerrainFields f;
	f.altitude = in_terrainFields.x;
	f.temperature = in_terrainFields.y;
	f.humidity = in_terrainFields.z;
	f.waterLevel = in_terrainFields.w;
	return f;
}

// THE WETNESS AT THIS PIXEL, after the slope drain, plus the gate that keeps water off ground the OCEAN
// itself is drawing.
// wet: the smooth field (bilinear + diffusion), highest where water stood most recently. Everything else -
// the ground's darkening and gloss, the water LEVEL in the relief, the film's surface - derives from it.
// aboveLive: fades from 1 to 0 over the first "Ocean blend (m)" of LIVE water over the GROUND (the same
// live surface that switches the caustics on). The wet gloss and the film then never show deep in the
// ocean's own water (a second sky reflection under the first). The film lies over the ocean's shallow edge
// and fades into it gradually, and the fade is complete before its surface sinks under the ocean's. There,
// the depth test cut it with a hard edge. groundBelow = how far the tested point (in_pos) sits above the
// ground under it: 0 for the ground itself, the film's height over the relief for the film.
// Also 0 for the whole frame while the CAMERA is under water: a submerged viewer never sees water mirrored
// off standing water (the ocean's own underside draws what it mirrors). The camera's side is the particle
// draw's gate: the live wave height under the camera (the CPU mirror, u_weather_cameraWaterY), else the calm
// level here, else sea level.
void terrainWetness(vec3 coverN, TerrainFields fields, float groundBelow, out float16_t wet, out float16_t aboveLive)
{
	// We already sampled the terrain data cascade for fields.waterLevel; hand it to the lit core so its
	// underwater test reuses it instead of re-fetching the same cascade - then resolve that test NOW
	// (doSunLight would, but the gloss below needs it first; it runs once per pixel either way).
	g_waterLevelOverride = fields.waterLevel;
	resolveLiveDepth(in_pos);
	const float16_t one = float16_t(1.0);
	const float waterAtCamera = u_weather_cameraWaterValid > 0.5 ? u_weather_cameraWaterY : fields.waterLevel;
	aboveLive = float16_t(u_viewPos.y < waterAtCamera ? 0.0
		: 1.0 - smoothstep(0.0, u_terrainWater_oceanBlend, g_liveDepthBelow + groundBelow));
	wet = float16_t(0.0);
	if (!terrainWetPresent())
		return;
	wet = float16_t(terrainWetnessAt(in_pos.xz));
	// Slope drain: water runs off a face instead of soaking in, so steep ground dries faster. The stored
	// wetness decays as exp(-t / tau), so wet^k IS a k-times faster decay - evaluated here per pixel against
	// the exact mesh normal (the map's 8 m texels cannot see a cliff face), with no extra state.
	// k = 1 + slope * drain: at drain 4 a 45-degree face dries ~2.2x faster, a wall 5x.
	const float16_t slope = one - float16_t(clamp(coverN.y, 0.0, 1.0));
	wet = pow(wet, one + slope * float16_t(u_terrainWater_slopeDrain));
}
