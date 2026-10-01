export module File:ImageIO;

import Core;

// Image files for GENERATED content (procedural textures written out once, read back on later runs).
// stb encodes / decodes in memory; the bytes go through FileSystem - the disk seam - so the same thread
// rule applies (allowMainThread). Assets the renderer loads directly do not come through here.
export namespace ImageIO
{
	// RGBA8, rows top to bottom.
	bool writePngRgba8(const oc::string& path, uint32 width, uint32 height, oc::span<const uint8> rgba, bool allowMainThread = false);
	// Any stb-decodable image (png/jpg/tga/...) as RGBA8, rows top to bottom. False when missing or undecodable.
	bool readImageRgba8(const oc::string& path, uint32& outWidth, uint32& outHeight, oc::vector<uint8>& outRgba, bool allowMainThread = false);
}
