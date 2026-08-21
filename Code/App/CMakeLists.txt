project(App)

file(GLOB_RECURSE PROJECT_PRIVATE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(${PROJECT_NAME} ${PROJECT_PRIVATE_FILES})

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER Apps)
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/ignore:4099")

target_link_libraries(${PROJECT_NAME} PRIVATE Core Animation RendererVK UI Entity File Input Script Physics Network Audio Spatial Threading Procedural Particle Force Game AppScripts)

# Cooked build: AppScripts' whole job is the scripts' static-init registrations (ocRegisterScriptEntry). Nothing
# references its symbols, so a normal static-lib link dead-strips the entire object and NO script registers.
# /WHOLEARCHIVE forces it in so the initializers run. (No-op when AppScripts is the INTERFACE stub, hence the guard.)
if(SCRIPTS_STATIC)
	target_link_options(${PROJECT_NAME} PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:AppScripts>")
endif()

apply_compile_settings(${PROJECT_NAME})

# onnxruntime.dll imports DirectML.dll by name, so both must sit next to the exe: the AddDllDirectory
# in FileSystem::initialize only covers LoadLibraryEx(LOAD_LIBRARY_SEARCH_USER_DIRS), not load-time imports.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/Dependencies/Dll/GFSDK_Aftermath_Lib.x64.dll"
        "${CMAKE_SOURCE_DIR}/Dependencies/Dll/onnxruntime.dll"
        "${CMAKE_SOURCE_DIR}/Dependencies/Dll/DirectML.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>")

# Launch App with networking args from Visual Studio: set run-server / run-client as the Startup
# Project and F5 (the run-cooked pattern — VS runs the debugger command below, and building the
# target just builds App; the target's own build step is a no-op so nothing launches twice). Run
# both instances by starting one, then right-click the other target -> Debug -> Start New Instance.
add_custom_target(run-server COMMAND ${CMAKE_COMMAND} -E true VERBATIM)
add_dependencies(run-server App)
set_target_properties(run-server PROPERTIES
	FOLDER Apps
	EXCLUDE_FROM_ALL TRUE
	VS_DEBUGGER_COMMAND "$<TARGET_FILE:App>"
	VS_DEBUGGER_COMMAND_ARGUMENTS "--game --server"
	VS_DEBUGGER_WORKING_DIRECTORY "${ROOT_DIR}")

add_custom_target(run-client COMMAND ${CMAKE_COMMAND} -E true VERBATIM)
add_dependencies(run-client App)
set_target_properties(run-client PROPERTIES
	FOLDER Apps
	EXCLUDE_FROM_ALL TRUE
	VS_DEBUGGER_COMMAND "$<TARGET_FILE:App>"
	VS_DEBUGGER_COMMAND_ARGUMENTS "--game --connect 127.0.0.1"
	VS_DEBUGGER_WORKING_DIRECTORY "${ROOT_DIR}")

add_custom_target(run-headless COMMAND ${CMAKE_COMMAND} -E true VERBATIM)
add_dependencies(run-headless App)
set_target_properties(run-headless PROPERTIES
	FOLDER Apps
	EXCLUDE_FROM_ALL TRUE
	VS_DEBUGGER_COMMAND "$<TARGET_FILE:App>"
	VS_DEBUGGER_COMMAND_ARGUMENTS "--server --headless"
	VS_DEBUGGER_WORKING_DIRECTORY "${ROOT_DIR}")

add_custom_target(run-game COMMAND ${CMAKE_COMMAND} -E true VERBATIM)
add_dependencies(run-game App)
set_target_properties(run-game PROPERTIES
	FOLDER Apps
	EXCLUDE_FROM_ALL TRUE
	VS_DEBUGGER_COMMAND "$<TARGET_FILE:App>"
	VS_DEBUGGER_COMMAND_ARGUMENTS "--game"
	VS_DEBUGGER_WORKING_DIRECTORY "${ROOT_DIR}")

add_custom_target(run-scenario COMMAND ${CMAKE_COMMAND} -E true VERBATIM)
add_dependencies(run-scenario App)
set_target_properties(run-scenario PROPERTIES
	FOLDER Apps
	EXCLUDE_FROM_ALL TRUE
	VS_DEBUGGER_COMMAND "$<TARGET_FILE:App>"
	VS_DEBUGGER_COMMAND_ARGUMENTS "--game --server --scenario Scenarios/march-64-units.txt --tweaks Scenarios/cpu-profile.tweaks" 
	VS_DEBUGGER_WORKING_DIRECTORY "${ROOT_DIR}")