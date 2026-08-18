/**
 * @file
 * @brief J04/B03 local Server-SDO fixture hosted by CANopen master node 127.
 */

#ifndef HOST_SDO_SERVER_FIXTURE_H
#define HOST_SDO_SERVER_FIXTURE_H

#include <lely/co/obj.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class EmcyTestMaster;
/**
 * @brief Own test-only Object Dictionary entries served by the master SSDO.
 *
 * The fixture installs three manufacturer-specific objects only for B03 and
 * removes them before the master is destroyed. Existing objects are never
 * replaced.
 */
class HostSdoServerFixture {
public:
    /** Remote U32 read/write test object. */
    static constexpr std::uint16_t kU32Index = 0x2F00U;
    /** Remote segmented OCTET_STRING read/write test object. */
    static constexpr std::uint16_t kOctetsIndex = 0x2F01U;
    /** Remote read-only abort test object. */
    static constexpr std::uint16_t kReadOnlyIndex = 0x2F02U;
    /** Scalar sub-index used by all fixture objects. */
    static constexpr std::uint8_t kSubindex = 0x00U;
    /** Stable baseline value for the U32 object. */
    static constexpr std::uint32_t kInitialU32 = 0x13579BDFU;
    /** Stable one-byte baseline for the segmented OCTET_STRING object. */
    static constexpr std::uint8_t kInitialOctet = 0x5AU;
    /** Stable value exposed by the read-only object. */
    static constexpr std::uint32_t kReadOnlyValue = 0xA5A55A5AU;

    /**
     * @brief Bind the fixture to the existing master without installing it.
     *
     * @param master Active Host CANopen master.
     */
    explicit HostSdoServerFixture(EmcyTestMaster& master) noexcept;

    /** @brief Remove all fixture objects if they are still installed. */
    ~HostSdoServerFixture();

    /**
     * @brief Verify SSDO #1 and install all B03 test objects atomically.
     *
     * @return true when all objects were installed; otherwise false.
     */
    bool install() noexcept;

    /** @brief Remove all objects created by install(). */
    void uninstall() noexcept;

    /** @return true when all fixture objects are currently installed. */
    bool installed() const noexcept { return installed_; }

    /**
     * @brief Read the local 0x2F00 value under the master lock.
     *
     * @param value Receives the current U32 value.
     * @return true on success; otherwise false.
     */
    bool readU32(std::uint32_t& value) const noexcept;

    /**
     * @brief Write the local 0x2F00 value under the master lock.
     *
     * @param value New U32 value.
     * @return true on success; otherwise false.
     */
    bool writeU32(std::uint32_t value) noexcept;

    /**
     * @brief Read the local 0x2F01 byte array under the master lock.
     *
     * @param value Receives a copy of the current OCTET_STRING.
     * @return true on success; otherwise false.
     */
    bool readOctets(std::vector<std::uint8_t>& value) const;

    /**
     * @brief Replace the local 0x2F01 byte array under the master lock.
     *
     * @param value New OCTET_STRING bytes.
     * @return true on success; otherwise false.
     */
    bool writeOctets(const std::vector<std::uint8_t>& value) noexcept;

private:
    bool createU32Object(std::uint16_t index, unsigned int access,
                         std::uint32_t value, co_obj_t*& object) noexcept;
    bool createOctetObject(co_obj_t*& object) noexcept;
    void uninstallLocked() noexcept;

    EmcyTestMaster& master_; /**< Master whose local OD serves the objects. */
    co_obj_t* u32_object_ = nullptr; /**< Owned 0x2F00 object, if installed. */
    co_obj_t* octets_object_ = nullptr; /**< Owned 0x2F01 object, if installed. */
    co_obj_t* readonly_object_ = nullptr; /**< Owned 0x2F02 object, if installed. */
    bool installed_ = false; /**< True only after all three inserts succeed. */
};

#endif /* HOST_SDO_SERVER_FIXTURE_H */
