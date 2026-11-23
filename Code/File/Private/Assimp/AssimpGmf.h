#pragma once

// Linux (clang) only: assimp as a textual global-module-fragment include. On MSVC `import :Assimp;` exports the assimp
// header units. clang cannot re-export assimp from a module purview (its .inl files define members out of class, which
// an export block rejects), so every module unit that imports :Assimp also includes this in its GMF.
#ifndef _MSC_VER
#include "LinuxGmfPrelude.h"
#include <assimp/Importer.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif
