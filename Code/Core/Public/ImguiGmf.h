#pragma once

// Linux (clang) only: Dear ImGui as a textual global-module-fragment include. On MSVC `import Core.imgui;` exports the
// imgui header units. clang cannot re-export imgui from a module purview (imgui.h has out-of-class member definitions,
// which an export block rejects), so every module unit that imports Core.imgui also includes this in its GMF:
//     module;
//     #include "ImguiGmf.h"
#ifndef _MSC_VER
#include "LinuxGmfPrelude.h"
#include <vulkan/vulkan.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <imgui/imgui_impl_sdl3.h>
#include <imgui/imgui_impl_vulkan.h>
#endif
