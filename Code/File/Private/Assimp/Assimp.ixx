module;

#include "AssimpGmf.h"

export module File:Assimp;

#ifdef _MSC_VER
export import <assimp/Importer.hpp>;
export import <assimp/IOStream.hpp>; // the FileSystem IO handler (SceneData.cpp): imports read through the packs too
export import <assimp/IOSystem.hpp>;
export import <assimp/postprocess.h>;
export import <assimp/scene.h>;
#endif
