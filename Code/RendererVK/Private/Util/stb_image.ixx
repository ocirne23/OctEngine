module;

#ifndef _MSC_VER
#include "LinuxGmfPrelude.h"
#endif

export module RendererVK:stb_image;

#ifdef _MSC_VER
export import <stb/stb_image.h>;
#else
export extern "C++"
{
#include <stb/stb_image.h>
}
#endif

export using stbi_uc = unsigned char;
