project(RendererVK)

file(GLOB_RECURSE PROJECT_PRIVATE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Private/*/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.ixx" "${CMAKE_CURRENT_SOURCE_DIR}/Private/*.cpp")
file(GLOB_RECURSE PROJECT_PUBLIC_FILES  "${CMAKE_CURRENT_SOURCE_DIR}/Public/*/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.ixx")
file(GLOB_RECURSE PROJECT_SHADER_FILES  "${ROOT_DIR}/Assets/Shaders/*.*")
set(EXTRA_MODULE_FILES "${ROOT_DIR}/Dependencies/Include/vulkan/vulkan.cppm")

add_library(${PROJECT_NAME} STATIC ${PROJECT_PRIVATE_FILES} ${PROJECT_PUBLIC_FILES})

target_sources(${PROJECT_NAME} PRIVATE FILE_SET modules BASE_DIRS "${ROOT_DIR}/Dependencies/include" TYPE CXX_MODULES FILES ${EXTRA_MODULE_FILES})
target_sources(${PROJECT_NAME} PRIVATE FILE_SET headers BASE_DIRS "${ROOT_DIR}/Assets/Shaders" TYPE HEADERS FILES ${PROJECT_SHADER_FILES})

source_group("Private/Shaders" FILES ${PROJECT_SHADER_FILES})
source_group("Private/Util" FILES ${EXTRA_MODULE_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Private")
target_include_directories(${PROJECT_NAME} PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/Public")

target_compile_definitions(${PROJECT_NAME} PRIVATE VULKAN_HPP_NO_SPACESHIP_OPERATOR VULKAN_HPP_NO_STRUCT_CONSTRUCTORS VULKAN_HPP_NO_EXCEPTIONS)
target_compile_definitions(${PROJECT_NAME} PRIVATE "VULKAN_HPP_ASSERT_ON_RESULT=__noop")

target_link_libraries(${PROJECT_NAME} PUBLIC Core Animation File Threading vulkan-1
	glslang$<$<CONFIG:Debug>:d> 
	MachineIndependent$<$<CONFIG:Debug>:d>
	OSDependent$<$<CONFIG:Debug>:d>
	GenericCodeGen$<$<CONFIG:Debug>:d>
	glslang-default-resource-limits$<$<CONFIG:Debug>:d>
	shaderc$<$<CONFIG:Debug>:d>
	SPIRV$<$<CONFIG:Debug>:d>
	SPIRV-Tools$<$<CONFIG:Debug>:d>
	SPIRV-Tools-opt$<$<CONFIG:Debug>:d>
	openxr_loader$<$<CONFIG:Debug>:d>
	meshoptimizer$<$<CONFIG:Debug>:d>
)

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER Engine)

apply_compile_settings(${PROJECT_NAME})