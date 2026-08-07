# Stop at the first instruction when source line information is unavailable.
set step-mode on

# Avoid remote range-stepping inconsistencies.
set range-stepping off

# Skip recognizable C++ standard-library implementations.
skip -rfunction ^std::.*
skip -rfunction ^__gnu_cxx::.*
skip -rfunction ^__cxxabiv1::.*

# Skip inline C++ standard-library header implementations.
skip -gfile */include/c++/*
skip -gfile */include/c++/*/*
skip -gfile */include/c++/*/*/*

echo System-library stepping filters installed.\n