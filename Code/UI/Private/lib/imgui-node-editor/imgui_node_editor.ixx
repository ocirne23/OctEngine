module;

#include "ImguiGmf.h"

export module UI:imgui_node_editor;

#ifdef _MSC_VER
export import <lib/imgui-node-editor/imgui_node_editor.h>;
#else
export extern "C++"
{
#include <lib/imgui-node-editor/imgui_node_editor.h>
}
#endif
export namespace ed = ax::NodeEditor;
