project(UI)

file(GLOB_RECURSE PROJECT_PRIVATE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp")
file(GLOB_RECURSE PROJECT_PUBLIC_FILES  "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.ixx")

add_library(${PROJECT_NAME} STATIC ${PROJECT_PRIVATE_FILES} ${PROJECT_PUBLIC_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")
target_include_directories(${PROJECT_NAME} PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/Public")
if(NOT MSVC) # imgui-node-editor's `#include "imgui.h"`: MSVC finds it next to an already-open imgui include
	target_include_directories(${PROJECT_NAME} PRIVATE "${ROOT_DIR}/Dependencies/Include/imgui")
	# a plain TU: std::terminate reaches it through MSVC's headers, not libc++'s
	set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Private/lib/imgui-node-editor/crude_json.cpp" PROPERTIES COMPILE_OPTIONS "-include;exception")
	# The vendored imgui-node-editor is third-party code: its own warnings (std::aligned_storage, which the Windows build
	# silences with _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING above, and an unused local) are not ours.
	file(GLOB NODE_EDITOR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Private/lib/imgui-node-editor/*.cpp")
	set_property(SOURCE ${NODE_EDITOR_SOURCES} APPEND PROPERTY COMPILE_OPTIONS "-Wno-deprecated-declarations;-Wno-unused-but-set-variable")
endif()

target_link_libraries(${PROJECT_NAME} PUBLIC Core RendererVK Entity Script Settings imgui$<$<CONFIG:Debug>:d>)

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER Engine)

target_compile_definitions(${PROJECT_NAME} PRIVATE _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING) # std::aligned_storage and std::aligned_storage_t are deprecated in C++23.

apply_compile_settings(${PROJECT_NAME})