module;

#include "ImguiGmf.h"

export module Core.imgui;

import Core;

#ifdef _MSC_VER
export import <imgui/imgui.h>;
export import <imgui/imgui_internal.h>;
export import <imgui/imgui_impl_sdl3.h>;
export import <imgui/imgui_impl_vulkan.h>;
#endif

export void imgui_check_vk_result(VkResult err)
{
    if (err == 0)
        return;
    printf("[vulkan] Error: imgui VkResult = %d\n", err);
    if (err < 0)
        __debugbreak();
}
