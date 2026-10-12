// The part of the terrain fragment shaders that the ground (instanced_indirect_terrain.fs.glsl) and the surface-water
// film (terrain_film.fs.glsl) share: the wetness, the mirror sky, the splat and the baked fields.
// Include it AFTER instanced_indirect_lit.inc.glsl and the in_pos / in_terrainFields inputs.

// Terrain wetness clipmap (written by terrain_wetness.cs.glsl): the decaying memory of where water touched the ground.
#define TERRAIN_WET_BINDING 18
#include "terrain_wetness.inc.glsl"
#include "reflection_fog.inc.glsl"

// Sky for the film's reflection: the same mirror-sky bake the ocean fetches, so the film reflects the same sky as the
// water next to it. No sun disc: the GGX glint is its reflection.
vec3 terrainReflectedSkyRadiance(vec3 dir)
{
	return textureLod(u_skyMap, vec3(skyMapUV(dir), SKY_MAP_LAYER_MIRROR), 0.0).rgb;
}

// RELIEF (the height blend + the parallax march) here only: it needs screen derivatives.
#define TERRAIN_SPLAT_RELIEF
#include "terrain_splat.inc.glsl"

// The baked terrain fields, evaluated PER VERTEX (instanced_indirect_terrain.vs.glsl: band-limited far below any
// LOD's vertex lattice, so interpolation is exact) and read from the in_terrainFields interpolant.
TerrainFields terrainFields()
{
	TerrainFields f;
	f.altitude = in_terrainFields.x;
	f.temperature = in_terrainFields.y;
	f.humidity = in_terrainFields.z;
	f.waterLevel = in_terrainFields.w;
	f.river = in_river; // the mesh vertex's u, interpolated
	return f;
}

// THE WETNESS AT THIS PIXEL, after the slope drain, plus the gate that keeps water off ground the OCEAN draws.
// wet: the smooth field, highest where water stood most recently; the darkening, gloss, relief water level and film
// all derive from it.
// aboveLive: 1 -> 0 over the first "Ocean blend (m)" of LIVE water over the ground, so the wet gloss and the film never
// show deep in the ocean's water (a second sky reflection under the first) and fade out before the depth test would
// cut them hard. groundBelow = how far in_pos sits above the ground (0 for the ground, the film's height for the film).
// Also 0 for the whole frame while the CAMERA is under water (the ocean's underside draws what it mirrors); the
// camera's side is the particle draw's gate: the CPU live wave height under the camera, else the calm level here.
void terrainWetness(vec3 coverN, TerrainFields fields, float groundBelow, out float16_t wet, out float16_t aboveLive)
{
	// Hand the already-sampled water level to the lit core so its underwater test does not re-fetch the cascade, then
	// resolve that test NOW (doSunLight would, but the gloss below needs it first).
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

	// Slope drain: steep ground dries faster. The stored wetness decays as exp(-t / tau), so wet^k IS a k-times faster
	// decay, per pixel on the exact mesh normal (the map's 8 m texels cannot see a cliff). k = 1 + slope * drain.
	const float16_t slope = one - float16_t(clamp(coverN.y, 0.0, 1.0));
	wet = pow(wet, one + slope * float16_t(u_terrainWater_slopeDrain));
}
