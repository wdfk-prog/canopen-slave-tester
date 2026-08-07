set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_cross_compile "$ENV{CROSS_COMPILE}")
if(_cross_compile STREQUAL "")
    message(FATAL_ERROR
        "CROSS_COMPILE is not set. Example: "
        "export CROSS_COMPILE=/opt/toolchains/bin/aarch64-linux-gnu-")
endif()

set(CMAKE_C_COMPILER "${_cross_compile}gcc" CACHE FILEPATH "AArch64 C compiler")
set(CMAKE_CXX_COMPILER "${_cross_compile}g++" CACHE FILEPATH "AArch64 C++ compiler")
set(CMAKE_AR "${_cross_compile}ar" CACHE FILEPATH "AArch64 archiver")
set(CMAKE_RANLIB "${_cross_compile}ranlib" CACHE FILEPATH "AArch64 ranlib")
set(CMAKE_STRIP "${_cross_compile}strip" CACHE FILEPATH "AArch64 strip")

if(DEFINED ENV{TARGET_SYSROOT} AND NOT "$ENV{TARGET_SYSROOT}" STREQUAL "")
    set(CMAKE_SYSROOT "$ENV{TARGET_SYSROOT}" CACHE PATH "Target sysroot")
    list(APPEND CMAKE_FIND_ROOT_PATH "$ENV{TARGET_SYSROOT}")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
