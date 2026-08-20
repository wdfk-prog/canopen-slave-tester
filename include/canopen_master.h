/**
 * @file
 * @brief Narrow AsyncMaster access shim for test-only protected services.
 */

#ifndef CANOPEN_MASTER_H
#define CANOPEN_MASTER_H

#include <lely/co/dev.h>
#include <lely/co/emcy.hpp>
#include <lely/co/nmt.hpp>
#include <lely/co/obj.h>
#include <lely/coapp/master.hpp>

#include <cstdint>
#include <mutex>

class HostSdoServerFixture;

/**
 * @brief Test master with narrow access to Lely protected services.
 *
 * Lely's high-level AsyncMaster::Error() does not expose the return value from
 * the underlying local EMCY push operation, and the coapp API does not expose
 * the clear operation required to emit a standard error-reset message. This
 * shim exposes only protected services required by the test fixtures and
 * protocol fault-injection cases; it does not implement a second CANopen
 * protocol path.
 */
class CanopenTestMaster final : public lely::canopen::AsyncMaster {
public:
    using lely::canopen::AsyncMaster::AsyncMaster;

    /**
     * @brief Push one local EMCY and report whether the local stack changed.
     *
     * A failed Lely push can occur either before the local EMCY stack changes
     * or after the error has been added but before the EMCY frame is queued.
     * The latter case still belongs to B06 cleanup, so stack_updated is derived
     * while holding the master lock. When the stack was already non-empty,
     * OD 0x1003 is synchronized by Lely and distinguishes duplicate pushes.
     *
     * @param error_code Emergency error code; must be non-zero.
     * @param error_register Error Register value.
     * @param msef Five manufacturer-specific EMCY bytes.
     * @param stack_updated Receives true if this call added an error to the
     *        local EMCY stack, including a push that later failed to queue.
     * @return Lely COEmcy::push() result: 0 on success, otherwise -1.
     */
    int pushLocalEmcy(std::uint16_t error_code,
                      std::uint8_t error_register,
                      const std::uint8_t msef[5],
                      bool& stack_updated) noexcept
    {
        std::lock_guard<lely::util::BasicLockable> lock(*this);
        stack_updated = false;

        lely::COEmcy* const emcy = localEmcyService();
        if (emcy == nullptr) {
            return -1;
        }

        std::uint16_t previous_error = 0;
        std::uint8_t previous_register = 0;
        emcy->peek(&previous_error, &previous_register);

        std::uint8_t previous_count = 0;
        const bool previous_stack_empty =
            previous_error == 0U && previous_register == 0U;
        const bool previous_count_valid =
            !previous_stack_empty && localEmcyHistoryCount(emcy, previous_count);

        const int result = emcy->push(error_code, error_register, msef);
        if (result == 0) {
            stack_updated = true;
            return 0;
        }

        std::uint16_t active_error = 0;
        std::uint8_t active_register = 0;
        emcy->peek(&active_error, &active_register);

        if (previous_stack_empty) {
            stack_updated = active_error == error_code && active_register != 0U;
        } else if (previous_count_valid) {
            std::uint8_t active_count = 0;
            if (localEmcyHistoryCount(emcy, active_count)) {
                stack_updated =
                    active_error == error_code
                    && static_cast<unsigned int>(active_count)
                        == static_cast<unsigned int>(previous_count) + 1U;
            }
        } else if (previous_error != error_code && active_error == error_code) {
            /* A changed top entry proves insertion even without 0x1003 count. */
            stack_updated = true;
        }

        return result;
    }

    /**
     * @brief Read the newest local EMCY error and combined Error Register.
     *
     * @param error_code Receives the newest active error code, or zero.
     * @param error_register Receives the combined local Error Register.
     * @return true when the local EMCY service is available; otherwise false.
     */
    bool peekLocalEmcy(std::uint16_t& error_code,
                       std::uint8_t& error_register) noexcept
    {
        std::lock_guard<lely::util::BasicLockable> lock(*this);
        lely::COEmcy* const emcy = localEmcyService();
        if (emcy == nullptr) {
            return false;
        }
        emcy->peek(&error_code, &error_register);
        return true;
    }

