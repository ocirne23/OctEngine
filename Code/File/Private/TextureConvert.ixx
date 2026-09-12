export module File:TextureConvert;

import Core;

// Standalone image -> BC-compressed .dds conversion (same compressor the scene cooker uses for cooked
// scene textures), for loose textures that no ISceneData references - e.g. the procedural terrain's
// biome texture sets. Output mip chains stream through the TextureStreamer like any cooked .dds.
export namespace TextureConvert
{
	enum class EUsage : uint8
	{
		Color,     // sRGB content -> BC1 (BC3 when an alpha channel is used)
		NormalMap, // tangent normals -> BC5 (XY only; the material flags Z reconstruction)
		Data,      // linear data (roughness/AO/masks) -> BC1
	};

	// Converts srcPath (png/jpg/tga/...) into a full-mip-chain .dds at outPath. Returns false when the
	// source can't be decoded or the output can't be written.
	bool convertToDds(const char* srcPath, EUsage usage, const char* outPath);

	// Channel-packing variant: builds an RGB image from up to three GRAYSCALE sources (r required; g/b
	// nullptr = 0) and compresses it as Data/BC1. All present sources must share dimensions. For packing
	// separate AO / roughness / metalness maps into one ARM-style texture.
	bool convertPackedToDds(const char* srcPathR, const char* srcPathG, const char* srcPathB, const char* outPath);
}
