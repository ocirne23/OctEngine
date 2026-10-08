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
		Height,    // one linear channel (displacement / height, the source's R) -> BC4
		ColorAlpha, // sRGB RGB + a LINEAR data alpha (not coverage: no alpha weighting in the mips) -> BC3 always
		TwoChannel, // two linear data channels (R, G) -> BC5
	};

	// One output channel for convertChannelsToDds: channel srcChannel (0..3 = RGBA) of the image at path, or
	// the constant fill when path is nullptr. scale reduces / raises the source's contrast about mid grey
	// (128 + (v - 128) * scale): a height map's relief, with 0.5 = flat unchanged.
	struct PackChannel
	{
		const char* path = nullptr;
		uint8 srcChannel = 0;
		uint8 fill = 0;
		float scale = 1.0f;
	};

	// Converts srcPath (png/jpg/tga/...) into a full-mip-chain .dds at outPath. Returns false when the
	// source can't be decoded or the output can't be written.
	bool convertToDds(const char* srcPath, EUsage usage, const char* outPath);

	// Channel-packing variant: builds an RGB image from up to three GRAYSCALE sources (r required; g/b
	// nullptr = 0) and compresses it as Data/BC1. All present sources must share dimensions. For packing
	// separate AO / roughness / metalness maps into one ARM-style texture.
	bool convertPackedToDds(const char* srcPathR, const char* srcPathG, const char* srcPathB, const char* outPath);

	// General channel packing: output RGBA channel i = channels[i]. Each distinct source is decoded once; all
	// must share dimensions (else false, with a warning). usage picks the format (e.g. ColorAlpha: albedo +
	// roughness in one BC3; TwoChannel: height + AO in one BC5). At least one channel needs a path.
	bool convertChannelsToDds(const PackChannel (&channels)[4], EUsage usage, const char* outPath);

	// In-memory BC compression of RGBA8 images generated at runtime (the procedural trees). Pure, any thread; the
	// same stb_dxt compressor as above. BC1 = RGB, BC3 = RGBA, BC4 = R, BC5 = RG. Any size: edge texels repeat
	// into the partial blocks.
	enum class EBlockFormat : uint8 { BC1, BC3, BC4, BC5 };
	uint32 blockBytes(EBlockFormat format); // 8 (BC1, BC4) or 16
	size_t compressedSize(uint32 width, uint32 height, EBlockFormat format);
	// Block rows [rowBegin, rowEnd) of the image (each 4 texel rows) into `dst`, the start of the WHOLE image's output
	// (block row r at r x blocksX x blockBytes) - callers split the rows over jobs.
	void compressBlockRows(const uint8* rgba, uint32 width, uint32 height, EBlockFormat format, uint32 rowBegin, uint32 rowEnd, uint8* dst);
}
