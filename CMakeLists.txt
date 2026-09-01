cmake_minimum_required(VERSION 3.16)

option(CANOPEN_NATIVE_BUILD
    "Build with a supplied native Linux Lely installation instead of the TQ8MP toolchain." OFF)

# The deployed TQ8MP cross build remains the default. Native mode is an optional
# local host-build surface and is not used by CI or release publication.
if(NOT CANOPEN_NATIVE_BUILD)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_LIST_DIR}/cmake/build_config.cmake"
        CACHE FILEPATH "CANopen master toolchain configuration" FORCE)
else()
    set(CANOPEN_DEFAULT_BUILD_TYPE "MinSizeRel"
        CACHE STRING "Default build type for optional native host builds")
endif()

project(canopen_master VERSION 0.5.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES)
    if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
        set(CMAKE_BUILD_TYPE
            "${CANOPEN_DEFAULT_BUILD_TYPE}"
            CACHE STRING "Build type" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

message(STATUS "CANopen master build type: ${CMAKE_BUILD_TYPE}")

option(CANOPEN_ENABLE_WARNINGS "Enable strict project warnings." ON)

if(NOT IS_DIRECTORY "${CANOPEN_LELY_INCLUDE_DIR}")
    message(FATAL_ERROR
        "Lely include directory does not exist: ${CANOPEN_LELY_INCLUDE_DIR}\n"
        "Set CANOPEN_LELY_INCLUDE_DIR to a matching Lely installation.")
endif()
if(NOT IS_DIRECTORY "${CANOPEN_LELY_LIBRARY_DIR}")
    message(FATAL_ERROR
        "Lely library directory does not exist: ${CANOPEN_LELY_LIBRARY_DIR}\n"
        "Set CANOPEN_LELY_LIBRARY_DIR to a matching Lely installation.")
endif()

set(LELY_INCLUDE_DIR "${CANOPEN_LELY_INCLUDE_DIR}")
set(LELY_LIBRARY_DIR "${CANOPEN_LELY_LIBRARY_DIR}")
set(LELY_COAPP_PKGCONFIG_FILE "${LELY_LIBRARY_DIR}/pkgconfig/liblely-coapp.pc")

if(NOT EXISTS "${LELY_COAPP_PKGCONFIG_FILE}")
    message(FATAL_ERROR
        "Lely package metadata does not exist: ${LELY_COAPP_PKGCONFIG_FILE}")
endif()

file(STRINGS "${LELY_COAPP_PKGCONFIG_FILE}" LELY_VERSION_LINES
     REGEX "^Version:[ \t]*")
list(LENGTH LELY_VERSION_LINES LELY_VERSION_LINE_COUNT)
if(NOT LELY_VERSION_LINE_COUNT EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one Version field in ${LELY_COAPP_PKGCONFIG_FILE}.")
endif()
list(GET LELY_VERSION_LINES 0 LELY_VERSION_LINE)
string(REGEX REPLACE "^Version:[ \t]*" "" CANOPEN_LELY_VERSION
       "${LELY_VERSION_LINE}")
string(STRIP "${CANOPEN_LELY_VERSION}" CANOPEN_LELY_VERSION)
if(CANOPEN_LELY_VERSION STREQUAL "")
    message(FATAL_ERROR
        "The Lely Version field is empty in ${LELY_COAPP_PKGCONFIG_FILE}.")
endif()

set(SPDLOG_VERSION_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include/spdlog/version.h")
if(NOT EXISTS "${SPDLOG_VERSION_HEADER}")
    message(FATAL_ERROR
        "Vendored spdlog version header is missing: ${SPDLOG_VERSION_HEADER}")
endif()

find_package(Threads REQUIRED)

# Build the pinned vendored spdlog sources once as a static archive. The
# spdlog::spdlog target propagates SPDLOG_COMPILED_LIB to all consumers.
add_subdirectory(third_party/spdlog)

function(canopen_apply_warnings target_name)
    if(NOT CANOPEN_ENABLE_WARNINGS)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Werror=return-type)
    endif()
endfunction()

function(canopen_apply_debug_options target_name)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:-O0>
            $<$<CONFIG:Debug>:-g3>
            $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
            $<$<CONFIG:Debug>:-fno-optimize-sibling-calls>)
    endif()
endfunction()

set(LELY_COAPP_LIBRARY "${LELY_LIBRARY_DIR}/liblely-coapp.so")
set(LELY_IO2_LIBRARY "${LELY_LIBRARY_DIR}/liblely-io2.so")
set(LELY_EV_LIBRARY "${LELY_LIBRARY_DIR}/liblely-ev.so")
set(LELY_CO_LIBRARY "${LELY_LIBRARY_DIR}/liblely-co.so")
set(LELY_CAN_LIBRARY "${LELY_LIBRARY_DIR}/liblely-can.so")
set(LELY_UTIL_LIBRARY "${LELY_LIBRARY_DIR}/liblely-util.so")
set(LELY_LIBC_LIBRARY "${LELY_LIBRARY_DIR}/liblely-libc.so")

foreach(lely_library IN ITEMS
        LELY_COAPP_LIBRARY
        LELY_IO2_LIBRARY
        LELY_EV_LIBRARY
        LELY_CO_LIBRARY
        LELY_CAN_LIBRARY
        LELY_UTIL_LIBRARY
        LELY_LIBC_LIBRARY)
    if(NOT EXISTS "${${lely_library}}")
        message(FATAL_ERROR "Required Lely library not found: ${${lely_library}}")
    endif()
endforeach()

set(TARGET_NAME canopen_master)

file(GLOB CANOPEN_MASTER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(${TARGET_NAME}
    ${CANOPEN_MASTER_SOURCES}
)

target_include_directories(${TARGET_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(${TARGET_NAME}
    SYSTEM PRIVATE
        "${LELY_INCLUDE_DIR}")

target_link_options(${TARGET_NAME}
    PRIVATE
        "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.map"
        "-Wl,-rpath-link,${LELY_LIBRARY_DIR}")

target_link_libraries(${TARGET_NAME}
    PRIVATE
        spdlog::spdlog
        "${LELY_COAPP_LIBRARY}"
        "${LELY_IO2_LIBRARY}"
        "${LELY_EV_LIBRARY}"
        "${LELY_CO_LIBRARY}"
        "${LELY_CAN_LIBRARY}"
        "${LELY_UTIL_LIBRARY}"
        "${LELY_LIBC_LIBRARY}"
        Threads::Threads
        rt)

set_target_properties(${TARGET_NAME} PROPERTIES
    SKIP_BUILD_RPATH TRUE
    INSTALL_RPATH "")

canopen_apply_warnings(${TARGET_NAME})
canopen_apply_debug_options(${TARGET_NAME})

if(NOT CANOPEN_NATIVE_BUILD)
    add_custom_target(download
        COMMENT "Deploying ${TARGET_NAME} to target board and running..."
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMAND /bin/chmod a+x ./deploy/run.sh
        COMMAND ./deploy/run.sh "${TARGET_NAME}" "${CACHED_IP_ADDR}" run
                "$<TARGET_FILE:${TARGET_NAME}>"
        USES_TERMINAL
        VERBATIM)
    add_dependencies(download ${TARGET_NAME})

    add_custom_target(debug-deploy
        COMMENT "Deploying Debug ${TARGET_NAME} and starting gdbserver..."
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMAND ${CMAKE_COMMAND}
            "-DCANOPEN_BUILD_TYPE=$<CONFIG>"
            -P "${CMAKE_SOURCE_DIR}/cmake/require_debug_build.cmake"
        COMMAND /bin/chmod a+x ./deploy/run.sh
        COMMAND ./deploy/run.sh "${TARGET_NAME}" "${CACHED_IP_ADDR}" gdb
                "$<TARGET_FILE:${TARGET_NAME}>"
        USES_TERMINAL
        VERBATIM)
    add_dependencies(debug-deploy ${TARGET_NAME})

    add_custom_target(debug DEPENDS debug-deploy)
endif()

install(TARGETS ${TARGET_NAME} RUNTIME DESTINATION bin)
install(FILES
    config/project.eds
    config/master.yml
    config/generated/master.dcf
    config/generated/mcu_node_1.bin
    DESTINATION config)
install(FILES
    config/generated/master.bin
    config/generated/project.dcfgen.eds
    DESTINATION config/generated)
