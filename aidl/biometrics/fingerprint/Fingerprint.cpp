// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#include "Fingerprint.h"

namespace lge::fingerprint {

Fingerprint::Fingerprint(Config config)
    : mConfig(std::move(config)),
      mHal(std::make_shared<LegacyHal>()),
      mWorker(std::make_shared<Worker>()),
      mLockouts(std::make_shared<Lockouts>()) {}

ScopedAStatus Fingerprint::getSensorProps(std::vector<fp::SensorProps>* out) {
    fp::SensorProps props;
    props.commonProps.sensorId = mConfig.sensorId;
    props.commonProps.sensorStrength = common::SensorStrength::STRONG;
    props.commonProps.maxEnrollmentsPerUser = mConfig.maxEnrollments;
    props.sensorType = fp::FingerprintSensorType::UNDER_DISPLAY_OPTICAL;
    fp::SensorLocation location;
    location.sensorLocationX = mConfig.x;
    location.sensorLocationY = mConfig.y;
    location.sensorRadius = mConfig.radius;
    props.sensorLocations = {location};
    props.supportsNavigationGestures = false;
    props.supportsDetectInteraction = false;
    props.halHandlesDisplayTouches = false;
    // Reuse SystemUI's illumination surface and its UI-ready lifecycle signal.
    props.halControlsIllumination = false;
    *out = {props};
    return ScopedAStatus::ok();
}

ScopedAStatus Fingerprint::createSession(int32_t sensorId, int32_t userId,
                                         const std::shared_ptr<fp::ISessionCallback>& callback,
                                         std::shared_ptr<fp::ISession>* out) {
    if (sensorId != mConfig.sensorId || userId < 0 || !callback) {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard lock(mSessionMutex);
    if (mSession && !mSession->closed()) {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    auto session =
            ndk::SharedRefBase::make<Session>(mHal, mWorker, mLockouts, mConfig, userId, callback);
    mHal->setSession(session);
    if (session->initialize() != STATUS_OK) {
        session->close();
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    mSession = session;
    *out = session;
    return ScopedAStatus::ok();
}

}  // namespace lge::fingerprint
