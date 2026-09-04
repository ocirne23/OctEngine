project(DslCompiler)

file(GLOB_RECURSE PROJECT_PRIVATE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(${PROJECT_NAME} ${PROJECT_PRIVATE_FILES})

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER _Build)
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/ignore:4099")

# Entity is what carries registerScriptDslBindings (the whole DSL vocabulary), and its public link surface
# pulls in everything it depends on (RendererVK, File, Script, Physics, ...). Nothing gets initialized at
# runtime -- the tool only needs the registration call and Script's loader/transpiler. See CONTEXT.md.
target_link_libraries(${PROJECT_NAME} PRIVATE Core Entity Script File)

apply_compile_settings(${PROJECT_NAME})
