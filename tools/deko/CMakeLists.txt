cmake_minimum_required(VERSION 3.25)
project(fear_deko_shader_smoke LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    # The SDK's host memory helpers use SSSE3 byte shuffles but expect the
    # architecture flag normally supplied by its developer build presets.
    add_compile_options(-mssse3)
endif()

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/rexglue-sdk" CACHE PATH
    "Pinned ReXGlue source tree")
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "" FORCE)
set(REXGLUE_USE_DEKO3D OFF CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
# The host plugin archive has references to the SDK performance counter API;
# compile its implementation so the standalone executable can load the real
# translator library.
set(REXGLUE_ENABLE_PERF_COUNTERS ON CACHE BOOL "" FORCE)
set(REXGLUE_ENABLE_FIDELITYFX OFF CACHE BOOL "" FORCE)
set(REXGLUE_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# The SDK's top-level CMake configures its Linux UI target even though this
# tool only builds the shader translator/runtime. Some Switch build images omit
# X11 and Wayland development packages. Supply empty pkg-config metadata only
# for those missing configure-time checks; rexui is not a dependency of this
# executable and is never built.
if(UNIX AND NOT APPLE)
    find_program(DEKO_SMOKE_PKG_CONFIG_EXECUTABLE pkg-config REQUIRED)
    set(DEKO_SMOKE_PKGCONFIG_DIR "${CMAKE_BINARY_DIR}/pkgconfig-configure-only")
    set(DEKO_SMOKE_HEADER_SHIM_DIR "${CMAKE_BINARY_DIR}/include-shims")
    file(MAKE_DIRECTORY "${DEKO_SMOKE_PKGCONFIG_DIR}")
    set(DEKO_SMOKE_HAS_PKGCONFIG_STUBS FALSE)
    set(DEKO_SMOKE_NEED_XCB_SHIM FALSE)
    set(DEKO_SMOKE_NEED_WAYLAND_SHIM FALSE)
    foreach(_pkg x11-xcb wayland-client)
        execute_process(
            COMMAND "${DEKO_SMOKE_PKG_CONFIG_EXECUTABLE}" --exists "${_pkg}"
            RESULT_VARIABLE _pkg_exists
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT _pkg_exists EQUAL 0)
            file(WRITE "${DEKO_SMOKE_PKGCONFIG_DIR}/${_pkg}.pc"
                "prefix=/usr\nName: ${_pkg}\nDescription: configure-only stub for unused SDK UI target\nVersion: 99.0\nLibs:\nCflags:\n")
            set(DEKO_SMOKE_HAS_PKGCONFIG_STUBS TRUE)
            if(_pkg STREQUAL "x11-xcb")
                set(DEKO_SMOKE_NEED_XCB_SHIM TRUE)
            else()
                set(DEKO_SMOKE_NEED_WAYLAND_SHIM TRUE)
            endif()
        endif()
    endforeach()
    if(DEKO_SMOKE_HAS_PKGCONFIG_STUBS)
        set(ENV{PKG_CONFIG_PATH}
            "${DEKO_SMOKE_PKGCONFIG_DIR}:$ENV{PKG_CONFIG_PATH}")
    endif()
    if(DEKO_SMOKE_NEED_XCB_SHIM OR DEKO_SMOKE_NEED_WAYLAND_SHIM)
        file(MAKE_DIRECTORY "${DEKO_SMOKE_HEADER_SHIM_DIR}/xcb")
        file(MAKE_DIRECTORY "${DEKO_SMOKE_HEADER_SHIM_DIR}/X11")
        if(DEKO_SMOKE_NEED_XCB_SHIM)
            file(WRITE "${DEKO_SMOKE_HEADER_SHIM_DIR}/xcb/xcb.h"
                "#pragma once\n#include <stdint.h>\ntypedef struct xcb_connection_t xcb_connection_t;\ntypedef uint32_t xcb_window_t;\ntypedef uint32_t xcb_visualid_t;\ntypedef struct { uint32_t sequence; } xcb_get_geometry_cookie_t;\ntypedef struct { uint8_t response_type, depth; uint16_t sequence; uint32_t length; xcb_window_t root; int16_t x, y; uint16_t width, height, border_width; uint8_t pad[2]; } xcb_get_geometry_reply_t;\nstatic inline xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t*, xcb_window_t) { return {0}; }\nstatic inline xcb_get_geometry_reply_t* xcb_get_geometry_reply(xcb_connection_t*, xcb_get_geometry_cookie_t, void*) { return nullptr; }\n")
            file(WRITE "${DEKO_SMOKE_HEADER_SHIM_DIR}/X11/Xlib.h"
                "#pragma once\ntypedef struct _XDisplay Display;\n")
            file(WRITE "${DEKO_SMOKE_HEADER_SHIM_DIR}/X11/Xlib-xcb.h"
                "#pragma once\n#include <X11/Xlib.h>\n#include <xcb/xcb.h>\nstatic inline xcb_connection_t* XGetXCBConnection(Display*) { return nullptr; }\n")
        endif()
        if(DEKO_SMOKE_NEED_WAYLAND_SHIM)
            file(WRITE "${DEKO_SMOKE_HEADER_SHIM_DIR}/wayland-client.h"
                "#pragma once\nstruct wl_display;\nstruct wl_surface;\n")
        endif()
        include_directories(BEFORE SYSTEM "${DEKO_SMOKE_HEADER_SHIM_DIR}")
    endif()
endif()

# Reuse the exact SDK Xenos SPIR-V translator and its real runtime support.
# Vulkan is selected only because the SDK's host CMake requires one backend;
# this tool never creates a Vulkan instance or device.
add_subdirectory("${REXSDK_DIR}" "${CMAKE_BINARY_DIR}/rexglue-sdk")

# The production plugin hides C++ symbols because it is loaded through its C
# entry points. This offline-only executable directly exercises its translator.
set_target_properties(rexgpu-xenos PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF)

set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${REXSDK_DIR}/thirdparty/spirv-cross" "${CMAKE_BINARY_DIR}/spirv-cross")
target_include_directories(spirv-cross-glsl PUBLIC
    "${REXSDK_DIR}/thirdparty/spirv-cross/include")
target_include_directories(spirv-cross-core PRIVATE
    "${REXSDK_DIR}/thirdparty/spirv-headers/include/spirv/unified1")

add_subdirectory("${REXSDK_DIR}/thirdparty/uam" "${CMAKE_BINARY_DIR}/uam")

add_executable(deko_shader_smoke
    shader_smoke.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deko/deko_shaders.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deko/deko_geometry_shaders.cpp")
target_include_directories(deko_shader_smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${REXSDK_DIR}/include"
    "${REXSDK_DIR}/thirdparty/glslang"
    "${REXSDK_DIR}/thirdparty/renderdoc"
    "${REXSDK_DIR}/thirdparty/xxHash"
    "${REXSDK_DIR}/thirdparty/vulkan-headers/include")
target_compile_definitions(deko_shader_smoke PRIVATE
    FEAR_NX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(deko_shader_smoke PRIVATE
    rexgpu-xenos
    rexruntime
    uamlib
    spirv-cross-glsl
    spirv-cross-core
    xxHash::xxhash)
set_target_properties(deko_shader_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${REXSDK_DIR}/out/linux-amd64")

# PC-side compilation of the exact variants captured by the Switch renderer.
add_executable(deko_shader_precompile
    shader_precompile.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deko/deko_shader_cache.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deko/deko_shader_warmup.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deko/deko_shaders.cpp"
    "${REXSDK_DIR}/src/ui/deko3d/cache_dksh.cpp")
target_include_directories(deko_shader_precompile PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${REXSDK_DIR}/include"
    "${REXSDK_DIR}/thirdparty/glslang"
    "${REXSDK_DIR}/thirdparty/renderdoc"
    "${REXSDK_DIR}/thirdparty/xxHash"
    "${REXSDK_DIR}/thirdparty/vulkan-headers/include")
target_compile_definitions(deko_shader_precompile PRIVATE
    FEAR_NX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(deko_shader_precompile PRIVATE
    rexgpu-xenos rexruntime uamlib spirv-cross-glsl spirv-cross-core xxHash::xxhash)
set_target_properties(deko_shader_precompile PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${REXSDK_DIR}/out/linux-amd64")

enable_testing()
if(UNIX AND NOT CMAKE_CROSSCOMPILING)
    add_executable(deko_nfsmw_audit_test EXCLUDE_FROM_ALL nfsmw_audit_test.cpp)
    target_include_directories(deko_nfsmw_audit_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
    add_test(NAME deko_nfsmw_audit_test COMMAND deko_nfsmw_audit_test)
endif()
foreach(_adapter_test cpu_adapter frontbuffer)
    add_executable(deko_${_adapter_test}_test EXCLUDE_FROM_ALL ${_adapter_test}_test.cpp)
    target_include_directories(deko_${_adapter_test}_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
    target_link_libraries(deko_${_adapter_test}_test PRIVATE pthread)
    add_test(NAME deko_${_adapter_test}_test COMMAND deko_${_adapter_test}_test)
endforeach()
add_executable(deko_shared_resources_test EXCLUDE_FROM_ALL shared_resources_test.cpp)
target_include_directories(deko_shared_resources_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_test(NAME deko_shared_resources_test COMMAND deko_shared_resources_test)
add_executable(deko_draw_upload_cache_test EXCLUDE_FROM_ALL draw_upload_cache_test.cpp)
target_include_directories(deko_draw_upload_cache_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_test(NAME deko_draw_upload_cache_test COMMAND deko_draw_upload_cache_test)
add_executable(deko_geometry_window_test EXCLUDE_FROM_ALL geometry_window_test.cpp)
target_include_directories(deko_geometry_window_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src" "${REXSDK_DIR}/thirdparty/xxHash")
target_link_libraries(deko_geometry_window_test PRIVATE xxHash::xxhash)
add_test(NAME deko_geometry_window_test COMMAND deko_geometry_window_test)
add_executable(deko_vertex_range_test EXCLUDE_FROM_ALL vertex_range_test.cpp)
target_include_directories(deko_vertex_range_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_test(NAME deko_vertex_range_test COMMAND deko_vertex_range_test)
add_executable(deko_vertex_range_shader_test EXCLUDE_FROM_ALL vertex_range_shader_test.cpp)
target_include_directories(deko_vertex_range_shader_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src" "${REXSDK_DIR}/include"
    "${REXSDK_DIR}/thirdparty/glslang" "${REXSDK_DIR}/thirdparty/renderdoc"
    "${REXSDK_DIR}/thirdparty/vulkan-headers/include")
target_link_libraries(deko_vertex_range_shader_test PRIVATE rexgpu-xenos rexruntime xxHash::xxhash)
set_target_properties(deko_vertex_range_shader_test PROPERTIES BUILD_RPATH "${REXSDK_DIR}/out/linux-amd64")
add_executable(deko_validation_cache_test EXCLUDE_FROM_ALL validation_cache_test.cpp)
target_link_libraries(deko_validation_cache_test PRIVATE xxHash::xxhash)
target_include_directories(deko_validation_cache_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src" "${REXSDK_DIR}/thirdparty/xxHash")
add_test(NAME deko_validation_cache_test COMMAND deko_validation_cache_test)
add_executable(deko_vertex_cache_test EXCLUDE_FROM_ALL vertex_cache_test.cpp)
target_include_directories(deko_vertex_cache_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_test(NAME deko_vertex_cache_test COMMAND deko_vertex_cache_test)
foreach(_cache_test shader_cache shader_warmup)
    add_executable(deko_${_cache_test}_test EXCLUDE_FROM_ALL
        ${_cache_test}_test.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deko/deko_${_cache_test}.cpp")
    target_link_libraries(deko_${_cache_test}_test PRIVATE xxHash::xxhash)
    target_include_directories(deko_${_cache_test}_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src" "${REXSDK_DIR}/thirdparty/xxHash")
    add_test(NAME deko_${_cache_test}_test COMMAND deko_${_cache_test}_test)
endforeach()

# Small CPU-only regression checks for Deko's byte-stride copy ABI. Build
# explicitly with `--target deko_texture_copy_test`; excluded from ordinary targets.
find_path(DEKO3D_INCLUDE_DIR deko3d.h HINTS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/deko3d/include"
    "$ENV{DEVKITPRO}/portlibs/switch/include"
    "/opt/devkitpro/portlibs/switch/include")
if(DEKO3D_INCLUDE_DIR)
    enable_testing()
    foreach(_image_test image_ops image_copy_shader image_sync constant_slots small_index_upload)
        add_executable(deko_${_image_test}_test EXCLUDE_FROM_ALL ${_image_test}_test.cpp)
        target_include_directories(deko_${_image_test}_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src" "${DEKO3D_INCLUDE_DIR}")
        add_test(NAME deko_${_image_test}_test COMMAND deko_${_image_test}_test)
    endforeach()
    target_link_libraries(deko_image_copy_shader_test PRIVATE uamlib)
    add_executable(deko_buffer_descriptor_test EXCLUDE_FROM_ALL buffer_descriptor_test.cpp)
    target_include_directories(deko_buffer_descriptor_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src" "${DEKO3D_INCLUDE_DIR}")
    add_test(NAME deko_buffer_descriptor_test COMMAND deko_buffer_descriptor_test)
    add_executable(deko_texture_copy_test EXCLUDE_FROM_ALL texture_layout_test.cpp)
    target_include_directories(deko_texture_copy_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src" "${DEKO3D_INCLUDE_DIR}")
    add_test(NAME deko_texture_copy_test COMMAND deko_texture_copy_test)
endif()

add_executable(deko_draw_dependencies_test EXCLUDE_FROM_ALL draw_dependencies_test.cpp)
target_include_directories(deko_draw_dependencies_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
add_test(NAME deko_draw_dependencies_test COMMAND deko_draw_dependencies_test)

add_executable(deko_texture_checks_test EXCLUDE_FROM_ALL texture_checks_test.cpp)
target_include_directories(deko_texture_checks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src" "${REXSDK_DIR}/thirdparty/xxHash")
add_test(NAME deko_texture_checks_test COMMAND deko_texture_checks_test)
