# Linux (clang + libc++ + Ninja) build settings and dependencies. Included from the root CMakeLists.txt only when the
# compiler is not MSVC; the Windows build never reads this file.
#
# Configure inside the flake's dev shell (`nix develop`), which sets the OC_* variables read below:
#   cmake -B Build-Linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build Build-Linux --target App
#
# The library CMakeLists.txt files link the Windows .lib names (EASTL$<$<CONFIG:Debug>:d>, vulkan-1, Ws2_32, ...). Every
# such name is defined here as a target, mapped to its Linux library, or to an empty INTERFACE target for the Windows
# system libraries. A library that is not in nixpkgs, or whose vendored headers are pinned to one upstream revision, is
# built here from that revision (flake inputs) against the vendored headers in Dependencies/Include.

if(NOT CMAKE_GENERATOR MATCHES "Ninja")
	message(FATAL_ERROR "The Linux build needs a Ninja generator (C++ modules): cmake -G Ninja")
endif()

enable_language(C) # box3d is C

foreach(var OC_EASTL_SRC OC_IMGUI_SRC OC_BOX3D_SRC OC_MESHOPTIMIZER_SRC OC_PHONON_LIB OC_LIBCXX_MODULES OC_NIX_INCLUDES)
	if("$ENV{${var}}" STREQUAL "")
		message(FATAL_ERROR "${var} is not set: configure from inside `nix develop`")
	endif()
	set(${var} "$ENV{${var}}")
endforeach()

# project() picks clang-scan-deps from PATH. It must be the flake's wrapped one (clang-tools over libcxxClang): the
# unwrapped scanner does not see the libc++ / glibc include paths that the clang wrapper adds.
if(NOT "$ENV{OC_CLANG_SCAN_DEPS}" STREQUAL "" AND NOT CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS STREQUAL "$ENV{OC_CLANG_SCAN_DEPS}")
	message(WARNING "clang-scan-deps is ${CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS}, not the flake's $ENV{OC_CLANG_SCAN_DEPS}: "
		"delete the build folder and configure again from inside `nix develop`")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

# The MSVC flags, translated:
#   /arch:AVX2       -> -march=x86-64-v3 (AVX2 + BMI1/2 + LZCNT + FMA: Core.OcBit treats TZCNT/LZCNT as baseline)
#   /fp:fast         -> the -ffast-math parts that MSVC's /fp:fast also has. NOT -ffinite-math-only: /fp:fast keeps
#                       inf/NaN semantics, and the engine tests for them.
#   /FI              -> command-line macros (OC_FORCEINCLUDE_DEFINES below). clang rejects ANY -include in front of
#                       `module;` / a module declaration, even a header that holds only macros.
#   /GS-             -> -fno-stack-protector
#   /Zc:threadSafeInit- -> -fno-threadsafe-statics (the same standing rule: see .claude/CLAUDE.md)
#   /Gw              -> -fdata-sections -ffunction-sections + --gc-sections
#   /GT              -> no flag. clang may cache a TLS address across a fiber switch; Threading reads its worker context
#                       through a non-inlined accessor on Linux instead (see Threading).
#   /Zc:tlsGuards-   -> no flag; clang has no equivalent switch (the guards stay).
set(COMPILE_OPTIONS
	"-march=x86-64-v3"
	"-fno-math-errno" "-fno-trapping-math" "-fno-signed-zeros" "-freciprocal-math" "-fassociative-math" "-ffp-contract=fast"
	"-fno-stack-protector"
	"-fno-threadsafe-statics"
	"-fdata-sections" "-ffunction-sections"
	"-Wall" "-Wextra"
	"-Wno-unused-parameter"            # /wd4100
	"-Wno-unknown-pragmas"             # #pragma warning / init_seg / comment
	"-Wno-reserved-module-identifier"
	"-Wno-missing-field-initializers"
	"-Wno-nan-infinity-disabled"
	"-Wno-deprecated-declarations"
	"-Wno-sign-compare"
	"-Wno-unused-private-field"
	"-Wno-unused-function"
	"-Wno-unused-variable"
	"-Wno-unused-but-set-variable"
	"-Wno-include-angled-in-module-purview"
	"-Wno-microsoft-anon-tag"
	"-Wno-invalid-offsetof"
	"-Wno-c99-designator"
	"-Wno-nullability-completeness"
	"-Wno-deprecated-literal-operator" # /wd5311
)
set(LINK_OPTIONS "-fuse-ld=lld" "-Wl,--gc-sections" "-pthread")

# ---- What Code/Core/Private/forceinclude.h gives every Windows TU ----
# assert with no #include: (void)sizeof in non-Debug, exactly like forceinclude.h; a trap in Debug. A TU that includes
# <cassert> itself gets glibc's assert from there on.
#
# OC_INIT_SEG expands to NOTHING: no init_seg on ELF. Construction order is the C++ module initializer order instead -
# every engine global is defined in a module interface, and a module's initializer runs the initializers of the modules
# it imports first, so each global is constructed after (and destructed before) every global it can reach through an
# import. That matches InitSeg.h's order wherever one global's dtor uses another; see "Globals and teardown" in
# .claude/CLAUDE.md.
list(APPEND COMPILE_OPTIONS
	"-DOC_INIT_SEG(seg)="
	"$<IF:$<CONFIG:Debug>,-Dassert=[](bool oc_ok){ if (!oc_ok) __builtin_debugtrap(); },-Dassert=(void)sizeof>")

