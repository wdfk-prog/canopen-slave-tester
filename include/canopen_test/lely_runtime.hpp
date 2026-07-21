/**
 * @file
 * @brief Declares the dependency-isolated Lely runtime owner.
 */

#ifndef CANOPEN_TEST_LELY_RUNTIME_HPP_
#define CANOPEN_TEST_LELY_RUNTIME_HPP_

#include <memory>

#include "canopen_test/app_config.hpp"
#include "canopen_test/runtime_status.hpp"

namespace canopen_test {

/**
 * @brief Owns the complete Lely I/O and CANopen runtime for stage 1.
 */
class LelyRuntime final {
public:
    /**
     * @brief Constructs an idle runtime.
     *
     * @param status Thread-safe status recorder owned by the caller.
     */
    explicit LelyRuntime(RuntimeStatus& status);

    LelyRuntime(const LelyRuntime&) = delete;
    LelyRuntime& operator=(const LelyRuntime&) = delete;

    /**
     * @brief Stops and destroys all runtime resources.
     */
    ~LelyRuntime() noexcept;

    /**
     * @brief Creates the I/O context, SocketCAN channel, and AsyncMaster.
     *
     * Startup is transactional. If construction fails, all partially created
     * Lely resources are released in reverse dependency order before the
     * exception reaches the caller. A completed Stop() allows the same object
     * to start a new independent runtime session.
     *
     * @param config Validated runtime configuration.
     *
     * @throws SocketCanError on controller/channel failures.
     * @throws LelyRuntimeError on DCF or Lely initialization failures.
     */
    void Start(const AppConfig& config);

    /**
     * @brief Runs the Lely event loop until a stop request completes.
     *
     * @return Zero on normal shutdown.
     *
     * @throws LelyRuntimeError if the loop fails or terminates unexpectedly.
     */
    int Run();

    /**
     * @brief Requests asynchronous shutdown from any normal thread.
     *
     * This function is idempotent. It does not destroy Lely objects on the
     * caller thread; shutdown is posted to the event-loop executor.
     */
    void RequestStop() noexcept;

    /**
     * @brief Releases runtime resources in reverse dependency order.
     *
     * This function is idempotent. If Run() is active on another thread, it
     * requests loop termination and waits until Run() releases the runtime.
     * Calling Stop() from the event-loop thread only requests termination; the
     * owner must keep this object alive until Run() returns.
     */
    void Stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace canopen_test

#endif /* CANOPEN_TEST_LELY_RUNTIME_HPP_ */
