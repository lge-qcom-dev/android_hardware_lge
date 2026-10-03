// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <mutex>

#include "fingerprint.h"

namespace lge::fingerprint {
class Session;

class LegacyHal {
  public:
    LegacyHal();
    ~LegacyHal();
    fingerprint_device_t* device() const { return mDevice; }
    void setSession(const std::shared_ptr<Session>& session);
    bool scan(bool enabled);

  private:
    static void notify(const fingerprint_msg_t* message);
    static std::mutex sMutex;
    static LegacyHal* sInstance;
    std::weak_ptr<Session> mSession;
    fingerprint_device_t* mDevice = nullptr;
};

}  // namespace lge::fingerprint
