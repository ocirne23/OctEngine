project(Spatial)

file(GLOB_RECURSE PROJECT_PRIVATE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp")
file(GLOB_RECURSE PROJECT_PUBLIC_FILES  "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.ixx")

add_library(${PROJECT_NAME} STATIC ${PROJECT_PRIVATE_FILES} ${PROJECT_PUBLIC_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")
target_include_directories(${PROJECT_NAME} PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/Public")

target_link_libraries(${PROJECT_NAME} PUBLIC Core Threading)

# Finest cell size (meters) is compile-time so all quantization math constant-folds.
# Override here if a world needs a different extent or density: 8m cells span +/-8389km.
# target_compile_definitions(${PROJECT_NAME} PRIVATE SPATIAL_FINEST_CELL_SIZE=8.0)

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER Engine)

apply_compile_settings(${PROJECT_NAME})
