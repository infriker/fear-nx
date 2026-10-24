cmake_minimum_required(VERSION 3.25)
project(fear_nx VERSION 0.1.2 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT REXGLUE_PLATFORM_SWITCH)
    message(FATAL_ERROR "Configure with tools/switch/toolchain.cmake for the Horizon ARM64 target.")
endif()
option(REXGLUE_SWITCH_COMPILE_ONLY "Compile ARM64 objects while the Vulkan driver is being built" OFF)
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rexglue-sdk" CACHE PATH "Pinned ReXGlue 0.10 source")
set(FEAR_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/default" CACHE PATH "Locally translated original F.E.A.R. code")
if(NOT EXISTS "${FEAR_GENERATED_DIR}/sources.cmake")
    message(FATAL_ERROR "Generate F.E.A.R. with the pinned ReXGlue 0.10 tool before cross-compilation.")
endif()
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xex" FEAR_XEX_SHA256)
if(NOT FEAR_XEX_SHA256 STREQUAL "e7fccdf6dd57bb58d10efbdd0b2ca567b8cece5b6a3fc696c7fca879aaea2792")
    message(FATAL_ERROR "The executable does not match the validated F.E.A.R. revision.")
endif()
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
include("${FEAR_GENERATED_DIR}/sources.cmake")

# This generated function contains ~95,000 registration calls and runs once at
# startup. Optimizing that single giant function costs disproportionate time
# in GCC; keep gameplay translation units fully optimized.
set_source_files_properties("${FEAR_GENERATED_DIR}/fear_xbox360_register.cpp"
    PROPERTIES COMPILE_OPTIONS "-O0" SKIP_PRECOMPILE_HEADERS ON)

add_library(fear_nx_recomp OBJECT ${GENERATED_SOURCES})
target_include_directories(fear_nx_recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${FEAR_GENERATED_DIR}")
target_link_libraries(fear_nx_recomp PRIVATE rex::runtime)
target_precompile_headers(fear_nx_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/fear_nx_pch.h")
# Guest-memory fault recovery must preserve the original register semantics.
target_compile_options(fear_nx_recomp PRIVATE
    -ffp-contract=off -fno-tree-slp-vectorize -fno-tree-vectorize)

set(FEAR_HOST_SOURCES
    src/main.cpp
    src/input/gyro_input.cpp
    src/game/reflections.cpp
    src/game/quality.cpp
    src/native/native_system.cpp
    src/native/native_renderer.cpp
    src/native/native_textures.cpp
    src/native/native_geometry_shaders.cpp)
if(REXGLUE_SWITCH_COMPILE_ONLY)
    add_library(fear_nx OBJECT ${FEAR_HOST_SOURCES})
else()
    add_executable(fear_nx ${FEAR_HOST_SOURCES})
endif()
target_include_directories(fear_nx PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${FEAR_GENERATED_DIR}"
    "${REXSDK_DIR}/thirdparty/imgui"
    "${REXSDK_DIR}/src/graphics"
    "${REXSDK_DIR}/thirdparty/glslang"
    "${REXSDK_DIR}/thirdparty/xxHash")
target_link_libraries(fear_nx PRIVATE fear_nx_recomp)
rexglue_switch_configure_target(fear_nx GPU_PLUGINS xenos)
if(REXGLUE_SWITCH_COMPILE_ONLY)
    # OBJECT targets have no link step to pull in their library dependencies.
    add_dependencies(fear_nx fear_nx_recomp rexruntime rexgpu-xenos rexglue_switch_startup)
else()
    # The SDK links these by -l name, which does not tell Ninja when an
    # externally rebuilt driver changes. Always relink and repackage the NRO
    # after staging a new driver archive.
    if(REXGLUE_SWITCH_NVK_SDK)
        set_property(TARGET fear_nx APPEND PROPERTY LINK_DEPENDS
            "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a"
            "${REXGLUE_SWITCH_NVK_SDK}/lib/libnak_rs.a")
    endif()
    set(FEAR_NX_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.jpg")
    set_property(TARGET fear_nx APPEND PROPERTY LINK_DEPENDS "${FEAR_NX_ICON}")
    rexglue_switch_add_nro(fear_nx NAME "F.E.A.R. NX" AUTHOR "fear-nx contributors"
        VERSION "${PROJECT_VERSION}" ICON "${FEAR_NX_ICON}")
endif()
