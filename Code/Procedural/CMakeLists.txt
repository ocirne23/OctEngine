project(Procedural)

file(GLOB_RECURSE PROJECT_PRIVATE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp")
file(GLOB_RECURSE PROJECT_PUBLIC_FILES  "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.ixx")

add_library(${PROJECT_NAME} STATIC ${PROJECT_PRIVATE_FILES} ${PROJECT_PUBLIC_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")
target_include_directories(${PROJECT_NAME} PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/Public")

# Procedural sits above the renderer: its generation partitions are Core-only, but TerrainStreamer bridges
# generated chunks into renderable ObjectContainer/RenderNode handles, so the lib links RendererVK + File;
# TerrainCollider bridges the same height field into static PhysicsBody/PhysicsMesh tiles, so it links
# Physics too (Core-only itself; box3d never leaks through it).
# onnxruntime (V3/Diffusion) is PRIVATE and never leaks into the public surface, like box3d in Physics; it
# ships release-CRT only, so no $<$<CONFIG:Debug>:d> suffix (the Aftermath precedent, not the box3d one).
# zstd (the diffusion disk tile cache) is PRIVATE for the same reason, with the usual d-suffix pair.
# meshoptimizer (the rocks' base mesh simplification, RockGenerator.cpp) is PRIVATE too.
target_link_libraries(${PROJECT_NAME} PUBLIC Core RendererVK File Spatial Physics PRIVATE onnxruntime zstd$<$<CONFIG:Debug>:d> meshoptimizer$<$<CONFIG:Debug>:d>)

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER Engine)

apply_compile_settings(${PROJECT_NAME})

# The diffusion pipeline reproduces a reference implementation whose float behaviour is load-bearing: the
# blend guard `w > 1e-6f` is exactly the tent window's corner weight (eps^2), EDMScheduler deliberately mixes
# double logs with a float sigma ratio, and LaplacianUtils leans on double accumulators against catastrophic
# cancellation. The global /fp:fast (root CMakeLists) would reassociate all of that away, so opt these TUs
# out. Later flag wins in MSVC, so this overrides the inherited /fp:fast.
# This covers the .cpp files ONLY, deliberately. Two reasons:
#  - It forces the invariant that all FP-sensitive math lives in the .cpp and the Diffusion .ixx files carry
#    declarations only. Anything defined in an .ixx could be inlined into a /fp:fast consumer (more so under
#    /GL+/LTCG) and would silently lose /fp:precise anyway. Keep the math out of the .ixx.
#  - MSVC stamps the FP mode into the BMI and raises C5050 when a module is imported by a TU built with a
#    different mode. Leaving the .ixx on the default /fp:fast keeps them compatible with Core and with their
#    /fp:fast importers (e.g. TerrainStreamer importing :GeneratorV3), so the mismatch stays contained to
#    the .cpp files below, where /wd5050 acknowledges it: these TUs import /fp:fast modules on purpose, and
#    none of the inline code they pull in is FP-critical.
file(GLOB_RECURSE DIFFUSION_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Private/Diffusion/*.cpp")
set_source_files_properties(${DIFFUSION_SOURCES} PROPERTIES COMPILE_OPTIONS "/fp:precise;/wd5050")
