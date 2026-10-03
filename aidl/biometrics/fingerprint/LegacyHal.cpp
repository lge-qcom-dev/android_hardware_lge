// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#include "LegacyHal.h"

#include <android-base/logging.h>
#include <cstddef>

#include "Session.h"

namespace lge::fingerprint {

static_assert(offsetof(fingerprint_device_t, do_extra_api_in) == 0xe0,
              "The Egis extended device ABI must match the 64-bit vendor module");

std::mutex LegacyHal::sMutex;
LegacyHal* LegacyHal::sInstance = nullptr;

LegacyHal::LegacyHal() {
    const hw_module_t* module = nullptr;
    CHECK_EQ(hw_get_module(FINGERPRINT_HARDWARE_MODULE_ID, &module), 0);
    CHECK(module && module->methods && module->methods->open);
    hw_device_t* device = nullptr;
    CHECK_EQ(module->methods->open(module, nullptr, &device), 0);
    CHECK(device);
    CHECK_EQ(device->version, static_cast<uint32_t>(HARDWARE_MODULE_API_VERSION(2, 1)));
    mDevice = reinterpret_cast<fingerprint_device_t*>(device);
    CHECK(mDevice->set_notify && mDevice->pre_enroll && mDevice->post_enroll && mDevice->enroll &&
          mDevice->authenticate && mDevice->cancel && mDevice->enumerate && mDevice->remove &&
          mDevice->set_active_group && mDevice->get_authenticator_id && mDevice->do_extra_api_in);
    {
        std::lock_guard lock(sMutex);
        CHECK(!sInstance);
        sInstance = this;
    }
    CHECK_EQ(mDevice->set_notify(mDevice, notify), 0);
}

LegacyHal::~LegacyHal() {
    {
        std::lock_guard lock(sMutex);
        sInstance = nullptr;
    }
    mDevice->common.close(&mDevice->common);
}

void LegacyHal::setSession(const std::shared_ptr<Session>& session) {
    std::lock_guard lock(sMutex);
    mSession = session;
}

void LegacyHal::notify(const fingerprint_msg_t* message) {
    if (!message) return;
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(sMutex);
        if (sInstance) session = sInstance->mSession.lock();
    }
    if (session) session->notify(*message);
}

bool LegacyHal::scan(bool enabled) {
    uint32_t param = 0;
    const auto command = enabled ? FINGERPRINT_LGE_SCAN_START : FINGERPRINT_LGE_SCAN_STOP;
    const int result = mDevice->do_extra_api_in(command, &param);
    if (result) LOG(ERROR) << "Egis scan control failed: " << result;
    return result == 0;
}

}  // namespace lge::fingerprint
