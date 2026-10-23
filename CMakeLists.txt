cmake_minimum_required(VERSION 3.25)
project(fear_nx VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT REXGLUE_PLATFORM_SWITCH)
    message(FATAL_ERROR "Configure with tools/switch/toolchain.cmake for the Horizon ARM64 target.")
endif()
option(REXGLUE_SWITCH_COMPILE_ONLY "Compile ARM64 objects while the Vulkan driver is being built" OFF)
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rexglue-sdk" CACHE PATH "Pinned ReXGlue 0.10 source")
set(FEAR_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/default" CACHE PATH "Locally translated original F.E.A.R. code")
set(FEAR_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH "Directory containing the local original F.E.A.R. assets")
get_filename_component(FEAR_GENERATED_INCLUDE_ROOT "${FEAR_GENERATED_DIR}/../.." ABSOLUTE)
set(FEAR_NX_RENDERER "DEKO3D" CACHE STRING "Switch renderer backend (DEKO3D or VULKAN)")
set_property(CACHE FEAR_NX_RENDERER PROPERTY STRINGS DEKO3D VULKAN)
string(TOUPPER "${FEAR_NX_RENDERER}" FEAR_NX_RENDERER)
if(NOT FEAR_NX_RENDERER STREQUAL "DEKO3D" AND NOT FEAR_NX_RENDERER STREQUAL "VULKAN")
    message(FATAL_ERROR "FEAR_NX_RENDERER must be DEKO3D or VULKAN")
endif()
if(FEAR_NX_RENDERER STREQUAL "DEKO3D")
    set(REXGLUE_USE_DEKO3D ON CACHE BOOL "Deko3D graphics backend" FORCE)
    set(REXGLUE_USE_VULKAN OFF CACHE BOOL "Vulkan graphics backend" FORCE)
else()
    set(REXGLUE_USE_DEKO3D OFF CACHE BOOL "Deko3D graphics backend" FORCE)
    set(REXGLUE_USE_VULKAN ON CACHE BOOL "Vulkan graphics backend" FORCE)
endif()
if(FEAR_NX_RENDERER STREQUAL "DEKO3D")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    option(FEAR_DEKO_SKIP_SETUP "Use already prepared pinned SDK without rerunning setup" OFF)
    if(NOT FEAR_DEKO_SKIP_SETUP)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch/prepare_deko3d.py"
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}" --sdk-dir "${REXSDK_DIR}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE FEAR_DEKO_SETUP_RESULT)
    if(NOT FEAR_DEKO_SETUP_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not prepare the pinned Deko3D dependencies.")
    endif()
    endif()
endif()
if(NOT EXISTS "${FEAR_GENERATED_DIR}/sources.cmake")
    message(FATAL_ERROR "Generate F.E.A.R. with the pinned ReXGlue 0.10 tool before cross-compilation.")
endif()
file(SHA256 "${FEAR_ASSET_DIR}/default.xex" FEAR_XEX_SHA256)
if(NOT FEAR_XEX_SHA256 STREQUAL "e7fccdf6dd57bb58d10efbdd0b2ca567b8cece5b6a3fc696c7fca879aaea2792")
    message(FATAL_ERROR "The executable does not match the validated F.E.A.R. revision.")
endif()
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
include("${FEAR_GENERATED_DIR}/sources.cmake")
if(FEAR_NX_RENDERER STREQUAL "DEKO3D")
    set(FEAR_CPU_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/fear-cpu")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch/prepare_cpu_adapter.py"
        --source "${FEAR_GENERATED_DIR}" --output "${FEAR_CPU_GENERATED}" COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch/prepare_cpu_adapter.py")
    file(GLOB FEAR_CPU_INPUTS CONFIGURE_DEPENDS "${FEAR_GENERATED_DIR}/fear_xbox360_recomp.*.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FEAR_CPU_INPUTS})
    foreach(FEAR_SOURCE IN LISTS GENERATED_SOURCES)
        get_filename_component(FEAR_FILENAME "${FEAR_SOURCE}" NAME)
        if(FEAR_FILENAME MATCHES "^fear_xbox360_recomp[.][0-9]+[.]cpp$")
            list(REMOVE_ITEM GENERATED_SOURCES "${FEAR_SOURCE}")
            list(APPEND GENERATED_SOURCES "${FEAR_CPU_GENERATED}/${FEAR_FILENAME}")
        endif()
    endforeach()
endif()

# This generated function contains ~95,000 registration calls and runs once at
# startup. Optimizing that single giant function costs disproportionate time
# in GCC; keep gameplay translation units fully optimized.
set_source_files_properties("${FEAR_GENERATED_DIR}/fear_xbox360_register.cpp"
    PROPERTIES COMPILE_OPTIONS "-O0" SKIP_PRECOMPILE_HEADERS ON)

add_library(fear_nx_recomp OBJECT ${GENERATED_SOURCES})
target_include_directories(fear_nx_recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${FEAR_GENERATED_DIR}" "${FEAR_GENERATED_INCLUDE_ROOT}")
target_link_libraries(fear_nx_recomp PRIVATE rex::runtime)
target_precompile_headers(fear_nx_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/fear_nx_pch.h")
if(FEAR_NX_RENDERER STREQUAL "DEKO3D")
    target_compile_definitions(fear_nx_recomp PRIVATE FEAR_NX_CPU_ADAPTER=1)
endif()
# Guest-memory fault recovery must preserve the original register semantics.
target_compile_options(fear_nx_recomp PRIVATE
    -ffp-contract=off -fno-tree-slp-vectorize -fno-tree-vectorize)

set(FEAR_HOST_SOURCES
    src/main.cpp
    src/input/gyro_input.cpp
    src/native/native_system.cpp)
if(FEAR_NX_RENDERER STREQUAL "DEKO3D")
    list(APPEND FEAR_HOST_SOURCES
        src/cpu/fear_cpu.cpp
        src/deko/deko_renderer.cpp
        src/deko/deko_textures.cpp
        src/deko/deko_shaders.cpp
        src/deko/deko_shader_cache.cpp
        src/deko/deko_shader_warmup.cpp
        src/deko/deko_geometry_shaders.cpp)
else()
    list(APPEND FEAR_HOST_SOURCES
        src/native/native_renderer.cpp
        src/native/native_textures.cpp
        src/native/native_geometry_shaders.cpp)
endif()
if(REXGLUE_SWITCH_COMPILE_ONLY)
    add_library(fear_nx OBJECT ${FEAR_HOST_SOURCES})
else()
    add_executable(fear_nx ${FEAR_HOST_SOURCES})
endif()
target_include_directories(fear_nx PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${FEAR_GENERATED_DIR}" "${FEAR_GENERATED_INCLUDE_ROOT}"
    "${REXSDK_DIR}/thirdparty/imgui"
    "${REXSDK_DIR}/src/graphics"
    "${REXSDK_DIR}/thirdparty/glslang"
    "${REXSDK_DIR}/thirdparty/xxHash")
if(FEAR_NX_RENDERER STREQUAL "DEKO3D")
    target_compile_definitions(fear_nx PRIVATE FEAR_NX_RENDERER_DEKO3D=1)
    target_compile_definitions(fear_nx PRIVATE FEAR_NX_CPU_ADAPTER=1)
    target_link_options(fear_nx PRIVATE "-Wl,--wrap=_ZN3rex6thread10MaybeYieldEv")
    target_include_directories(fear_nx PRIVATE
        "${REXSDK_DIR}/thirdparty/vulkan-headers/include"
        "${REXSDK_DIR}/thirdparty/uam"
        "${REXSDK_DIR}/thirdparty/spirv-cross"
        "${REXSDK_DIR}/thirdparty/spirv-cross/include")
else()
    target_compile_definitions(fear_nx PRIVATE FEAR_NX_RENDERER_VULKAN=1)
endif()
target_link_libraries(fear_nx PRIVATE fear_nx_recomp)
rexglue_switch_configure_target(fear_nx GPU_PLUGINS xenos)
if(FEAR_NX_RENDERER STREQUAL "DEKO3D")
    # libdeko3d depends on libnx services; the existing helper links deko3d,
    # while the Switch toolchain does not add nx through its specs.
    target_link_libraries(fear_nx PRIVATE nx)
endif()
if(REXGLUE_SWITCH_COMPILE_ONLY)
    # OBJECT targets have no link step to pull in their library dependencies.
    add_dependencies(fear_nx fear_nx_recomp rexruntime rexgpu-xenos rexglue_switch_startup)
else()
    # The SDK links these by -l name, which does not tell Ninja when an
    # externally rebuilt driver changes. Always relink and repackage the NRO
    # after staging a new driver archive.
    if(FEAR_NX_RENDERER STREQUAL "VULKAN" AND REXGLUE_SWITCH_NVK_SDK)
        set_property(TARGET fear_nx APPEND PROPERTY LINK_DEPENDS
            "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a"
            "${REXGLUE_SWITCH_NVK_SDK}/lib/libnak_rs.a")
    endif()
    set(FEAR_NX_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.jpg")
    set_property(TARGET fear_nx APPEND PROPERTY LINK_DEPENDS "${FEAR_NX_ICON}")
    if(FEAR_NX_RENDERER STREQUAL "DEKO3D")
        set(FEAR_NX_PACKAGE_NAME "fear_nx_deko3d")
        set(FEAR_NX_PACKAGE_TITLE "F.E.A.R. NX deko3d")
        if(REXGLUE_SWITCH_DEKO3D_DEBUG)
            set(FEAR_NX_PACKAGE_NAME "fear_nx_deko3d_diag")
            set(FEAR_NX_PACKAGE_TITLE "F.E.A.R. NX deko3d diagnostic")
        endif()
    else()
        set(FEAR_NX_PACKAGE_NAME "fear_nx")
        set(FEAR_NX_PACKAGE_TITLE "F.E.A.R. NX")
    endif()
    rexglue_switch_add_nro(fear_nx NAME "${FEAR_NX_PACKAGE_TITLE}" AUTHOR "fear-nx contributors"
        VERSION "${PROJECT_VERSION}" ICON "${FEAR_NX_ICON}" OUTPUT_NAME "${FEAR_NX_PACKAGE_NAME}")
endif()
