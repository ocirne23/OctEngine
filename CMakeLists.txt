cmake_minimum_required (VERSION 3.14)

project(OctEngine CXX)

set(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}")

include(Utils.cmake)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_Build")

# /GT (fiber-safe TLS): Threading's job fibers migrate between worker threads across a wait, so
# thread_local addresses must never be cached across calls anywhere job code can run.
list(APPEND COMPILE_OPTIONS "/std:c++latest" "/Zc:__cplusplus" "/fp:fast" "/arch:AVX2" "/GT" "/MP" "$<$<NOT:$<CONFIG:DEBUG>>:/GL>" "/Oi" "/Ot" "/W4" "$<$<NOT:$<CONFIG:DEBUG>>:/Ob2>" "$<$<CONFIG:DEBUG>:/JMC>" "$<$<CONFIG:Debug>:/ZI>")
list(APPEND LINK_OPTIONS "$<$<NOT:$<CONFIG:DEBUG>>:/LTCG>" "$<$<NOT:$<CONFIG:DEBUG>>:/INCREMENTAL:NO>" "$<$<CONFIG:Debug>:/INCREMENTAL>")

add_definitions(/FI"${ROOT_DIR}/Code/Core/Private/forceinclude.h"
	/wd4100 # unreferenced formal parameter
	/wd5260 # the constant variable 'ai_epsilon' has internal linkage in an included header file context, but external linkage in imported header unit context
	/wd5105 # macro expansion producing 'defined' has undefined behavior
	/wd4324 # structure was padded due to alignment specifier - deliberate cache-line alignment in Threading, re-fires in every importer
	/wd5311 # deprecated literal-operator-id form in EASTL's string.h (third-party), re-fires in every importer of Core
)

# Cooked/shipping build: compile the visual scripts straight INTO the engine (the App-Scripts target) so their
# ctx->* calls are whole-program-optimized and inlined against the engine thunks (non-Debug already builds /GL +
# /LTCG above), instead of the default runtime-compiled, hot-reloadable script DLLs. OFF for normal dev. NOTE:
# the .scr files must be regenerated once after enabling (the codegen wraps each script in a self-registering
# class) — this is a deliberate one-time shipping step, not a hot path.
option(SCRIPTS_STATIC "Compile visual scripts statically into the engine (cooked build) instead of runtime DLLs" OFF)
if(SCRIPTS_STATIC)
	add_definitions(/DSCRIPTS_STATIC)
endif()

add_subdirectory(Code)

# One-command cooked build: the static-vs-DLL choice is configure-time (it changes which sources App-Scripts
# compiles), so a cooked build wants its own tree. This `cooked` target drives that from the normal tree —
# it configures + builds a sibling Build-Cooked/ with SCRIPTS_STATIC=ON, leaving this tree on hot-reload script
# DLLs. Build it on demand (it's EXCLUDE_FROM_ALL): `cmake --build Build --target cooked`, or pick "cooked" in the
# IDE. Output: Build-Cooked/Code/App/<Config>/App.exe. Only offered in the default tree (not recursively in the
# cooked one).
if(NOT SCRIPTS_STATIC)
	add_custom_target(cooked
		COMMAND ${CMAKE_COMMAND} -S "${ROOT_DIR}" -B "${ROOT_DIR}/Build-Cooked" -G "${CMAKE_GENERATOR}" -DSCRIPTS_STATIC=ON
		COMMAND ${CMAKE_COMMAND} --build "${ROOT_DIR}/Build-Cooked" --config $<CONFIG> --target App
		VERBATIM USES_TERMINAL
		COMMENT "Cooked build: configuring + building the statically-linked-scripts App in Build-Cooked/")
	set_target_properties(cooked PROPERTIES FOLDER "_Build" EXCLUDE_FROM_ALL TRUE)

	# Launch the cooked App from Visual Studio: set `run-cooked` as the Startup Project and hit F5 / Ctrl+F5. VS
	# runs the debugger command below (the cooked App.exe), and building it first cooks via the `cooked` dependency
	# (incremental — a no-op once Build-Cooked/ is up to date). Its own build step is a no-op so nothing launches
	# twice. From the command line, use `cmake --build Build --target cooked` then run the exe under Build-Cooked/.
	add_custom_target(run-cooked COMMAND ${CMAKE_COMMAND} -E true VERBATIM)
	add_dependencies(run-cooked cooked)
	set_target_properties(run-cooked PROPERTIES
		FOLDER "_Build"
		EXCLUDE_FROM_ALL TRUE
		VS_DEBUGGER_COMMAND "${ROOT_DIR}/Build-Cooked/Code/App/$<CONFIG>/App.exe"
		VS_DEBUGGER_WORKING_DIRECTORY "${ROOT_DIR}")
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT "App")