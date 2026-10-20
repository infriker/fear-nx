if(NOT REXGLUE_PLATFORM_SWITCH)
    message(FATAL_ERROR "horizon-probe is only available for the Horizon ARM64 target.")
endif()
if(NOT TARGET rexruntime OR NOT COMMAND rexglue_switch_configure_target)
    message(FATAL_ERROR "Add horizon-probe after the ReXGlue SDK subdirectory.")
endif()

add_executable(fear_nx_horizon_probe EXCLUDE_FROM_ALL main.cpp)
target_include_directories(fear_nx_horizon_probe PRIVATE
    "${REXSDK_DIR}/src/core")

# NO_ENTRY keeps the windowed app and renderer startup out of this test NRO.
# No GPU plugin is requested; the target exercises the SDK runtime directly.
rexglue_switch_configure_target(fear_nx_horizon_probe NO_ENTRY)
rexglue_switch_add_nro(fear_nx_horizon_probe
    NAME "F.E.A.R. NX Horizon Probe"
    AUTHOR "fear-nx contributors"
    VERSION "0.1.0")