# ---- import std (libc++'s own std.cppm / std.compat.cppm) ----
add_library(oc_std STATIC)
target_sources(oc_std PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${OC_LIBCXX_MODULES}"
	FILES "${OC_LIBCXX_MODULES}/std.cppm" "${OC_LIBCXX_MODULES}/std.compat.cppm")
target_include_directories(oc_std SYSTEM PRIVATE "${OC_LIBCXX_MODULES}")
target_compile_options(oc_std PRIVATE "-march=x86-64-v3" "-fno-math-errno" "-fno-trapping-math" "-fno-signed-zeros"
	"-freciprocal-math" "-fassociative-math" "-ffp-contract=fast" "-fno-threadsafe-statics" "-Wno-reserved-module-identifier"
	"-Wno-reserved-user-defined-literal")
set_target_properties(oc_std PROPERTIES FOLDER "_Deps" CXX_SCAN_FOR_MODULES ON)

# ---- Prebuilt-on-Windows libraries, built from their pinned upstream revision ----
function(oc_dep_settings target)
	target_compile_options(${target} PRIVATE "-march=x86-64-v3" "-w")
	set_target_properties(${target} PROPERTIES FOLDER "_Deps" CXX_SCAN_FOR_MODULES OFF)
endfunction()

file(GLOB OC_EASTL_SOURCES "${OC_EASTL_SRC}/source/*.cpp")
add_library(oc_eastl STATIC ${OC_EASTL_SOURCES})
target_include_directories(oc_eastl PUBLIC "${ROOT_DIR}/Dependencies/Include")
oc_dep_settings(oc_eastl)

add_library(oc_imgui STATIC
	"${OC_IMGUI_SRC}/imgui.cpp" "${OC_IMGUI_SRC}/imgui_draw.cpp" "${OC_IMGUI_SRC}/imgui_tables.cpp" "${OC_IMGUI_SRC}/imgui_widgets.cpp"
	"${OC_IMGUI_SRC}/backends/imgui_impl_null.cpp" "${OC_IMGUI_SRC}/backends/imgui_impl_sdl3.cpp" "${OC_IMGUI_SRC}/backends/imgui_impl_vulkan.cpp")
target_include_directories(oc_imgui PRIVATE ${OC_NIX_INCLUDES} "${ROOT_DIR}/Dependencies/Include/imgui" "${ROOT_DIR}/Dependencies/Include")
target_link_libraries(oc_imgui PUBLIC "-lSDL3" "-lvulkan")
oc_dep_settings(oc_imgui)

file(GLOB OC_BOX3D_SOURCES "${OC_BOX3D_SRC}/src/*.c")
add_library(oc_box3d STATIC ${OC_BOX3D_SOURCES})
target_include_directories(oc_box3d PRIVATE "${OC_BOX3D_SRC}/include" "${OC_BOX3D_SRC}/src")
set_target_properties(oc_box3d PROPERTIES C_STANDARD 17)
target_link_libraries(oc_box3d PUBLIC m)
oc_dep_settings(oc_box3d)

file(GLOB OC_MESHOPT_SOURCES "${OC_MESHOPTIMIZER_SRC}/src/*.cpp")
add_library(oc_meshoptimizer STATIC ${OC_MESHOPT_SOURCES})
oc_dep_settings(oc_meshoptimizer)

# ---- The Windows link names ----
function(oc_alias name)
	foreach(n ${name} ${name}d)
		if(NOT TARGET ${n})
			add_library(${n} INTERFACE)
			target_link_libraries(${n} INTERFACE ${ARGN})
		endif()
	endforeach()
endfunction()

# "-l<name>" and not the bare name: a bare name equal to the target's own name would link the target to itself.
oc_alias(EASTL oc_eastl)
oc_alias(imgui oc_imgui)
oc_alias(box3d oc_box3d)
oc_alias(meshoptimizer oc_meshoptimizer)
oc_alias(SDL3-static "-lSDL3")
oc_alias(vulkan-1 "-lvulkan")
oc_alias(assimp-vc143-mt "-lassimp")
oc_alias(zlibstatic "-lz")
oc_alias(zstd "-lzstd")
oc_alias(openxr_loader "-lopenxr_loader")
oc_alias(phonon "${OC_PHONON_LIB}/libphonon.so")
oc_alias(onnxruntime "-lonnxruntime")
oc_alias(glslang "-lglslang")
oc_alias(SPIRV "-lSPIRV")
oc_alias(glslang-default-resource-limits "-lglslang-default-resource-limits")
oc_alias(SPIRV-Tools "-lSPIRV-Tools")
oc_alias(SPIRV-Tools-opt "-lSPIRV-Tools-opt" "-lSPIRV-Tools")
oc_alias(shaderc "-lshaderc_shared")
oc_alias(MachineIndependent) # merged into libglslang on Linux
oc_alias(OSDependent)
oc_alias(GenericCodeGen)
foreach(winlib ntdll Winmm Setupapi Cfgmgr32 Version Imm32 Ws2_32 Bcrypt wintrust crypt32)
	oc_alias(${winlib})
endforeach()
