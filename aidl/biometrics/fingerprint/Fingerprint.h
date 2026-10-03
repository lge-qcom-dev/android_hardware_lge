// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>

#include "Session.h"

namespace lge::fingerprint {

class Fingerprint : public fp::BnFingerprint {
  public:
    explicit Fingerprint(Config config);
    ScopedAStatus getSensorProps(std::vector<fp::SensorProps>* out) override;
    ScopedAStatus createSession(int32_t sensorId, int32_t userId,
                                const std::shared_ptr<fp::ISessionCallback>& callback,
                                std::shared_ptr<fp::ISession>* out) override;

  private:
    const Config mConfig;
    std::shared_ptr<LegacyHal> mHal;
    std::shared_ptr<Worker> mWorker;
    std::shared_ptr<Lockouts> mLockouts;
    std::mutex mSessionMutex;
    std::shared_ptr<Session> mSession;
};

}  // namespace lge::fingerprint
