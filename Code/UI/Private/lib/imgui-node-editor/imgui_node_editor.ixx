module;

#include "ImguiGmf.h"

export module UI:imgui_node_editor;

#ifdef _MSC_VER
export import <lib/imgui-node-editor/imgui_node_editor.h>;
#else
export extern "C++"
{
#include "imgui_node_editor.h" // quoted, next to this file: no "angled include in the module purview" warning
}
#endif
export namespace ed = ax::NodeEditor;
