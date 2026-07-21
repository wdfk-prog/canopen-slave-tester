/**
 * @file
 * @brief Declares the command-line application entry point.
 */

#ifndef CANOPEN_TEST_APPLICATION_HPP_
#define CANOPEN_TEST_APPLICATION_HPP_

namespace canopen_test {

/**
 * @brief Stage-1 command-line application entry point.
 */
class Application final {
public:
    /**
     * @brief Parses arguments, validates configuration, and runs the runtime.
     *
     * @param argc Argument count.
     * @param argv Argument vector.
     * @return Process exit code.
     */
    int Run(int argc, char* argv[]);
};

} // namespace canopen_test

#endif /* CANOPEN_TEST_APPLICATION_HPP_ */
