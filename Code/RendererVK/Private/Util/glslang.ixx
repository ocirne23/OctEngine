module;

#ifndef _MSC_VER
#include "LinuxGmfPrelude.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <memory>
#include <unordered_set>
#endif

export module RendererVK:glslang;

#ifdef _MSC_VER
#pragma warning(disable: 5260)
export import <glslang/Public/ResourceLimits.h>;
export import <glslang/Public/ShaderLang.h>;
export import <glslang/SPIRV/GlslangToSpv.h>;
#else
export extern "C++"
{
#include "glslang/Public/ResourceLimits.h" // quoted: no "angled include in the module purview" warning
#include "glslang/Public/ShaderLang.h"
#include "glslang/SPIRV/GlslangToSpv.h"
}
#endif
