# Cross-compilation configuration for the CANopen slave tester.
#
# Machine-specific paths are loaded from build_config.local.cmake, which is
# intentionally excluded from Git. Copy build_config.local.cmake.example before
# configuring the project for the first time.

set(CANOPEN_LOCAL_BUILD_CONFIG
    "${CMAKE_CURRENT_LIST_DIR}/build_config.local.cmake"
    CACHE FILEPATH "Machine-local CANopen tester build configuration")

if(NOT EXISTS "${CANOPEN_LOCAL_BUILD_CONFIG}")
    message(FATAL_ERROR
        "Local build configuration does not exist: ${CANOPEN_LOCAL_BUILD_CONFIG}\n"
        "Copy cmake/build_config.local.cmake.example to "
        "cmake/build_config.local.cmake and update the local paths.")
endif()

include("${CANOPEN_LOCAL_BUILD_CONFIG}")

set(CANOPEN_TARGET_SYSTEM_NAME "Linux" CACHE STRING "Target operating system" FORCE)
set(CANOPEN_TARGET_PROCESSOR "aarch64" CACHE STRING "Target processor architecture" FORCE)
set(CANOPEN_DEFAULT_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Default build type" FORCE)

if(NOT DEFINED CACHED_IP_ADDR)
    set(CACHED_IP_ADDR "$ENV{CANOPEN_TARGET_IP}" CACHE STRING "Target board address for deployment")
endif()

foreach(_canopen_required_variable IN ITEMS
        CANOPEN_TOOLCHAIN_BIN_DIR
        CANOPEN_TOOLCHAIN_PREFIX
        CANOPEN_SYSROOT
        CANOPEN_LELY_INCLUDE_DIR
        CANOPEN_LELY_LIBRARY_DIR)
    if(NOT DEFINED ${_canopen_required_variable}
            OR "${${_canopen_required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Required variable ${_canopen_required_variable} is not set in "
            "${CANOPEN_LOCAL_BUILD_CONFIG}.")
    endif()
endforeach()

set(CMAKE_SYSTEM_NAME "${CANOPEN_TARGET_SYSTEM_NAME}")
set(CMAKE_SYSTEM_PROCESSOR "${CANOPEN_TARGET_PROCESSOR}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_canopen_tool_prefix "${CANOPEN_TOOLCHAIN_BIN_DIR}/${CANOPEN_TOOLCHAIN_PREFIX}")

set(CMAKE_C_COMPILER "${_canopen_tool_prefix}-gcc" CACHE FILEPATH "C cross-compiler" FORCE)
set(CMAKE_CXX_COMPILER "${_canopen_tool_prefix}-g++" CACHE FILEPATH "C++ cross-compiler" FORCE)
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}" CACHE FILEPATH "ASM cross-compiler" FORCE)
set(CMAKE_AR "${_canopen_tool_prefix}-ar" CACHE FILEPATH "Cross archiver" FORCE)
set(CMAKE_RANLIB "${_canopen_tool_prefix}-ranlib" CACHE FILEPATH "Cross ranlib" FORCE)
set(CMAKE_STRIP "${_canopen_tool_prefix}-strip" CACHE FILEPATH "Cross strip tool" FORCE)
set(CMAKE_OBJCOPY "${_canopen_tool_prefix}-objcopy" CACHE FILEPATH "Cross objcopy tool" FORCE)
set(CMAKE_OBJDUMP "${_canopen_tool_prefix}-objdump" CACHE FILEPATH "Cross objdump tool" FORCE)
set(CMAKE_SIZE "${_canopen_tool_prefix}-size" CACHE FILEPATH "Cross size tool" FORCE)

if(NOT IS_DIRECTORY "${CANOPEN_TOOLCHAIN_BIN_DIR}")
    message(FATAL_ERROR "Cross-tool directory does not exist: ${CANOPEN_TOOLCHAIN_BIN_DIR}")
endif()
if(NOT IS_DIRECTORY "${CANOPEN_SYSROOT}")
    message(FATAL_ERROR "Target sysroot does not exist: ${CANOPEN_SYSROOT}")
endif()

foreach(_canopen_tool IN ITEMS
        CMAKE_C_COMPILER
        CMAKE_CXX_COMPILER
        CMAKE_AR
        CMAKE_RANLIB)
    if(NOT EXISTS "${${_canopen_tool}}")
        message(FATAL_ERROR "Required cross-tool does not exist: ${${_canopen_tool}}")
    endif()
endforeach()

set(CMAKE_SYSROOT "${CANOPEN_SYSROOT}" CACHE PATH "CMake sysroot" FORCE)
set(CMAKE_FIND_ROOT_PATH "${CANOPEN_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

unset(_canopen_required_variable)
unset(_canopen_tool)
unset(_canopen_tool_prefix)
