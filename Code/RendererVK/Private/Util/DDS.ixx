module;

#ifndef _MSC_VER
#include "LinuxGmfPrelude.h"
#endif

export module RendererVK:DDS;

#ifdef _MSC_VER
export import <dds/dds.h>;
#else
export extern "C++"
{
#include "dds/dds.h" // quoted: no "angled include in the module purview" warning
}
#endif