    /**
     * @brief Clear the local EMCY stack and attempt standard error reset.
     *
     * For a non-empty stack, Lely clears the stack and OD 0x1003 before it
     * attempts to transmit the recovery EMCY. A transmission failure therefore
     * returns -1 after the local clear has already taken effect.
     *
     * @return 0 on success, including an already-empty stack; otherwise -1.
     */
    int clearLocalEmcy() noexcept
    {
        std::lock_guard<lely::util::BasicLockable> lock(*this);
        lely::COEmcy* const emcy = localEmcyService();
        return emcy != nullptr ? emcy->clear() : -1;
    }

    /**
     * @brief Abort the active/queued Client-SDO requests for one remote node.
     *
     * This exposes the protected Lely Client-SDO queue operation required by
     * B02-08. The caller must ensure no unrelated SDO request for the same node
     * is active because Lely applies the abort code to the complete per-node
     * queue. CancelAll() does not count the stopped ongoing request in its return
     * value, so the request completion callback is the authoritative abort result.
     *
     * @param node_id Remote SDO server node-ID.
     * @param abort_code SDO abort code sent for the cancellation.
     * @return true when the remote Client-SDO service exists and cancellation
     *         was requested; otherwise false.
     */
    bool cancelRemoteSdoRequests(
        std::uint8_t node_id, lely::canopen::SdoErrc abort_code)
    {
        std::lock_guard<lely::util::BasicLockable> lock(*this);
        lely::canopen::Sdo* const sdo = GetSdo(node_id);
        if (sdo == nullptr) {
            return false;
        }

        (void)sdo->CancelAll(abort_code);
        return true;
    }

private:
    friend class HostSdoServerFixture;

    /** @return Lely's existing local NMT service, or null if unavailable. */
    lely::CONMT* localNmtService() noexcept
    {
        __co_nmt* const raw_nmt = nmt();
        return raw_nmt != nullptr ? reinterpret_cast<lely::CONMT*>(raw_nmt)
                                  : nullptr;
    }

    /** @return Local Object Dictionary owned by the master NMT service. */
    co_dev_t* localDevice() noexcept
    {
        lely::CONMT* const local_nmt = localNmtService();
        return local_nmt != nullptr
            ? reinterpret_cast<co_dev_t*>(local_nmt->getDev()) : nullptr;
    }

    /** @return Default local Server-SDO number 1, or null if unavailable. */
    lely::COSSDO* localServerSdo() noexcept
    {
        lely::CONMT* const local_nmt = localNmtService();
        return local_nmt != nullptr ? local_nmt->getSSDO(1U) : nullptr;
    }

    /**
     * @brief Read the synchronized OD 0x1003 active-error count.
     *
     * @param emcy Local EMCY service.
     * @param count Receives the current active-error count.
     * @return true when OD 0x1003:00 exists; otherwise false.
     */
    static bool localEmcyHistoryCount(lely::COEmcy* emcy,
                                      std::uint8_t& count) noexcept
    {
        co_dev_t* const dev = reinterpret_cast<co_dev_t*>(emcy->getDev());
        co_sub_t* const count_sub = co_dev_find_sub(dev, 0x1003U, 0x00U);
        if (count_sub == nullptr) {
            return false;
        }
        count = co_sub_get_val_u8(count_sub);
        return true;
    }

    /** @return Lely's existing local EMCY service, or null if unavailable. */
    lely::COEmcy* localEmcyService() noexcept
    {
        lely::CONMT* const local_nmt = localNmtService();
        return local_nmt != nullptr ? local_nmt->getEmcy() : nullptr;
    }
};

#endif /* CANOPEN_MASTER_H */
