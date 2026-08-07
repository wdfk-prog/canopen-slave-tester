if(NOT DEFINED CANOPEN_BUILD_TYPE OR NOT CANOPEN_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "The debug deployment target requires a Debug build. "
        "Reconfigure with: cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug")
endif()
