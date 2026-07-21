/**
 * @file
 * @brief Implements the process entry point and top-level exception boundary.
 */

#include "canopen_test/application.hpp"

#include "canopen_test/error.hpp"

#include <cstdio>
#include <exception>

namespace {

/**
 * @brief Writes an unrecoverable top-level failure without throwing.
 * @param category Stable diagnostic category.
 * @param message Failure description.
 */
void WriteFatal(const char* category, const char* message) noexcept
{
    std::fprintf(stderr, "[ERROR] [%s] %s\n", category, message);
}

} // namespace

/**
 * @brief Process entry point for the CANopen slave tester.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Stable process exit code.
 */
int main(int argc, char* argv[])
{
    try {
        canopen_test::Application application;
        return application.Run(argc, argv);
    } catch (const canopen_test::ApplicationError& exception) {
        WriteFatal(exception.category().c_str(), exception.what());
        return static_cast<int>(exception.code());
    } catch (const std::exception& exception) {
        WriteFatal("INTERNAL", exception.what());
        return static_cast<int>(canopen_test::ExitCode::kInternal);
    } catch (...) {
        WriteFatal("INTERNAL", "unknown exception");
        return static_cast<int>(canopen_test::ExitCode::kInternal);
    }
}
