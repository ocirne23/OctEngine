project(Script)

file(GLOB_RECURSE PROJECT_PRIVATE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp")
file(GLOB_RECURSE PROJECT_PUBLIC_FILES  "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.ixx")

add_library(${PROJECT_NAME} STATIC ${PROJECT_PRIVATE_FILES} ${PROJECT_PUBLIC_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")
target_include_directories(${PROJECT_NAME} PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/Public")

# File: ScriptHost reads/writes .dsl + generated .cpp and scans Assets/Local/Scripts — since Core
# stopped exporting <filesystem>/<fstream>, ALL disk access goes through File's FileSystem.
target_link_libraries(${PROJECT_NAME} PUBLIC Core File)

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER Engine)

apply_compile_settings(${PROJECT_NAME})

# Scripts #include "ScriptAPI.h" and are compiled at runtime from the Assets working dir; ScriptHost
# points the compiler's /I straight at Code/Script/Public (the single source of truth), so no copy is kept.
