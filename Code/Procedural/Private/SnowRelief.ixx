export module Procedural:SnowRelief;

import Core;

// THE SNOW'S RELIEF (Docs/Context/RendererVK/Snow.md, "The look"): the terrain splat's snow slot is GENERATED, not a photo
// - a tileable height over one snow tile (10 m at the default "Terrain/Textures/Snow uv scale" 0.1): soft lumps, wind
// ripples with sharp crests, ridged sastrugi in wind-crust patches, grain and small pits. A spectral synthesis on the
// torus (integer cycles per tile: every band tiles), a fixed seed. Written once as PNG sources into Local/TerrainTex/Gen/,
// which the terrain texture bake (TerrainStreamer) turns into the slot's BC textures like any set. The snow's colour and
// gloss come from its state (snow.inc.glsl), so the albedo source is a flat placeholder.
export namespace Procedural::SnowRelief
{
	// The recipe's version is in the stem: a recipe change bumps it, so no stale bake is read.
	constexpr const char* STEM = "snow_relief_v1";
	constexpr const char* DIFF_PATH = "Local/TerrainTex/Gen/snow_relief_v1_diff.png";
	constexpr const char* NOR_PATH  = "Local/TerrainTex/Gen/snow_relief_v1_nor_gl.png";
	constexpr const char* DISP_PATH = "Local/TerrainTex/Gen/snow_relief_v1_disp.png";
	constexpr uint32 SIZE = 2048;        // texels over ONE snow tile (4.9 mm at 10 m)
	constexpr float TILE_METRES = 10.0f; // the tile at the default "Snow uv scale" (the normals' slope scale)
	constexpr float REF_DEPTH = 0.6f;    // m: the default "Terrain/Tessellation" ground depth the normals assume
	constexpr float HEIGHT_SCALE = 0.3f; // the splat entry's heightScale: the relief a few cm deep

	// Writes the three sources unless they all exist. False = a write failed. The texture bake thread only (seconds of CPU
	// once; the FFTs run as jobs).
	bool ensureSources(const oc::atomic<bool>& stopRequested);
}
