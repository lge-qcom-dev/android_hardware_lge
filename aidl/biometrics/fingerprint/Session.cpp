/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <thread>

#include <android-base/file.h>
#include <android-base/strings.h>

#include "Legacy2Aidl.h"
#include "Session.h"

#include "CancellationSignal.h"

#define FOD_HBM_LEGACY_PATH "/sys/devices/virtual/panel/brightness/fp_lhbm"
#define FOD_HBM_PATH "/sys/devices/virtual/panel/panel-0/brightness/fp_lhbm"
#define LGE_TOUCH_RESET_PATH "/sys/devices/virtual/input/lge_touch/reset_ctrl"

namespace aidl::android::hardware::biometrics::fingerprint {

void onClientDeath(void* cookie) {
    ALOGI("FingerprintService has died");
    Session* session = static_cast<Session*>(cookie);
    if (session && !session->isClosed()) {
        session->close();
    }
}

Session::Session(rbs_fingerprint_device_t* device, int userId, std::shared_ptr<ISessionCallback> cb,
                 LockoutTracker lockoutTracker)
    : mDevice(device), mLockoutTracker(lockoutTracker), mUserId(userId), mCb(cb) {
    mDeathRecipient = AIBinder_DeathRecipient_new(onClientDeath);

    auto path = std::format("/data/vendor_de/{}/fpdata/", userId);
    strncpy(mDevice->g_custom_ini_path, path.c_str(), RBS_CUSTOM_INI_PATH_SIZE);

    mDevice->rbs_active_user_group(userId, path.c_str());
}

ndk::ScopedAStatus Session::generateChallenge() {
    uint64_t challenge;
    mDevice->rbs_get_challenge(&challenge);
    ALOGI("generateChallenge: %ld", challenge);
    mCb->onChallengeGenerated(challenge);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    ALOGI("revokeChallenge: %ld", challenge);
    uint64_t mChallenge = (uint64_t)challenge;
    mDevice->rbs_post_challenge(&mChallenge);
    mCb->onChallengeRevoked(challenge);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enroll(const HardwareAuthToken& hat,
                                   std::shared_ptr<ICancellationSignal>* out) {
    int rc = 0;
    int seed = 0;
    int pre_enroll_rc = 0;
    hw_auth_token_t authToken;
    translate(hat, authToken);

    rc = mDevice->rbs_chk_auth_token(&authToken, sizeof(hw_auth_token_t));
    if (rc != 0) {
        ALOGE("Auth token check failed, error %d", rc);
        goto fail_enroll;
    }

    rc = mDevice->rbs_chk_secure_id(mUserId, authToken.user_id);
    if (rc != 0) {
        ALOGD("Secure ID check failed, error %d", rc);
        if (rc != 0x21) {
            goto fail_enroll;
        }
        rc = mDevice->rbs_remove_fingerprint(mUserId, 0);
        if (rc == 0) {
            // After nuking everything, check if Secure ID is okay again
            rc = mDevice->rbs_chk_secure_id(mUserId, authToken.user_id);
            if (rc == 0) {
                ALOGD("Removed all fingerprints and secure ID check OK");
                goto continue_enroll;
            } else {
                ALOGD("Secure ID check failed, error %d", rc);
                goto fail_enroll;
            }
        } else {
            ALOGE("Remove all fingerprints failed, error %d", rc);
            goto fail_enroll;
        }
    }

continue_enroll:
    do {
        seed = rand();
        pre_enroll_rc = mDevice->rbs_pre_enroll(mUserId, seed);
        if (pre_enroll_rc == 0) {
            uint32_t rbs_param[8] = {};
            uint32_t rbs_param_size = sizeof(rbs_param);
            uint32_t mTimeoutSec = 60;
            mDevice->rbs_extra_api(7, &mTimeoutSec, 4, rbs_param, &rbs_param_size);
            rc = mDevice->rbs_enroll();
            if (rc == 0) goto end_enroll;
            mCb->onError(Error::CANCELED, 0);
            if (rc != 4) goto end_enroll;
            mCb->onError(Error::UNABLE_TO_PROCESS, 0);
            goto end_enroll;
        }
    } while (pre_enroll_rc != 11);
    ALOGE("Pre-enroll failed, no space");

fail_enroll:
    mCb->onError(Error::UNABLE_TO_PROCESS, rc);
    *out = SharedRefBase::make<CancellationSignal>(this);

end_enroll:
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<ICancellationSignal>* out) {
    checkSensorLockout();

    int rc = mDevice->rbs_authenticator(mUserId, 0, 0, operationId);
    if (rc != 0) {
        if (rc == 4) {
            mCb->onError(Error::HW_UNAVAILABLE, rc);
        }
        else
            mCb->onError(Error::CANCELED, rc);
    }

    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<ICancellationSignal>* out) {
    ALOGD("Detect interaction is not supported");
    mCb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorCode */);

    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    int rc = 0;
    uint32_t num_fids = 0;
    uint32_t fids[5] = {};
    std::vector<int32_t> callbackFids;
    if (mUserId == 9999) {
        ALOGE("User ID empty");
        return ndk::ScopedAStatus::fromServiceSpecificError(EINVAL);
    }

    rc = mDevice->rbs_get_fingerprint_ids(mUserId, fids, &num_fids);
    if (rc != 0) {
        ALOGE("Enumerate failed, error %d", rc);
        return ndk::ScopedAStatus::fromServiceSpecificError(rc);
    }

    for(size_t i = 0; i < 5; i++)
        callbackFids.push_back(static_cast<int32_t>(fids[i]));

    mCb->onEnrollmentsEnumerated(callbackFids);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& enrollmentIds) {
    int rc = 0;
    uint32_t num_fids = 0;
    uint32_t fids[5] = {};

    // Get a list of fingerprints
    rc = mDevice->rbs_get_fingerprint_ids(mUserId, fids, &num_fids);
    if (rc != 0) {
        mCb->onError(Error::UNABLE_TO_REMOVE, rc);
        return ndk::ScopedAStatus::ok();
    }

    // Check if there even are fingerprints to remove
    if (num_fids == 0) {
        ALOGD("No fingerprints registered");
        mCb->onEnrollmentsRemoved(enrollmentIds);
        return ndk::ScopedAStatus::ok();
    }

    /*
        Remove the fingerprints.
        It seems that the RBS API supports deleting all fingerprints if the FID is 0, so we don't
        need to manually loop here.
        We do need to manually loop the callbacks, however.
    */
    // If instructed to invalidate all, let's invalidate all
    if (enrollmentIds.size() == 5) {
        rc = mDevice->rbs_remove_fingerprint(mUserId, 0);
        if (rc == 0) {
            // Notify framework that all fingerprints are gone
            mCb->onEnrollmentsRemoved(enrollmentIds);
            return ndk::ScopedAStatus::ok();
        } else
            ALOGE("remove all failed: %d. Trying manually", rc);
    }

    // If invalidate all fails, or we are only removing a handful, do them one by one
    for (int32_t fid : enrollmentIds) {
        rc = mDevice->rbs_remove_fingerprint(mUserId, fid);
        if (rc) {
            ALOGE("remove failed: %d", rc);
            mCb->onError(Error::UNABLE_TO_REMOVE, rc);
        }
    }

    /*
        Interestingly on stock, removal can fail, but it will send a notify call that the
        fingerprint was removed anyway.
        The only explanation for this is that instead of having a dangling fingerprint where it may
        or may not have been successfully removed, it instructs the framework to invalidate the
        fingerprint anyway, just in case.
    */
    mCb->onEnrollmentsRemoved(enrollmentIds);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    uint64_t auth_id;
    mDevice->rbs_get_authenticator_id(&auth_id);
    ALOGI("getAuthenticatorId: %ld", auth_id);
    mCb->onAuthenticatorIdRetrieved(auth_id);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    uint64_t auth_id;
    mDevice->rbs_get_authenticator_id(&auth_id);
    ALOGI("invalidateAuthenticatorId: %ld", auth_id);
    mCb->onAuthenticatorIdInvalidated(auth_id);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const HardwareAuthToken& /*hat*/) {
    clearLockout(true);
    if (mIsLockoutTimerStarted) mIsLockoutTimerAborted = true;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerDown(int32_t /*pointerId*/, int32_t /*x*/, int32_t /*y*/,
                                          float /*minor*/, float /*major*/) {
    uint32_t param = 0x65;
    uint32_t rbs_param[8] = {};
    uint32_t rbs_param_size = sizeof(rbs_param);

    if (hbmFodEnabled) return ndk::ScopedAStatus::ok();

    mDevice->rbs_extra_api(7, &param, 4, rbs_param, &rbs_param_size);

    setFodHbm(true);
    resetLgeTouchPanel();

    hbmFodEnabled = true;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerUp(int32_t /*pointerId*/) {
    std::lock_guard<std::mutex> lock(mSetHbmFodMutex);
    uint32_t param = 0x66;
    uint32_t rbs_param[8] = {};
    uint32_t rbs_param_size = sizeof(rbs_param);

    if (!hbmFodEnabled) return ndk::ScopedAStatus::ok();

    mDevice->rbs_extra_api(7, &param, 4, rbs_param, &rbs_param_size);

    setFodHbm(false);

    hbmFodEnabled = false;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onUiReady() {
    // TODO: stub

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(
        int64_t operationId, const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return authenticate(operationId, out);
}

ndk::ScopedAStatus Session::enrollWithContext(const keymaster::HardwareAuthToken& hat,
                                              const common::OperationContext& /*context*/,
                                              std::shared_ptr<common::ICancellationSignal>* out) {
    return enroll(hat, out);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
        const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onPointerDownWithContext(const PointerContext& context) {
    return onPointerDown(context.pointerId, context.x, context.y, context.minor, context.major);
}

ndk::ScopedAStatus Session::onPointerUpWithContext(const PointerContext& context) {
    return onPointerUp(context.pointerId);
}

ndk::ScopedAStatus Session::onContextChanged(const common::OperationContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerCancelWithContext(const PointerContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setIgnoreDisplayTouches(bool /*shouldIgnore*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::cancel() {
    if (hbmFodEnabled) {
        onPointerUp(0);
    }

    int ret = mDevice->rbs_cancel();

    if (ret == 0) {
        mCb->onError(Error::CANCELED, 0 /* vendorCode */);
        return ndk::ScopedAStatus::ok();
    }

    return ndk::ScopedAStatus::fromServiceSpecificError(ret);
}

ndk::ScopedAStatus Session::close() {
    mClosed = true;
    mCb->onSessionClosed();
    AIBinder_DeathRecipient_delete(mDeathRecipient);
    return ndk::ScopedAStatus::ok();
}

binder_status_t Session::linkToDeath(AIBinder* binder) {
    return AIBinder_linkToDeath(binder, mDeathRecipient, this);
}

bool Session::isClosed() {
    return mClosed;
}

bool Session::checkSensorLockout() {
    LockoutTracker::LockoutMode lockoutMode = mLockoutTracker.getMode();
    if (lockoutMode == LockoutTracker::LockoutMode::kPermanent) {
        ALOGE("Fail: lockout permanent");
        mCb->onLockoutPermanent();
        mIsLockoutTimerAborted = true;
        return true;
    }
    if (lockoutMode == LockoutTracker::LockoutMode::kTimed) {
        int64_t timeLeft = mLockoutTracker.getLockoutTimeLeft();
        ALOGE("Fail: lockout timed: %ld", timeLeft);
        mCb->onLockoutTimed(timeLeft);
        if (!mIsLockoutTimerStarted) startLockoutTimer(timeLeft);
        return true;
    }
    return false;
}

void Session::clearLockout(bool clearAttemptCounter) {
    mLockoutTracker.reset(clearAttemptCounter);
    mCb->onLockoutCleared();
}

void Session::startLockoutTimer(int64_t timeout) {
    std::function<void()> action = std::bind(&Session::lockoutTimerExpired, this);
    std::thread([timeout, action]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        action();
    }).detach();

    mIsLockoutTimerStarted = true;
}

void Session::lockoutTimerExpired() {
    if (!mIsLockoutTimerAborted) clearLockout(false);

    mIsLockoutTimerStarted = false;
    mIsLockoutTimerAborted = false;
}

void Session::notify(uint32_t eventId, uint32_t value1, uint32_t value2, void* buffer,
                                   uint32_t buffer_size) {
    switch (eventId) {
        // Error
        case 0x3eb:
        case 0x401: {
            ALOGD("onError(%hhd, %d)", Error::CANCELED, eventId);
            mCb->onError(Error::CANCELED, eventId);
        } break;
        case 0x40e: {
            ALOGD("onError(%hhd, %d)", Error::TIMEOUT, eventId);
            mCb->onError(Error::TIMEOUT, 0);
        } break;
        // Acquired
        case 0x3ec:
        case 0x3ed: {
            ALOGD("onAcquired(%hhd, %d)", AcquiredInfo::TOO_SLOW, eventId);
            mCb->onAcquired(AcquiredInfo::TOO_SLOW, eventId);
        } break;
        case 0x3ee:
        case 0x3ef: {
            ALOGD("onAcquired(%hhd, %d)", AcquiredInfo::VENDOR, eventId);
            mCb->onAcquired(AcquiredInfo::VENDOR, eventId);
        } break;
        case 0x3f5: {
            ALOGD("onAcquired(%hhd, %d)", AcquiredInfo::INSUFFICIENT, eventId);
            mCb->onAcquired(AcquiredInfo::INSUFFICIENT, eventId);
        } break;
        case 0x3f7:
        case 0x3f8: {
            ALOGD("onAcquired(%hhd, %d)", AcquiredInfo::PARTIAL, eventId);
            mCb->onAcquired(AcquiredInfo::PARTIAL, eventId);
        } break;
        case 0x3f9:
        case 0x3fa:
        case 0x3fb: {
            ALOGD("onAcquired(%hhd, %d)", AcquiredInfo::TOO_FAST, eventId);
            mCb->onAcquired(AcquiredInfo::TOO_FAST, eventId);
        } break;
        case 0x3fe: {
            ALOGD("onAcquired(%hhd, %d)", AcquiredInfo::GOOD, eventId);
            mCb->onAcquired(AcquiredInfo::GOOD, eventId);
        } break;
        // Enrolling
        case 0x40d: {
            uint32_t fid = value1;
            uint32_t samples_remaining = value2;
            ALOGD("onEnrollmentProgress(fid=%d, rem=%d)", fid, samples_remaining);
            mCb->onEnrollmentProgress(fid, samples_remaining);
        } break;
        // Authenticated
        case 0x3f2:
        case 0x3f3: {
            uint32_t gid = value1;
            uint32_t fid = value2;
            ALOGD("onAuthenticated(fid=%d, gid=%d)", fid, gid);
            if (fid != 0) {
                HardwareAuthToken authToken;
                const hw_auth_token_t* hat = reinterpret_cast<const hw_auth_token_t*>(buffer);
                translate(*hat, authToken);

                mCb->onAuthenticationSucceeded(fid, authToken);
                mLockoutTracker.reset(true);
            } else {
                // Not a recognized fingerprint
                mCb->onAuthenticationFailed();
                mLockoutTracker.addFailedAttempt();
                checkSensorLockout();
            }
            break;
        }
    }
    // Disable FOD
    if (hbmFodEnabled) {
        onPointerUp(0);
    }
}

void Session::setFodHbm(bool status) {
    ::android::base::WriteStringToFile(status ? "1" : "0", mHbmPath);
}

void Session::resetLgeTouchPanel(void) {
    ::android::base::WriteStringToFile("4", LGE_TOUCH_RESET_PATH);
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
