cmake_minimum_required(VERSION 3.16)

# Load the project-local cross-compilation configuration before project() so a
# clean build directory can be configured with a plain `cmake ..` command.
set(CMAKE_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/cmake/build_config.cmake"
    CACHE FILEPATH "CANopen tester toolchain configuration" FORCE)

project(canopen_slave_tester VERSION 0.3.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "${CANOPEN_DEFAULT_BUILD_TYPE}" CACHE STRING "Build type" FORCE)
endif()

option(CANOPEN_TEST_ENABLE_WARNINGS "Enable strict project warnings." ON)

if(NOT IS_DIRECTORY "${CANOPEN_LELY_INCLUDE_DIR}")
    message(FATAL_ERROR
        "Lely include directory does not exist: ${CANOPEN_LELY_INCLUDE_DIR}\n"
        "Update CANOPEN_LELY_INCLUDE_DIR in cmake/build_config.local.cmake.")
endif()
if(NOT IS_DIRECTORY "${CANOPEN_LELY_LIBRARY_DIR}")
    message(FATAL_ERROR
        "Lely library directory does not exist: ${CANOPEN_LELY_LIBRARY_DIR}\n"
        "Update CANOPEN_LELY_LIBRARY_DIR in cmake/build_config.local.cmake.")
endif()

set(LELY_INCLUDE_DIR "${CANOPEN_LELY_INCLUDE_DIR}")
set(LELY_LIBRARY_DIR "${CANOPEN_LELY_LIBRARY_DIR}")
set(LELY_COAPP_PKGCONFIG_FILE "${LELY_LIBRARY_DIR}/pkgconfig/liblely-coapp.pc")

if(NOT EXISTS "${LELY_COAPP_PKGCONFIG_FILE}")
    message(FATAL_ERROR
        "Lely package metadata does not exist: ${LELY_COAPP_PKGCONFIG_FILE}")
endif()

file(STRINGS "${LELY_COAPP_PKGCONFIG_FILE}" LELY_VERSION_LINES REGEX "^Version:[ \t]*")
list(LENGTH LELY_VERSION_LINES LELY_VERSION_LINE_COUNT)
if(NOT LELY_VERSION_LINE_COUNT EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one Version field in ${LELY_COAPP_PKGCONFIG_FILE}.")
endif()
list(GET LELY_VERSION_LINES 0 LELY_VERSION_LINE)
string(REGEX REPLACE "^Version:[ \t]*" "" CANOPEN_LELY_VERSION "${LELY_VERSION_LINE}")
string(STRIP "${CANOPEN_LELY_VERSION}" CANOPEN_LELY_VERSION)
if(CANOPEN_LELY_VERSION STREQUAL "")
    message(FATAL_ERROR
        "The Lely Version field is empty in ${LELY_COAPP_PKGCONFIG_FILE}.")
endif()

set(SPDLOG_VERSION_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include/spdlog/version.h")
if(NOT EXISTS "${SPDLOG_VERSION_HEADER}")
    message(FATAL_ERROR "Vendored spdlog version header is missing: ${SPDLOG_VERSION_HEADER}")
endif()
foreach(spdlog_part IN ITEMS MAJOR MINOR PATCH)
    file(STRINGS "${SPDLOG_VERSION_HEADER}" SPDLOG_${spdlog_part}_LINES
        REGEX "^#define SPDLOG_VER_${spdlog_part} [0-9]+$")
    list(LENGTH SPDLOG_${spdlog_part}_LINES SPDLOG_${spdlog_part}_LINE_COUNT)
    if(NOT SPDLOG_${spdlog_part}_LINE_COUNT EQUAL 1)
        message(FATAL_ERROR
            "Expected one SPDLOG_VER_${spdlog_part} definition in ${SPDLOG_VERSION_HEADER}.")
    endif()
    list(GET SPDLOG_${spdlog_part}_LINES 0 SPDLOG_${spdlog_part}_LINE)
    string(REGEX REPLACE "^#define SPDLOG_VER_${spdlog_part} " ""
        SPDLOG_${spdlog_part} "${SPDLOG_${spdlog_part}_LINE}")
endforeach()
set(CANOPEN_SPDLOG_VERSION
    "${SPDLOG_MAJOR}.${SPDLOG_MINOR}.${SPDLOG_PATCH}")

set(CANOPEN_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${CANOPEN_GENERATED_INCLUDE_DIR}/canopen_test")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/canopen_test/version_config.hpp.in"
    "${CANOPEN_GENERATED_INCLUDE_DIR}/canopen_test/version_config.hpp"
    @ONLY)

find_package(Threads REQUIRED)

function(canopen_test_apply_warnings target_name)
    if(NOT CANOPEN_TEST_ENABLE_WARNINGS)
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

# spdlog is vendored as headers only. This interface target adds include and
# threading requirements without producing libspdlog.so or libspdlog.a.
add_library(canopen_test_spdlog INTERFACE)
target_include_directories(canopen_test_spdlog
    SYSTEM INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include")
target_link_libraries(canopen_test_spdlog INTERFACE Threads::Threads)

add_library(canopen_test_support STATIC
    src/app/app_config.cpp
    src/app/logger.cpp
    src/app/runtime_status.cpp
    src/app/signal_handler.cpp)

target_include_directories(canopen_test_support
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CANOPEN_GENERATED_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(canopen_test_support
    PUBLIC Threads::Threads
    PRIVATE canopen_test_spdlog)
canopen_test_apply_warnings(canopen_test_support)

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

set(TARGET_NAME canopen_slave_tester)

add_executable(${TARGET_NAME}
    src/main.cpp
    src/app/application.cpp
    src/runtime/lely_runtime.cpp)

target_include_directories(${TARGET_NAME}
    SYSTEM PRIVATE
        "${LELY_INCLUDE_DIR}")

target_link_options(${TARGET_NAME}
    PRIVATE
        "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.map"
        "-Wl,-rpath-link,${LELY_LIBRARY_DIR}")

target_link_libraries(${TARGET_NAME}
    PRIVATE
        canopen_test_support
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

canopen_test_apply_warnings(${TARGET_NAME})

# make download: deploy the executable to the target board and run it.
add_custom_target(download
    COMMENT "Deploying ${TARGET_NAME} to target board and running..."
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMAND /bin/chmod a+x ./deploy/run.sh
    COMMAND ./deploy/run.sh "${TARGET_NAME}" "${CACHED_IP_ADDR}" run "$<TARGET_FILE:${TARGET_NAME}>"
    USES_TERMINAL
    VERBATIM)
add_dependencies(download ${TARGET_NAME})

# make debug: deploy the executable and start gdbserver on the target board.
add_custom_target(debug
    COMMENT "Deploying ${TARGET_NAME} to target board and starting gdbserver..."
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMAND /bin/chmod a+x ./deploy/run.sh
    COMMAND ./deploy/run.sh "${TARGET_NAME}" "${CACHED_IP_ADDR}" gdb "$<TARGET_FILE:${TARGET_NAME}>"
    USES_TERMINAL
    VERBATIM)
add_dependencies(debug ${TARGET_NAME})

install(TARGETS ${TARGET_NAME} RUNTIME DESTINATION bin)
install(FILES
    config/project.eds
    config/master.yml
    config/tester.conf
    DESTINATION config)
install(FILES
    config/generated/master.dcf
    config/generated/master.bin
    config/generated/project.dcfgen.eds
    DESTINATION config/generated)
