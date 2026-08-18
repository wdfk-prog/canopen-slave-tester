/**
 * @file
 * @brief Implements the J04/B03 master-local Server-SDO test fixture.
 */

#include "host_sdo_server_fixture.h"

#include "canopen_master.h"

#include <lely/co/dev.h>
#include <lely/co/obj.h>
#include <lely/util/errnum.h>
#include <lely/co/type.h>

#include <spdlog/spdlog.h>

#include <mutex>

HostSdoServerFixture::HostSdoServerFixture(EmcyTestMaster& master) noexcept
    : master_(master)
{
}

HostSdoServerFixture::~HostSdoServerFixture()
{
    uninstall();
}

bool HostSdoServerFixture::createU32Object(std::uint16_t index,
                                           unsigned int access,
                                           std::uint32_t value,
                                           co_obj_t*& object) noexcept
{
    co_obj_t* candidate = co_obj_create(index);
    if (candidate == nullptr) {
        return false;
    }

    co_sub_t* const sub = co_sub_create(kSubindex, CO_DEFTYPE_UNSIGNED32);
    if (sub == nullptr || co_sub_set_access(sub, access) == -1
        || co_obj_insert_sub(candidate, sub) == -1) {
        if (sub != nullptr && co_sub_get_obj(sub) == nullptr) {
            co_sub_destroy(sub);
        }
        co_obj_destroy(candidate);
        return false;
    }

    /* co_obj_insert_sub() allocates the current-value storage used by
     * co_sub_set_val_u32(); setting it before insertion dereferences a null
     * value slot in Lely's dynamic-object implementation. */
    if (co_sub_set_val_u32(sub, value) != sizeof(co_unsigned32_t)) {
        co_obj_destroy(candidate);
        return false;
    }

    object = candidate;
    return true;
}

bool HostSdoServerFixture::createOctetObject(co_obj_t*& object) noexcept
{
    co_obj_t* candidate = co_obj_create(kOctetsIndex);
    if (candidate == nullptr) {
        return false;
    }

    co_sub_t* const sub = co_sub_create(kSubindex, CO_DEFTYPE_OCTET_STRING);
    const std::uint8_t initial_value = kInitialOctet;
    if (sub == nullptr || co_sub_set_access(sub, CO_ACCESS_RW) == -1
        || co_obj_insert_sub(candidate, sub) == -1) {
        if (sub != nullptr && co_sub_get_obj(sub) == nullptr) {
            co_sub_destroy(sub);
        }
        co_obj_destroy(candidate);
        return false;
    }

    /* Dynamic sub-object value storage exists only after insertion. */
    if (co_sub_set_val(sub, &initial_value, sizeof(initial_value))
        != sizeof(initial_value)) {
        co_obj_destroy(candidate);
        return false;
    }

    object = candidate;
    return true;
}

bool HostSdoServerFixture::install() noexcept
{
    std::lock_guard<lely::util::BasicLockable> lock(master_);
    if (installed_) {
        return true;
    }

    co_dev_t* const dev = master_.localDevice();
    if (dev == nullptr || master_.localServerSdo() == nullptr) {
        spdlog::error("B03 fixture preflight failed: master SSDO #1 is unavailable");
        return false;
    }
    if (co_dev_find_obj(dev, kU32Index) != nullptr
        || co_dev_find_obj(dev, kOctetsIndex) != nullptr
        || co_dev_find_obj(dev, kReadOnlyIndex) != nullptr) {
        spdlog::error("B03 fixture object range 0x2F00..0x2F02 is already in use");
        return false;
    }

    if (!createU32Object(kU32Index, CO_ACCESS_RW, kInitialU32, u32_object_)
        || !createOctetObject(octets_object_)
        || !createU32Object(kReadOnlyIndex, CO_ACCESS_RO, kReadOnlyValue,
                            readonly_object_)
        || co_dev_insert_obj(dev, u32_object_) == -1
        || co_dev_insert_obj(dev, octets_object_) == -1
        || co_dev_insert_obj(dev, readonly_object_) == -1) {
        spdlog::error("B03 fixture installation failed");
        uninstallLocked();
        return false;
    }

    installed_ = true;
    spdlog::info("B03 master SSDO fixture installed at 0x2F00..0x2F02");
    return true;
}

void HostSdoServerFixture::uninstallLocked() noexcept
{
    if (readonly_object_ != nullptr) {
        co_obj_destroy(readonly_object_);
        readonly_object_ = nullptr;
    }
    if (octets_object_ != nullptr) {
        co_obj_destroy(octets_object_);
        octets_object_ = nullptr;
    }
    if (u32_object_ != nullptr) {
        co_obj_destroy(u32_object_);
        u32_object_ = nullptr;
    }
    installed_ = false;
}

void HostSdoServerFixture::uninstall() noexcept
{
    std::lock_guard<lely::util::BasicLockable> lock(master_);
    uninstallLocked();
}

bool HostSdoServerFixture::readU32(std::uint32_t& value) const noexcept
{
    std::lock_guard<lely::util::BasicLockable> lock(master_);
    if (!installed_) {
        return false;
    }
    co_dev_t* const dev = master_.localDevice();
    co_sub_t* const sub = dev != nullptr
        ? co_dev_find_sub(dev, kU32Index, kSubindex) : nullptr;
    if (sub == nullptr) {
        return false;
    }
    value = co_sub_get_val_u32(sub);
    return true;
}

bool HostSdoServerFixture::writeU32(std::uint32_t value) noexcept
{
    std::lock_guard<lely::util::BasicLockable> lock(master_);
    if (!installed_) {
        return false;
    }
    co_dev_t* const dev = master_.localDevice();
    co_sub_t* const sub = dev != nullptr
        ? co_dev_find_sub(dev, kU32Index, kSubindex) : nullptr;
    return sub != nullptr
        && co_sub_set_val_u32(sub, value) == sizeof(co_unsigned32_t);
}

bool HostSdoServerFixture::readOctets(std::vector<std::uint8_t>& value) const
{
    std::lock_guard<lely::util::BasicLockable> lock(master_);
    if (!installed_) {
        return false;
    }
    co_dev_t* const dev = master_.localDevice();
    co_sub_t* const sub = dev != nullptr
        ? co_dev_find_sub(dev, kOctetsIndex, kSubindex) : nullptr;
    if (sub == nullptr) {
        return false;
    }

    const std::size_t size = co_sub_sizeof_val(sub);
    const auto* const data = static_cast<const std::uint8_t*>(
        co_sub_addressof_val(sub));
    if (size == 0U) {
        value.clear();
        return true;
    }
    if (data == nullptr) {
        return false;
    }
    value.assign(data, data + size);
    return true;
}

bool HostSdoServerFixture::writeOctets(
    const std::vector<std::uint8_t>& value) noexcept
{
    std::lock_guard<lely::util::BasicLockable> lock(master_);
    if (!installed_) {
        return false;
    }
    co_dev_t* const dev = master_.localDevice();
    co_sub_t* const sub = dev != nullptr
        ? co_dev_find_sub(dev, kOctetsIndex, kSubindex) : nullptr;
    if (sub == nullptr) {
        return false;
    }
    set_errc(0);
    const size_t written = co_sub_set_val(
        sub, value.empty() ? nullptr : value.data(), value.size());
    return written == value.size() && (written != 0U || get_errc() == 0);
}
