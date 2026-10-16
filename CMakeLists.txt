cmake_minimum_required(VERSION 3.16)
set(MOSAICO_UTILS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../submodule/esp-mosaico-utils")
set(MOSAICO_BSP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../submodule/esp-mosaico-bsp")
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.application.defaults")
endif()
# The pinned gspc writes \\?\-prefixed, backslash-escaped paths into its depfile;
# CMake's depfile conversion turns those into invalid "/D:/..." paths and Ninja
# aborts. Route the compiler through a wrapper that normalises the depfile.
# An explicit GSPC_EXECUTABLE in the environment still wins.
if(WIN32 AND NOT DEFINED ENV{GSPC_EXECUTABLE})
    set(GSPC_EXECUTABLE "${CMAKE_CURRENT_LIST_DIR}/tools/gspc_wrap.cmd"
        CACHE FILEPATH "ESP-GSP compiler (Windows depfile wrapper)" FORCE)
endif()
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_compiler.cmake")
list(APPEND EXTRA_COMPONENT_DIRS
    "${MOSAICO_UTILS_ROOT}/esp-mosaico-recovery/components/esp_mosaico_app_recovery"
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_gsp_bundle"
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_gsp_iris")
include("${MOSAICO_UTILS_ROOT}/esp-mosaico-recovery/cmake/mosaico_idf_project.cmake")
project(pc_display VERSION 1.0.0)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/system_update.cmake")
