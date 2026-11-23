module;

#ifndef _MSC_VER
#include "LinuxGmfPrelude.h"
#endif

export module Core.glm;

#ifdef _MSC_VER
export import <glm/glm.hpp>;
export import <glm/gtc/matrix_transform.hpp>;
export import <glm/gtc/quaternion.hpp>;
export import <glm/gtc/random.hpp>;
#else
import std.compat;
export extern "C++"
{
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/random.hpp>
}
#endif
