module;

// STATIC stb implementations (internal linkage), like SceneCooker.cpp: RendererVK owns the external-linkage
// stb_image, and every library links into the same executable.
#pragma warning(push, 0)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>
#pragma warning(pop)

module File;

import Core;

import :FileSystem;
import :ImageIO;

namespace ImageIO
{
	bool writePngRgba8(const oc::string& path, uint32 width, uint32 height, oc::span<const uint8> rgba, bool allowMainThread)
	{
		if (width == 0 || height == 0 || rgba.size() < (size_t)width * height * 4)
			return false;
		oc::vector<uint8> encoded;
		auto sink = [](void* context, void* data, int size)
		{
			oc::vector<uint8>& out = *static_cast<oc::vector<uint8>*>(context);
			out.insert(out.end(), static_cast<const uint8*>(data), static_cast<const uint8*>(data) + size);
		};
		if (!stbi_write_png_to_func(sink, &encoded, (int)width, (int)height, 4, rgba.data(), (int)width * 4))
			return false;
		return FileSystem::writeFileBytes(path, encoded, allowMainThread);
	}

	bool readImageRgba8(const oc::string& path, uint32& outWidth, uint32& outHeight, oc::vector<uint8>& outRgba, bool allowMainThread)
	{
		oc::vector<uint8> bytes;
		if (!FileSystem::readFileBytes(path, bytes, allowMainThread) || bytes.empty())
			return false;
		int w = 0, h = 0, channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &channels, 4);
		if (!pixels)
			return false;
		outWidth = (uint32)w;
		outHeight = (uint32)h;
		outRgba.assign(pixels, pixels + (size_t)w * h * 4);
		stbi_image_free(pixels);
		return true;
	}
}
