project(ScriptEditorSim)

# The Script Editor's OWN source files, compiled a second time into this exe under a one-line stand-in for the UI module
# (SimUIModule.ixx), so an edit to them rebuilds two translation units and a link: nothing of the UI library, which
# imports the renderer, is compiled here. Entity carries registerScriptDslBindings (the whole DSL vocabulary), linked and
# never initialized, as in DslCompiler. See Docs/Context/ScriptEditorSim/CONTEXT.md.
file(GLOB PROJECT_PRIVATE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB EDITOR_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../UI/Private/ScriptEditor/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/../../UI/Private/ScriptEditor/*.cpp")

# App's failure handling, for the same reason: nobody is at the screen, so an assert must print instead of opening a box.
add_executable(${PROJECT_NAME} ${PROJECT_PRIVATE_FILES} ${EDITOR_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/../../App/UnattendedRun.ixx")

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER Tools)
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/ignore:4099")

target_link_libraries(${PROJECT_NAME} PRIVATE Core Entity Script File imgui$<$<CONFIG:Debug>:d>)

apply_compile_settings(${PROJECT_NAME})
