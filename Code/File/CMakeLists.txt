project(File)

file(GLOB_RECURSE PROJECT_PRIVATE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp")
file(GLOB_RECURSE PROJECT_PUBLIC_FILES  "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.ixx")

add_library(${PROJECT_NAME} STATIC ${PROJECT_PRIVATE_FILES} ${PROJECT_PUBLIC_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")
target_include_directories(${PROJECT_NAME} PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/Public")

target_link_libraries(${PROJECT_NAME} PUBLIC Core Animation assimp-vc143-mt$<$<CONFIG:Debug>:d> zlibstatic$<$<CONFIG:Debug>:d> meshoptimizer$<$<CONFIG:Debug>:d>
    PRIVATE zstd$<$<CONFIG:Debug>:d>) # the .dat packs' optional compression

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER Engine)

apply_compile_settings(${PROJECT_NAME})