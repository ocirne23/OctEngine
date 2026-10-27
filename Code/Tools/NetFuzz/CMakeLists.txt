project(NetFuzz)

file(GLOB_RECURSE PROJECT_PRIVATE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(${PROJECT_NAME} ${PROJECT_PRIVATE_FILES})

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER _Build)
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/ignore:4099")

# Core + Network only: the point is a headless target that stands up the protocol stack in seconds.
# Reaching the GAME-layer parsers (NetworkManager) does not need Entity linked here - the `game` mode
# drives them through a real socket against a running `App --server --headless`.
target_link_libraries(${PROJECT_NAME} PRIVATE Core Network)

apply_compile_settings(${PROJECT_NAME})

# Run from Visual Studio: set as Startup Project and F5.
add_custom_target(run-netfuzz COMMAND ${CMAKE_COMMAND} -E true VERBATIM)
add_dependencies(run-netfuzz NetFuzz)
set_target_properties(run-netfuzz PROPERTIES
	FOLDER _Build
	EXCLUDE_FROM_ALL TRUE
	VS_DEBUGGER_COMMAND "$<TARGET_FILE:NetFuzz>"
	VS_DEBUGGER_COMMAND_ARGUMENTS "all"
	VS_DEBUGGER_WORKING_DIRECTORY "${ROOT_DIR}")
