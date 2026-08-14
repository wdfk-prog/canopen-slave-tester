# Build the vendored spdlog v1.17.0 sources as a compiled static library.
# The source list and compile definition match spdlog's upstream static target.

add_library(spdlog STATIC
    src/spdlog.cpp
    src/stdout_sinks.cpp
    src/color_sinks.cpp
    src/file_sinks.cpp
    src/async.cpp
    src/cfg.cpp
    src/bundled_fmtlib_format.cpp)

add_library(spdlog::spdlog ALIAS spdlog)

target_compile_definitions(spdlog PUBLIC SPDLOG_COMPILED_LIB)
target_include_directories(spdlog SYSTEM PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(spdlog PUBLIC Threads::Threads)
